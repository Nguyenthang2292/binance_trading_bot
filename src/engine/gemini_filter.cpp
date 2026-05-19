#include "engine/gemini_filter.h"

#include "logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace engine {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string quoteString(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(std::string(value));
    return out.str();
}

std::string compactLogLine(std::string_view value) {
    constexpr size_t maxLen = 1200;
    std::string out;
    out.reserve(std::min(value.size(), maxLen));
    for (const char c : value) {
        if (out.size() >= maxLen) {
            out += "...";
            break;
        }
        if (c == '\r' || c == '\n' || c == '\t') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void logSubprocessDiagnostics(std::string_view diagnostics, LogLevel level = LogLevel::Subprocess) {
    std::istringstream lines{std::string(diagnostics)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        Logger::instance().log(level, "gemini.py | " + compactLogLine(line));
    }
}

std::string printableAsciiPrefix(std::string_view value, size_t maxLen = 800) {
    std::string out;
    out.reserve(std::min(value.size(), maxLen));
    for (const unsigned char c : value) {
        if (out.size() >= maxLen) {
            out += "...";
            break;
        }
        if (c >= 0x20 && c <= 0x7E) {
            out.push_back(static_cast<char>(c));
        } else if (c == '\r' || c == '\n' || c == '\t') {
            out.push_back(' ');
        } else {
            out += "\\x";
            constexpr char hex[] = "0123456789ABCDEF";
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string directionToString(strategy::Signal::Direction direction) {
    switch (direction) {
        case strategy::Signal::Direction::Long:
            return "Long";
        case strategy::Signal::Direction::Short:
            return "Short";
        case strategy::Signal::Direction::None:
            return "None";
    }
    return "None";
}

std::string fmt6(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

std::string klineSignature(const std::optional<std::vector<Kline>>& klines) {
    if (!klines.has_value() || klines->empty()) {
        return "missing";
    }
    const auto& first = klines->front();
    const auto& last = klines->back();
    std::ostringstream out;
    out << "n=" << klines->size()
        << "|f=" << first.openTime << ":" << first.closeTime
        << "|l=" << last.openTime << ":" << last.closeTime
        << "|c=" << fmt6(last.close);
    return out.str();
}

double readRequiredScore(const json& payload, std::string_view field) {
    const std::string key(field);
    if (!payload.contains(key)) {
        throw std::runtime_error("missing required field: " + key);
    }
    const auto& value = payload.at(key);
    if (!value.is_number()) {
        throw std::runtime_error("field must be number: " + key);
    }
    const double score = value.get<double>();
    if (!std::isfinite(score) || score < 0.0 || score > 1.0) {
        throw std::runtime_error("field must be finite score in [0,1]: " + key);
    }
    return score;
}

std::string readRequiredString(const json& payload, std::string_view field) {
    const std::string key(field);
    if (!payload.contains(key)) {
        throw std::runtime_error("missing required field: " + key);
    }
    const auto& value = payload.at(key);
    if (!value.is_string()) {
        throw std::runtime_error("field must be string: " + key);
    }
    return value.get<std::string>();
}

std::string makeEvalId() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    const uint64_t a = dist(gen);
    const uint64_t b = dist(gen);
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<uint32_t>(a >> 32) << "-"
        << std::setw(4) << static_cast<uint16_t>((a >> 16) & 0xFFFF) << "-"
        << std::setw(4) << static_cast<uint16_t>(a & 0xFFFF) << "-"
        << std::setw(4) << static_cast<uint16_t>(b >> 48) << "-"
        << std::setw(12) << (b & 0x0000FFFFFFFFFFFFULL);
    return out.str();
}

json serializeKlines(const std::optional<std::vector<Kline>>& klines) {
    json arr = json::array();
    if (!klines.has_value()) {
        return arr;
    }
    for (const auto& k : *klines) {
        arr.push_back({
            {"open_time", k.openTime},
            {"close_time", k.closeTime},
            {"open", k.open},
            {"high", k.high},
            {"low", k.low},
            {"close", k.close},
            {"volume", k.volume},
        });
    }
    return arr;
}

#ifdef _WIN32
std::string quoteWindowsArg(std::string_view value) {
    bool needsQuotes = value.empty();
    for (const char c : value) {
        if (c == ' ' || c == '\t' || c == '"') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return std::string(value);
    }
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

void readPipeAvailable(HANDLE readHandle, std::string& out) {
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readHandle, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            break;
        }
        char buffer[4096];
        const DWORD toRead = std::min<DWORD>(static_cast<DWORD>(sizeof(buffer)), available);
        DWORD readBytes = 0;
        if (!ReadFile(readHandle, buffer, toRead, &readBytes, nullptr) || readBytes == 0) {
            break;
        }
        out.append(buffer, buffer + readBytes);
    }
}
#else
bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void drainPipeAvailable(int fd, std::string& out, bool& openFlag) {
    std::array<char, 4096> buffer{};
    while (openFlag) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0) {
            out.append(buffer.data(), static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            openFlag = false;
            close(fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        openFlag = false;
        close(fd);
        return;
    }
}
#endif

} // namespace

GeminiFilterController::GeminiFilterController(GeminiFilterConfig config)
    : m_config(std::move(config)) {
    if (m_config.timeoutSeconds <= 0) {
        throw std::invalid_argument("gemini_filter.timeout_seconds must be > 0");
    }
    if (m_config.sentimentWeight < 0.0 || m_config.visionWeight < 0.0 ||
        (m_config.sentimentWeight + m_config.visionWeight) <= 0.0) {
        throw std::invalid_argument("gemini_filter weights must be non-negative and sum > 0");
    }
    if (m_config.confidenceThreshold < 0.0 || m_config.confidenceThreshold > 1.0) {
        throw std::invalid_argument("gemini_filter confidence_threshold must be in [0,1]");
    }
}

GeminiFilterResult GeminiFilterController::buildBlockResult(std::string reason, std::string errorCode) const {
    return {
        GeminiDecision::Block,
        0.0,
        0.0,
        0.0,
        std::move(reason),
        std::move(errorCode),
        true,
    };
}

std::string GeminiFilterController::buildCacheKey(
    std::string_view symbol,
    strategy::Signal::Direction direction,
    std::string_view signalInterval,
    const scanner::KlineCache& cache) const {
    std::ostringstream out;
    out << "v1"
        << "|symbol=" << symbol
        << "|direction=" << directionToString(direction)
        << "|tf=" << signalInterval
        << "|sentiment_model=" << m_config.sentimentModel
        << "|vision_model=" << m_config.visionModel
        << "|threshold=" << m_config.confidenceThreshold
        << "|weights=" << m_config.sentimentWeight << "," << m_config.visionWeight
        << "|primary=" << klineSignature(cache.snapshot(symbol, signalInterval));
    for (const auto& tf : m_config.extraTfs) {
        if (tf == signalInterval) {
            continue;
        }
        out << "|extra:" << tf << "=" << klineSignature(cache.snapshot(symbol, tf));
    }
    return out.str();
}

void GeminiFilterController::pruneExpiredCacheEntriesLocked(std::chrono::steady_clock::time_point now) const {
    for (auto it = m_resultCache.begin(); it != m_resultCache.end();) {
        if (it->second.expiresAt <= now) {
            it = m_resultCache.erase(it);
            continue;
        }
        ++it;
    }
}

std::optional<GeminiFilterResult> GeminiFilterController::getCachedResult(
    std::string_view symbol,
    strategy::Signal::Direction direction,
    std::string_view signalInterval,
    const scanner::KlineCache& cache) const {
    if (m_config.resultCacheTtlSeconds <= 0) {
        return std::nullopt;
    }
    const auto key = buildCacheKey(symbol, direction, signalInterval, cache);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(m_cacheMutex);
    auto it = m_resultCache.find(key);
    if (it == m_resultCache.end()) {
        ++m_cacheMissCount;
        const size_t total = m_cacheHitCount + m_cacheMissCount;
        if (total % 25 == 0) {
            const double hitRate = total > 0 ? static_cast<double>(m_cacheHitCount) / static_cast<double>(total) : 0.0;
            Logger::instance().log(
                LogLevel::Info,
                "gemini result cache metrics hits=" + std::to_string(m_cacheHitCount) +
                    " misses=" + std::to_string(m_cacheMissCount) +
                    " hit_rate=" + fmt6(hitRate));
        }
        return std::nullopt;
    }
    if (it->second.expiresAt <= now) {
        m_resultCache.erase(it);
        ++m_cacheMissCount;
        return std::nullopt;
    }
    ++m_cacheHitCount;
    const size_t total = m_cacheHitCount + m_cacheMissCount;
    if (total % 25 == 0) {
        const double hitRate = total > 0 ? static_cast<double>(m_cacheHitCount) / static_cast<double>(total) : 0.0;
        Logger::instance().log(
            LogLevel::Info,
            "gemini result cache metrics hits=" + std::to_string(m_cacheHitCount) +
                " misses=" + std::to_string(m_cacheMissCount) +
                " hit_rate=" + fmt6(hitRate));
    }
    return it->second.result;
}

void GeminiFilterController::putCachedResult(
    std::string_view symbol,
    strategy::Signal::Direction direction,
    std::string_view signalInterval,
    const scanner::KlineCache& cache,
    const GeminiFilterResult& result) const {
    if (m_config.resultCacheTtlSeconds <= 0) {
        return;
    }
    const auto key = buildCacheKey(symbol, direction, signalInterval, cache);
    auto ttl = std::chrono::seconds(m_config.resultCacheTtlSeconds);
    if (!result.errorCode.empty()) {
        if (result.errorCode == "quota_exhausted") {
            ttl = std::chrono::seconds(10);
        } else if (result.errorCode == "component_error" || result.errorCode == "timeout") {
            ttl = std::chrono::seconds(60);
        } else {
            ttl = std::chrono::seconds(300);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(m_cacheMutex);
    if (m_resultCache.size() >= 4096) {
        pruneExpiredCacheEntriesLocked(now);
    }
    m_resultCache[key] = CachedResult{
        .result = result,
        .expiresAt = now + ttl,
    };
}

void GeminiFilterController::cleanupStaleEvalDirsOnce() const {
    std::lock_guard lock(m_staleCleanupMutex);
    if (m_staleCleanupDone) {
        return;
    }
    m_staleCleanupDone = true;

    if (m_config.staleRuntimeTtlHours <= 0) {
        return;
    }

    try {
        const fs::path runtimeDir(m_config.runtimeDir);
        if (!fs::exists(runtimeDir) || !fs::is_directory(runtimeDir)) {
            return;
        }

        const auto now = fs::file_time_type::clock::now();
        const auto ttl = std::chrono::hours(m_config.staleRuntimeTtlHours);
        for (const auto& entry : fs::directory_iterator(runtimeDir)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name.rfind("eval-", 0) != 0) {
                continue;
            }
            std::error_code ecTime;
            const auto writeTime = fs::last_write_time(entry.path(), ecTime);
            if (ecTime) {
                continue;
            }
            if ((now - writeTime) > ttl) {
                std::error_code ecRm;
                fs::remove_all(entry.path(), ecRm);
                if (ecRm) {
                    Logger::instance().log(
                        LogLevel::Warning,
                        "gemini stale cleanup failed path=" + quoteString(entry.path().string()) +
                            " reason=" + ecRm.message());
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Warning, std::string("gemini stale cleanup exception: ") + e.what());
    }
}

std::string GeminiFilterController::runSubprocess(const std::string& inputPath) const {
    return runPythonModule(m_config.moduleName, {inputPath}, m_config.timeoutSeconds);
}

std::string GeminiFilterController::runPythonModule(
    const std::string& moduleName,
    const std::vector<std::string>& args,
    int timeoutSeconds) const {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
        throw std::runtime_error("CreatePipe stdout failed");
    }
    if (!SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        throw std::runtime_error("SetHandleInformation stdout failed");
    }
    if (!CreatePipe(&stderrRead, &stderrWrite, &sa, 0)) {
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        throw std::runtime_error("CreatePipe stderr failed");
    }
    if (!SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);
        throw std::runtime_error("SetHandleInformation stderr failed");
    }

    std::string commandLine = quoteWindowsArg(m_config.pythonPath) +
        " -m " + quoteWindowsArg(moduleName);
    for (const auto& arg : args) {
        commandLine += " " + quoteWindowsArg(arg);
    }
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stderrWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    const char* workDir = m_config.workingDirectory.empty() ? nullptr : m_config.workingDirectory.c_str();
    const BOOL created = CreateProcessA(
        nullptr,
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workDir,
        &si,
        &pi);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (!created) {
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        throw std::runtime_error("CreateProcess failed");
    }

    Logger::instance().log(LogLevel::Info, "gemini subprocess started command=" + quoteString(commandLine));

    std::string output;
    std::string diagnostics;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    for (;;) {
        readPipeAvailable(stdoutRead, output);
        readPipeAvailable(stderrRead, diagnostics);
        const DWORD waitCode = WaitForSingleObject(pi.hProcess, 50);
        if (waitCode == WAIT_OBJECT_0) {
            break;
        }
        if (waitCode == WAIT_FAILED) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(stdoutRead);
            CloseHandle(stderrRead);
            throw std::runtime_error("WaitForSingleObject failed");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            readPipeAvailable(stdoutRead, output);
            readPipeAvailable(stderrRead, diagnostics);
            if (!diagnostics.empty()) {
                logSubprocessDiagnostics(diagnostics, LogLevel::Warning);
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(stdoutRead);
            CloseHandle(stderrRead);
            throw std::runtime_error("gemini filter timeout");
        }
    }

    readPipeAvailable(stdoutRead, output);
    readPipeAvailable(stderrRead, diagnostics);
    if (!diagnostics.empty()) {
        logSubprocessDiagnostics(diagnostics);
    }

    DWORD exitCode = 1;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        throw std::runtime_error("GetExitCodeProcess failed");
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);

    if (exitCode != 0) {
        throw std::runtime_error("gemini subprocess exit code " + std::to_string(exitCode));
    }
    return output;
#else
    Logger::instance().log(
        LogLevel::Info,
        "gemini subprocess started command=" + quoteString(m_config.pythonPath + " -m " + moduleName));

    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        if (stdoutPipe[0] >= 0) close(stdoutPipe[0]);
        if (stdoutPipe[1] >= 0) close(stdoutPipe[1]);
        if (stderrPipe[0] >= 0) close(stderrPipe[0]);
        if (stderrPipe[1] >= 0) close(stderrPipe[1]);
        throw std::runtime_error("pipe failed");
    }

    const pid_t child = fork();
    if (child < 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);
        throw std::runtime_error("fork failed");
    }

    if (child == 0) {
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);

        if (!m_config.workingDirectory.empty()) {
            (void)chdir(m_config.workingDirectory.c_str());
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(m_config.pythonPath.c_str()));
        argv.push_back(const_cast<char*>("-m"));
        argv.push_back(const_cast<char*>(moduleName.c_str()));
        std::vector<std::string> ownedArgs;
        ownedArgs.reserve(args.size());
        for (const auto& arg : args) {
            ownedArgs.push_back(arg);
        }
        for (auto& arg : ownedArgs) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(stdoutPipe[1]);
    close(stderrPipe[1]);

    (void)setNonBlocking(stdoutPipe[0]);
    (void)setNonBlocking(stderrPipe[0]);

    std::string output;
    std::string diagnostics;
    bool stdoutOpen = true;
    bool stderrOpen = true;
    bool timedOut = false;
    bool childExited = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

    while (stdoutOpen || stderrOpen || !childExited) {
        drainPipeAvailable(stdoutPipe[0], output, stdoutOpen);
        drainPipeAvailable(stderrPipe[0], diagnostics, stderrOpen);

        if (!childExited) {
            const pid_t waitResult = waitpid(child, &status, WNOHANG);
            if (waitResult == child) {
                childExited = true;
            } else if (waitResult < 0) {
                if (stdoutOpen) {
                    close(stdoutPipe[0]);
                    stdoutOpen = false;
                }
                if (stderrOpen) {
                    close(stderrPipe[0]);
                    stderrOpen = false;
                }
                throw std::runtime_error("waitpid failed");
            }
        }

        if (!childExited && std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            kill(child, SIGKILL);
            (void)waitpid(child, &status, 0);
            childExited = true;
        }

        if ((stdoutOpen || stderrOpen || !childExited) && !timedOut) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    if (!diagnostics.empty()) {
        logSubprocessDiagnostics(diagnostics);
    }

    if (timedOut) {
        throw std::runtime_error("gemini filter timeout");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            throw std::runtime_error("gemini subprocess terminated by signal " + std::to_string(WTERMSIG(status)));
        }
        throw std::runtime_error("gemini subprocess exit code " + std::to_string(WEXITSTATUS(status)));
    }
    return output;
#endif
}

void GeminiFilterController::maybeTriggerAutotune() const {
    if (!m_config.autotuneEnabled) {
        return;
    }
    const std::string mode = m_config.autotuneMode;
    if (mode == "disabled") {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(m_autotuneMutex);
        if (m_autotuneRunning) {
            return;
        }
        if (m_nextAutotuneAt.time_since_epoch().count() > 0 && now < m_nextAutotuneAt) {
            return;
        }
        m_autotuneRunning = true;
        const int intervalSeconds = std::max(30, m_config.autotuneIntervalSeconds);
        m_nextAutotuneAt = now + std::chrono::seconds(intervalSeconds);
    }
    std::thread(
        [this]() {
            try {
                runAutotuneController();
            } catch (const std::exception& e) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "autotune controller run failed reason=" + quoteString(e.what()));
            } catch (...) {
                Logger::instance().log(
                    LogLevel::Warning,
                    "autotune controller run failed reason=" + quoteString("unknown exception"));
            }
            std::lock_guard lock(m_autotuneMutex);
            m_autotuneRunning = false;
        })
        .detach();
}

void GeminiFilterController::runAutotuneController() const {
    const fs::path runtimeBase = fs::absolute(fs::path(m_config.runtimeDir));
    const fs::path configPath = fs::absolute(fs::path(m_config.workingDirectory) / m_config.autotuneConfigPath);
    const std::vector<std::string> args{
        "--config",
        configPath.string(),
        "--runtime-base-dir",
        runtimeBase.string(),
    };
    const int timeoutSeconds = std::max(10, m_config.autotuneControllerTimeoutSeconds);
    const auto started = std::chrono::steady_clock::now();
    (void)runPythonModule("tools.gemini_filter.autotune", args, timeoutSeconds);
    const auto latencyMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    Logger::instance().log(
        LogLevel::Info,
        "autotune controller completed mode=" + quoteString(m_config.autotuneMode) +
            " runtime_base_dir=" + quoteString(runtimeBase.string()) +
            " latency_ms=" + std::to_string(latencyMs));
}

GeminiFilterResult GeminiFilterController::evaluate(
    std::string_view symbol,
    strategy::Signal::Direction direction,
    std::string_view signalInterval,
    const scanner::KlineCache& cache) const {
    if (!m_config.enabled || m_config.mode == GeminiFilterMode::Disabled) {
        return {GeminiDecision::Allow, 1.0, 1.0, 1.0, "gemini filter disabled", {}, false};
    }

    maybeTriggerAutotune();

    if (const auto cached = getCachedResult(symbol, direction, signalInterval, cache)) {
        Logger::instance().log(
            LogLevel::Info,
            "gemini result cache hit symbol=" + std::string(symbol) +
                " tf=" + std::string(signalInterval) +
                " decision=" + (cached->decision == GeminiDecision::Allow ? "Allow" : "Block"));
        return *cached;
    }

    cleanupStaleEvalDirsOnce();

    const auto startedAt = std::chrono::steady_clock::now();
    const std::string evalId = makeEvalId();
    fs::path evalDir = fs::path(m_config.runtimeDir) / ("eval-" + evalId);
    fs::path inputPath = evalDir / "input.json";

    try {
        Logger::instance().log(
            LogLevel::Info,
            "gemini eval started eval_id=" + evalId +
                " symbol=" + std::string(symbol) +
                " tf=" + std::string(signalInterval) +
                " runtime_dir=" + quoteString(fs::absolute(evalDir).string()));

        std::error_code ec;
        fs::create_directories(evalDir, ec);
        if (ec) {
            return buildBlockResult("failed to create eval directory: " + ec.message(), "runtime_dir_create_failed");
        }

        json payload;
        payload["eval_id"] = evalId;
        payload["symbol"] = std::string(symbol);
        payload["direction"] = directionToString(direction);
        payload["primary_tf"] = std::string(signalInterval);
        payload["extra_tfs"] = m_config.extraTfs;

        json klines = json::object();
        const auto primary = cache.snapshot(symbol, signalInterval);
        klines[std::string(signalInterval)] = serializeKlines(primary);
        for (const auto& tf : m_config.extraTfs) {
            if (tf == signalInterval) {
                continue;
            }
            const auto extra = cache.snapshot(symbol, tf);
            if (extra.has_value()) {
                klines[tf] = serializeKlines(extra);
            }
        }
        payload["klines"] = std::move(klines);
        payload["runtime_dir"] = fs::absolute(evalDir).string();
        payload["runtime_base_dir"] = fs::absolute(fs::path(m_config.runtimeDir)).string();
        payload["sentiment_model"] = m_config.sentimentModel;
        payload["vision_model"] = m_config.visionModel;
        payload["model_resolution"] = {
            {"enabled", m_config.modelResolutionEnabled},
            {"mode", m_config.modelResolutionMode},
            {"fallback_on_error", m_config.modelResolutionFallbackOnError},
            {"allow_preview", m_config.modelResolutionAllowPreview},
        };
        payload["sentiment_search_then_score"] = m_config.sentimentSearchThenScore;
        payload["sentiment_weight"] = m_config.sentimentWeight;
        payload["vision_weight"] = m_config.visionWeight;
        payload["confidence_threshold"] = m_config.confidenceThreshold;
        payload["sentiment_cache_ttl_seconds"] = m_config.sentimentCacheTtlSeconds;
        payload["sentiment_cache_max_stale_seconds"] = m_config.sentimentCacheMaxStaleSeconds;
        payload["model_resolution_ttl_seconds"] = m_config.modelResolutionTtlSeconds;
        payload["model_resolution_max_stale_seconds"] = m_config.modelResolutionMaxStaleSeconds;
        payload["model_routing"] = {
            {"enabled", m_config.modelRoutingEnabled},
            {"sentiment", {{"candidates", m_config.sentimentModelCandidates}}},
            {"vision",
             {{"candidates", m_config.visionModelCandidates},
              {"pro_escalation_enabled", m_config.visionProEscalationEnabled},
              {"pro_escalation_min_score", m_config.visionProEscalationMinScore},
              {"pro_escalation_max_score", m_config.visionProEscalationMaxScore}}},
        };
        json quotaModels = json::object();
        for (const auto& limit : m_config.quotaModelLimits) {
            quotaModels[limit.model] = {
                {"rpm", limit.rpm},
                {"rpd", limit.rpd},
            };
        }
        payload["quota"] = {
            {"enabled", m_config.quotaEnabled},
            {"safety_factor", m_config.quotaSafetyFactor},
            {"cooldown_seconds_on_429", m_config.quotaCooldownSecondsOn429},
            {"default_rpm", m_config.quotaDefaultRpm},
            {"default_rpd", m_config.quotaDefaultRpd},
            {"models", quotaModels},
        };
        payload["autotune"] = {
            {"enabled", m_config.autotuneEnabled},
            {"mode", m_config.autotuneMode},
        };

        {
            std::ofstream out(inputPath, std::ios::binary);
            if (!out) {
                return buildBlockResult("failed to write input json", "input_write_failed");
            }
            out << payload.dump();
        }

        const std::string output = runSubprocess(fs::absolute(inputPath).string());
        json parsed;
        try {
            parsed = json::parse(output);
        } catch (const std::exception& e) {
            Logger::instance().log(
                LogLevel::Error,
                "gemini json parse failed eval_id=" + evalId +
                    " reason=" + quoteString(e.what()) +
                    " output_prefix=" + quoteString(printableAsciiPrefix(output)));
            throw;
        }

        const auto decisionStr = readRequiredString(parsed, "decision");
        GeminiDecision decision = GeminiDecision::Block;
        if (decisionStr == "Allow") {
            decision = GeminiDecision::Allow;
        } else if (decisionStr == "Block") {
            decision = GeminiDecision::Block;
        } else {
            throw std::runtime_error("invalid decision value");
        }

        GeminiFilterResult result;
        result.decision = decision;
        result.confidence = readRequiredScore(parsed, "confidence");
        result.sentimentScore = readRequiredScore(parsed, "sentiment_score");
        result.visionScore = readRequiredScore(parsed, "vision_score");
        result.reason = readRequiredString(parsed, "reason");
        if (parsed.contains("error_code") && !parsed["error_code"].is_null()) {
            if (!parsed["error_code"].is_string()) {
                throw std::runtime_error("error_code must be string or null");
            }
            result.errorCode = parsed["error_code"].get<std::string>();
        }
        result.hasError = !result.errorCode.empty();

        const auto latencyMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();
        Logger::instance().log(
            LogLevel::Info,
            "gemini eval completed eval_id=" + evalId +
                " symbol=" + std::string(symbol) +
                " tf=" + std::string(signalInterval) +
                " decision=" + (result.decision == GeminiDecision::Allow ? "Allow" : "Block") +
                " confidence=" + std::to_string(result.confidence) +
                " latency_ms=" + std::to_string(latencyMs));
        putCachedResult(symbol, direction, signalInterval, cache, result);

        std::error_code ecRm;
        fs::remove_all(evalDir, ecRm);
        if (ecRm) {
            Logger::instance().log(
                LogLevel::Warning,
                "gemini cleanup failed eval_id=" + evalId + " reason=" + ecRm.message());
        }
        return result;
    } catch (const std::exception& e) {
        std::error_code ecRm;
        fs::remove_all(evalDir, ecRm);
        if (ecRm) {
            Logger::instance().log(
                LogLevel::Warning,
                "gemini cleanup failed eval_id=" + evalId + " reason=" + ecRm.message());
        }
        Logger::instance().log(
            LogLevel::Error,
            "gemini eval failed eval_id=" + evalId + " reason=" + quoteString(e.what()));
        const auto block = buildBlockResult(std::string("gemini evaluate failed: ") + e.what(), "evaluate_exception");
        putCachedResult(symbol, direction, signalInterval, cache, block);
        return block;
    }
}

} // namespace engine
