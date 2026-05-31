#include "orchestration/process_manager.h"

#include "orchestration/sqlite_helpers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace orchestration {

namespace {

std::string sanitizeTag(std::string value) {
    for (char& ch : value) {
        const bool keep = (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-';
        if (!keep) {
            ch = '_';
        }
    }
    if (value.empty()) {
        return "process";
    }
    return value;
}

std::string commandTag(const std::vector<std::string>& cmd) {
    if (cmd.empty()) {
        return "empty_cmd";
    }
    std::filesystem::path exe(cmd.front());
    return sanitizeTag(exe.stem().string());
}

std::string upperAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool isSensitiveEnvName(const std::string& name) {
    const std::string upper = upperAscii(name);
    const char* markers[] = {
        "SECRET",
        "TOKEN",
        "PASSWORD",
        "PASSWD",
        "PASSPHRASE",
        "PRIVATE",
        "CREDENTIAL",
        "COOKIE",
        "AUTH",
        "API_KEY",
        "ACCESS_KEY",
    };
    for (const char* marker : markers) {
        if (upper.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool isAllowedEnvName(const std::string& name) {
    if (isSensitiveEnvName(name)) {
        return false;
    }
    const std::string upper = upperAscii(name);
    return upper == "PATH" ||
        upper == "PATHEXT" ||
        upper == "SYSTEMROOT" ||
        upper == "WINDIR" ||
        upper == "COMSPEC" ||
        upper == "TEMP" ||
        upper == "TMP" ||
        upper == "TMPDIR" ||
        upper == "HOME" ||
        upper == "USERPROFILE" ||
        upper == "LANG" ||
        upper == "TZ" ||
        upper == "VIRTUAL_ENV" ||
        upper == "PYTHONPATH" ||
        upper.rfind("LC_", 0) == 0;
}

std::string envNameFromEntry(const std::string& entry) {
    const size_t pos = entry.find('=');
    if (pos == std::string::npos || pos == 0) {
        return {};
    }
    return entry.substr(0, pos);
}

std::string redactSensitiveLogText(std::string text) {
    const char* markers[] = {
        "AUTHORIZATION",
        "API_SECRET",
        "API_KEY",
        "ACCESS_KEY",
        "SECRET",
        "TOKEN",
        "PASSWORD",
        "PASSWD",
        "PASSPHRASE",
        "PRIVATE_KEY",
        "CREDENTIAL",
    };
    for (const char* marker : markers) {
        size_t searchFrom = 0;
        while (searchFrom < text.size()) {
            const std::string upper = upperAscii(text);
            const size_t markerPos = upper.find(marker, searchFrom);
            if (markerPos == std::string::npos) {
                break;
            }
            const size_t delim = text.find_first_of("=:", markerPos);
            if (delim == std::string::npos) {
                searchFrom = markerPos + 1;
                continue;
            }
            const size_t nextMarkerBoundary = markerPos + std::strlen(marker) + 32;
            if (delim > nextMarkerBoundary) {
                searchFrom = markerPos + 1;
                continue;
            }
            size_t valueStart = delim + 1;
            while (valueStart < text.size() &&
                   (text[valueStart] == ' ' || text[valueStart] == '\t' || text[valueStart] == '"' ||
                    text[valueStart] == '\'')) {
                ++valueStart;
            }
            size_t valueEnd = valueStart;
            while (valueEnd < text.size() && text[valueEnd] != '\r' && text[valueEnd] != '\n') {
                ++valueEnd;
            }
            text.replace(valueStart, valueEnd - valueStart, "[REDACTED]");
            searchFrom = valueStart + 10;
        }
    }
    return text;
}

std::string localDate() {
    const auto now = std::time(nullptr);
    std::tm tmNow{};
#if defined(_WIN32)
    localtime_s(&tmNow, &now);
#else
    localtime_r(&now, &tmNow);
#endif
    std::ostringstream out;
    out << std::put_time(&tmNow, "%Y-%m-%d");
    return out.str();
}

#if defined(_WIN32)
std::string quoteWindowsArg(const std::string& arg) {
    if (arg.empty()) {
        return "\"\"";
    }
    const bool needsQuote = arg.find_first_of(" \t\"") != std::string::npos;
    if (!needsQuote) {
        return arg;
    }
    std::string out = "\"";
    size_t backslashes = 0;
    for (char ch : arg) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            out.append(backslashes, '\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }
    if (backslashes > 0) {
        out.append(backslashes * 2, '\\');
    }
    out.push_back('"');
    return out;
}

std::string buildWindowsCmdline(const std::vector<std::string>& cmd) {
    std::ostringstream out;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i > 0) {
            out << ' ';
        }
        out << quoteWindowsArg(cmd[i]);
    }
    return out.str();
}

std::vector<char> buildSanitizedEnvBlock() {
    LPCH rawEnv = GetEnvironmentStringsA();
    if (!rawEnv) {
        return {'\0', '\0'};
    }
    std::vector<std::string> entries;
    for (LPCH cursor = rawEnv; *cursor != '\0';) {
        const std::string entry(cursor);
        cursor += entry.size() + 1;
        const std::string name = envNameFromEntry(entry);
        if (!name.empty() && name.front() != '=' && isAllowedEnvName(name)) {
            entries.push_back(entry);
        }
    }
    FreeEnvironmentStringsA(rawEnv);
    std::sort(entries.begin(), entries.end(), [](const std::string& a, const std::string& b) {
        return upperAscii(envNameFromEntry(a)) < upperAscii(envNameFromEntry(b));
    });

    std::vector<char> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back('\0');
    }
    if (block.empty()) {
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}
#else
extern char** environ;

void installSanitizedEnvironmentNoexcept() noexcept {
    std::vector<std::string> kept;
    for (char** cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor) {
        const std::string entry(*cursor);
        const std::string name = envNameFromEntry(entry);
        if (!name.empty() && isAllowedEnvName(name)) {
            kept.push_back(entry);
        }
    }
    clearenv();
    for (const auto& entry : kept) {
        const size_t pos = entry.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        setenv(entry.substr(0, pos).c_str(), entry.substr(pos + 1).c_str(), 1);
    }
}

size_t cStringLength(const char* text) noexcept {
    if (text == nullptr) {
        return 0;
    }
    const char* cursor = text;
    while (*cursor != '\0') {
        ++cursor;
    }
    return static_cast<size_t>(cursor - text);
}

void writeAllNoexcept(int fd, const char* data, size_t size) noexcept {
    while (size > 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

void writeCStringNoexcept(int fd, const char* text) noexcept {
    writeAllNoexcept(fd, text, cStringLength(text));
}

void copyRedactedPipeToLog(int readFd, int logFd) {
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t bytesRead = ::read(readFd, buffer.data(), buffer.size());
        if (bytesRead > 0) {
            const std::string redacted =
                redactSensitiveLogText(std::string(buffer.data(), buffer.data() + bytesRead));
            writeAllNoexcept(logFd, redacted.data(), redacted.size());
            continue;
        }
        if (bytesRead < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(readFd);
    ::close(logFd);
}

void writeIntNoexcept(int fd, int value) noexcept {
    char buffer[16]{};
    size_t pos = sizeof(buffer);
    unsigned int remaining = 0;
    if (value < 0) {
        remaining = static_cast<unsigned int>(-(value + 1)) + 1U;
    } else {
        remaining = static_cast<unsigned int>(value);
    }
    do {
        buffer[--pos] = static_cast<char>('0' + (remaining % 10U));
        remaining /= 10U;
    } while (remaining > 0 && pos > 0);
    if (value < 0 && pos > 0) {
        buffer[--pos] = '-';
    }
    writeAllNoexcept(fd, buffer + pos, sizeof(buffer) - pos);
}

void writeExecFailureDiagnostic(int fd, int execErrno, const char* command) noexcept {
    writeCStringNoexcept(fd, "execvp failed errno=");
    writeIntNoexcept(fd, execErrno);
    writeCStringNoexcept(fd, " cmd=");
    writeCStringNoexcept(fd, command == nullptr ? "" : command);
    writeCStringNoexcept(fd, "\n");
}
#endif

} // namespace

ProcessManager::ProcessManager(ProcessManagerConfig config)
    : m_config(std::move(config)) {
    m_config.maxAttempts = (std::max)(1, m_config.maxAttempts);
    m_config.backoffBaseSeconds = (std::max)(0, m_config.backoffBaseSeconds);
    m_config.timeoutSeconds = (std::max)(1, m_config.timeoutSeconds);
    if (m_config.logDir.empty()) {
        m_config.logDir = "logs/qlib_orch";
    }
}

ProcessResult ProcessManager::spawnWithRetry(const std::vector<std::string>& cmd) {
    ProcessResult last;
    const std::string tag = commandTag(cmd);

    for (int attempt = 1; attempt <= m_config.maxAttempts; ++attempt) {
        const std::string logPath = makeLogPath(tag, attempt);
        last = spawnOnce(cmd, logPath);
        if (last.succeeded) {
            return last;
        }
        if (attempt >= m_config.maxAttempts) {
            break;
        }
        const int sleepSeconds = m_config.backoffBaseSeconds * (1 << (attempt - 1));
        if (sleepSeconds > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(sleepSeconds));
        }
    }

    return last;
}

std::string ProcessManager::makeLogPath(const std::string& tag, int attemptNumber) const {
    std::filesystem::path dir(m_config.logDir);
    std::filesystem::create_directories(dir);

    std::ostringstream name;
    name << localDate() << "_" << sanitizeTag(tag) << "_attempt" << attemptNumber << ".log";
    return (dir / name.str()).string();
}

ProcessResult ProcessManager::spawnOnce(const std::vector<std::string>& cmd, const std::string& logPath) {
    ProcessResult out;
    out.logPath = logPath;
    if (cmd.empty()) {
        std::ofstream(logPath, std::ios::trunc) << "empty command\n";
        return out;
    }

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        std::ofstream(logPath, std::ios::trunc) << "CreatePipe failed\n";
        return out;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cmdline = buildWindowsCmdline(cmd);
    std::vector<char> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back('\0');
    std::vector<char> envBlock = buildSanitizedEnvBlock();

    const DWORD creationFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    const BOOL started = CreateProcessA(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        creationFlags,
        envBlock.data(),
        nullptr,
        &si,
        &pi);

    if (!started) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        std::ofstream(logPath, std::ios::trunc) << "CreateProcess failed GetLastError=" << GetLastError() << "\n";
        return out;
    }

    HANDLE jobHandle = CreateJobObjectA(nullptr, nullptr);
    if (jobHandle) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                jobHandle,
                JobObjectExtendedLimitInformation,
                &jobInfo,
                sizeof(jobInfo)) ||
            !AssignProcessToJobObject(jobHandle, pi.hProcess)) {
            CloseHandle(jobHandle);
            jobHandle = nullptr;
        }
    }
    ResumeThread(pi.hThread);

    CloseHandle(writePipe);
    writePipe = nullptr;

    std::ofstream log(logPath, std::ios::binary | std::ios::trunc);
    std::thread reader([&log, readPipe]() {
        char buffer[4096];
        DWORD bytesRead = 0;
        while (ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr) && bytesRead > 0) {
            const std::string redacted = redactSensitiveLogText(std::string(buffer, buffer + bytesRead));
            log.write(redacted.data(), static_cast<std::streamsize>(redacted.size()));
        }
    });

    const DWORD waitMs = static_cast<DWORD>(m_config.timeoutSeconds * 1000);
    const DWORD waitResult = WaitForSingleObject(pi.hProcess, waitMs);
    if (waitResult == WAIT_TIMEOUT) {
        out.timedOut = true;
        if (jobHandle) {
            CloseHandle(jobHandle);
            jobHandle = nullptr;
        } else {
            TerminateProcess(pi.hProcess, 124);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    out.exitCode = static_cast<int>(exitCode);
    out.succeeded = !out.timedOut && out.exitCode == 0;

    if (jobHandle) {
        CloseHandle(jobHandle);
        jobHandle = nullptr;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (reader.joinable()) {
        reader.join();
    }
    CloseHandle(readPipe);

#else
    const std::filesystem::path logFile(logPath);
    if (logFile.has_parent_path()) {
        std::filesystem::create_directories(logFile.parent_path());
    }

    const int logFd = ::open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (logFd < 0) {
        std::ofstream(logPath, std::ios::trunc) << "open log file failed errno=" << errno << "\n";
        return out;
    }
    int pipeFd[2]{-1, -1};
    if (::pipe(pipeFd) != 0) {
        const int pipeErrno = errno;
        ::close(logFd);
        std::ofstream(logPath, std::ios::app) << "pipe failed errno=" << pipeErrno << "\n";
        return out;
    }

    std::vector<char*> argv;
    argv.reserve(cmd.size() + 1);
    for (const auto& arg : cmd) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        const int forkErrno = errno;
        ::close(pipeFd[0]);
        ::close(pipeFd[1]);
        ::close(logFd);
        std::ofstream(logPath, std::ios::app) << "fork failed errno=" << forkErrno << "\n";
        return out;
    }

    if (child == 0) {
        (void)::setpgid(0, 0);
        installSanitizedEnvironmentNoexcept();
        ::close(pipeFd[0]);
        (void)::dup2(pipeFd[1], STDOUT_FILENO);
        (void)::dup2(pipeFd[1], STDERR_FILENO);
        ::close(pipeFd[1]);
        ::close(logFd);

        ::execvp(argv[0], argv.data());
        const int execErrno = errno;
        writeExecFailureDiagnostic(STDERR_FILENO, execErrno, argv[0]);
        _exit(127);
    }

    ::close(pipeFd[1]);
    std::thread reader([readFd = pipeFd[0], logFd]() {
        copyRedactedPipeToLog(readFd, logFd);
    });

    int status = 0;
    bool childExited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(m_config.timeoutSeconds);
    while (!childExited) {
        const pid_t waitResult = ::waitpid(child, &status, WNOHANG);
        if (waitResult == child) {
            childExited = true;
            break;
        }
        if (waitResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            out.exitCode = -1;
            if (reader.joinable()) {
                reader.join();
            }
            std::ofstream(logPath, std::ios::app) << "waitpid failed errno=" << errno << "\n";
            return out;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            out.timedOut = true;
            (void)::kill(-child, SIGKILL);
            pid_t killedWait = 0;
            do {
                killedWait = ::waitpid(child, &status, 0);
            } while (killedWait < 0 && errno == EINTR);
            childExited = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (out.timedOut) {
        out.exitCode = 124;
        if (reader.joinable()) {
            reader.join();
        }
        std::ofstream(logPath, std::ios::app)
            << "\n[process_manager] timed out after " << m_config.timeoutSeconds << " seconds\n";
    } else if (WIFEXITED(status)) {
        out.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out.exitCode = 128 + WTERMSIG(status);
    } else {
        out.exitCode = -1;
    }
    if (reader.joinable()) {
        reader.join();
    }
    out.succeeded = !out.timedOut && out.exitCode == 0;
#endif

    return out;
}

ProcessManager::~ProcessManager() {
    std::vector<std::string> daemons;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [name, _] : m_daemons) {
            daemons.push_back(name);
        }
    }
    for (const auto& name : daemons) {
        stopDaemon(name);
    }
}

bool ProcessManager::startDaemon(const std::string& name, const std::vector<std::string>& cmd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_daemons.count(name)) {
        return true;
    }
    if (cmd.empty()) return false;
    
    std::string logPath = makeLogPath(name + "_daemon", 1);
    
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cmdline = buildWindowsCmdline(cmd);
    std::vector<char> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back('\0');
    std::vector<char> envBlock = buildSanitizedEnvBlock();

    const DWORD creationFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    const BOOL started = CreateProcessA(
        nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, creationFlags, envBlock.data(), nullptr, &si, &pi);

    if (!started) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }

    HANDLE jobHandle = CreateJobObjectA(nullptr, nullptr);
    if (jobHandle) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo{};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(jobHandle, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo)) ||
            !AssignProcessToJobObject(jobHandle, pi.hProcess)) {
            CloseHandle(jobHandle);
            jobHandle = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(writePipe);

    std::thread([logPath, readPipe]() {
        std::ofstream log(logPath, std::ios::binary | std::ios::trunc);
        char buffer[4096];
        DWORD bytesRead = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
            const std::string redacted = redactSensitiveLogText(std::string(buffer, buffer + bytesRead));
            log.write(redacted.data(), static_cast<std::streamsize>(redacted.size()));
        }
        CloseHandle(readPipe);
    }).detach();

    m_daemons[name] = { pi.hProcess, jobHandle };
    return true;
#else
    const std::filesystem::path logFile(logPath);
    if (logFile.has_parent_path()) {
        std::filesystem::create_directories(logFile.parent_path());
    }

    const int logFd = ::open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (logFd < 0) return false;
    int pipeFd[2]{-1, -1};
    if (::pipe(pipeFd) != 0) {
        ::close(logFd);
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(cmd.size() + 1);
    for (const auto& arg : cmd) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipeFd[0]);
        ::close(pipeFd[1]);
        ::close(logFd);
        return false;
    }
    if (child == 0) {
        (void)::setpgid(0, 0);
        installSanitizedEnvironmentNoexcept();
        ::close(pipeFd[0]);
        ::dup2(pipeFd[1], STDOUT_FILENO);
        ::dup2(pipeFd[1], STDERR_FILENO);
        ::close(pipeFd[1]);
        ::close(logFd);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(pipeFd[1]);
    std::thread([readFd = pipeFd[0], logFd]() {
        copyRedactedPipeToLog(readFd, logFd);
    }).detach();
    m_daemons[name] = { child };
    return true;
#endif
}

bool ProcessManager::isDaemonRunning(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_daemons.find(name);
    if (it == m_daemons.end()) return false;
    
#if defined(_WIN32)
    const DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(it->second.hProcess), 0);
    if (waitResult == WAIT_TIMEOUT) {
        return true;
    }
    if (waitResult == WAIT_FAILED) {
        return true;
    }
    if (it->second.jobHandle) CloseHandle(static_cast<HANDLE>(it->second.jobHandle));
    CloseHandle(static_cast<HANDLE>(it->second.hProcess));
    m_daemons.erase(it);
    return false;
#else
    int status = 0;
    pid_t res = ::waitpid(it->second.pid, &status, WNOHANG);
    if (res == 0) {
        return true;
    }
    m_daemons.erase(it);
    return false;
#endif
}

void ProcessManager::stopDaemon(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_daemons.find(name);
    if (it == m_daemons.end()) return;

#if defined(_WIN32)
    if (it->second.jobHandle) {
        CloseHandle(static_cast<HANDLE>(it->second.jobHandle));
    } else {
        TerminateProcess(static_cast<HANDLE>(it->second.hProcess), 1);
    }
    WaitForSingleObject(static_cast<HANDLE>(it->second.hProcess), INFINITE);
    CloseHandle(static_cast<HANDLE>(it->second.hProcess));
#else
    ::kill(-it->second.pid, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int status = 0;
    if (::waitpid(it->second.pid, &status, WNOHANG) == 0) {
        ::kill(-it->second.pid, SIGKILL);
        ::waitpid(it->second.pid, &status, 0);
    }
#endif
    m_daemons.erase(it);
}

} // namespace orchestration
