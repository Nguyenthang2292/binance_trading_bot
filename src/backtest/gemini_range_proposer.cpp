#include "backtest/gemini_range_proposer.h"
#include "logger.h"
#include "strategy/indicators/atr.h"

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
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace backtest {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── Utility helpers (mirrored from gemini_filter.cpp) ─────────────────────

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
        << std::setw(12) << (b & 0x0000FFFFFFFFFFFFull);
    return out.str();
}

std::string fmt6(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(6) << v;
    return o.str();
}

std::string quoteStr(std::string_view v) {
    std::ostringstream o;
    o << std::quoted(std::string(v));
    return o.str();
}

void logSubprocess(std::string_view diag, LogLevel level = LogLevel::Subprocess) {
    std::istringstream lines{std::string(diag)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty())
            Logger::instance().log(level, "backtest_proposer.py | " + line);
    }
}

IRangeProposer::Failure failure(
    IRangeProposer::FailureReason reason,
    std::string message) {
    return IRangeProposer::Failure{
        .reason = reason,
        .message = std::move(message),
    };
}

IRangeProposer::FailureReason failureReasonFromPythonError(std::string_view code) {
    if (code == "invalid_input" || code == "invalid_response") {
        return IRangeProposer::FailureReason::InvalidResponse;
    }
    if (code == "timeout") {
        return IRangeProposer::FailureReason::Timeout;
    }
    return IRangeProposer::FailureReason::Unavailable;
}

#ifdef _WIN32
std::string quoteWinArg(std::string_view v) {
    bool needs = v.empty();
    for (char c : v) if (c == ' ' || c == '\t' || c == '"') { needs = true; break; }
    if (!needs) return std::string(v);
    std::string out; out.reserve(v.size() + 2); out.push_back('"');
    for (char c : v) { if (c == '"') out += "\\\""; else out.push_back(c); }
    out.push_back('"'); return out;
}

void readAvailable(HANDLE h, std::string& out) {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        char buf[4096];
        DWORD toRead = std::min<DWORD>(sizeof(buf), avail), got = 0;
        if (!ReadFile(h, buf, toRead, &got, nullptr) || got == 0) break;
        out.append(buf, buf + got);
    }
}
#else
bool setNB(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    return f >= 0 && fcntl(fd, F_SETFL, f | O_NONBLOCK) == 0;
}
void drainFd(int fd, std::string& out, bool& open) {
    std::array<char, 4096> buf{};
    while (open) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) { out.append(buf.data(), n); continue; }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        { open = false; close(fd); }
        return;
    }
}
#endif

}  // namespace

// ── PromptContextAggregates computation ───────────────────────────────────

PromptContextAggregates computePromptAggregates(const std::vector<Kline>& ctx) {
    PromptContextAggregates agg;
    if (ctx.empty()) return agg;

    agg.numCandles = static_cast<int>(ctx.size());

    // ATR of last bar (as % of close)
    if (ctx.size() >= 2) {
        const double atr = strategy::indicators::lastAtr(ctx, std::min<int>(14, static_cast<int>(ctx.size()) - 1));
        if (ctx.back().close > 0.0) agg.atrPctCurrent = atr / ctx.back().close * 100.0;
    }

    // Return over window
    if (ctx.front().close > 0.0) {
        agg.ret30dPct = (ctx.back().close - ctx.front().close) / ctx.front().close * 100.0;
    }

    // Avg volume (USD proxy = volume * close)
    double volSum = 0.0;
    for (const auto& k : ctx) volSum += k.volume * k.close;
    agg.avgVolumeUsd = volSum / ctx.size();

    // Trend direction: compare first half vs second half mean close
    const size_t mid = ctx.size() / 2;
    double firstHalf = 0.0, secondHalf = 0.0;
    for (size_t i = 0; i < mid; ++i) firstHalf += ctx[i].close;
    for (size_t i = mid; i < ctx.size(); ++i) secondHalf += ctx[i].close;
    firstHalf /= std::max<size_t>(1, mid);
    secondHalf /= std::max<size_t>(1, ctx.size() - mid);
    constexpr double kTrendThreshold = 0.005;  // 0.5%
    if (secondHalf > firstHalf * (1.0 + kTrendThreshold)) agg.trendDirection = "up";
    else if (secondHalf < firstHalf * (1.0 - kTrendThreshold)) agg.trendDirection = "down";
    else agg.trendDirection = "flat";

    // Realized volatility: std of log returns
    if (ctx.size() >= 2) {
        std::vector<double> logRet;
        logRet.reserve(ctx.size() - 1);
        for (size_t i = 1; i < ctx.size(); ++i) {
            if (ctx[i-1].close > 0.0 && ctx[i].close > 0.0)
                logRet.push_back(std::log(ctx[i].close / ctx[i-1].close));
        }
        if (!logRet.empty()) {
            double mean = std::accumulate(logRet.begin(), logRet.end(), 0.0) / logRet.size();
            double var = 0.0;
            for (double r : logRet) var += (r - mean) * (r - mean);
            agg.realizedVol = std::sqrt(var / logRet.size());
        }
    }

    return agg;
}

// ── GeminiRangeProposer ────────────────────────────────────────────────────

GeminiRangeProposer::GeminiRangeProposer(BacktestGateGeminiConfig cfg, BacktestGateCacheConfig cacheCfg)
    : m_cfg(std::move(cfg)), m_cacheCfg(std::move(cacheCfg)) {}

// ── Cache helpers ──────────────────────────────────────────────────────────

std::string GeminiRangeProposer::buildCacheKey(
    const RangeProposalRequest& req,
    const PromptContextAggregates& aggs) const
{
    // key v1.1|symbol|strategyId|interval|ret30d|atrPct|trend|numCandles
    std::ostringstream k;
    k << "v1.1"
      << "|" << req.symbol
      << "|" << req.strategyId
      << "|" << req.interval
      << "|ret=" << fmt6(aggs.ret30dPct)
      << "|atr=" << fmt6(aggs.atrPctCurrent)
      << "|trend=" << aggs.trendDirection
      << "|n=" << aggs.numCandles
      << "|model=" << m_cfg.model;
    return k.str();
}

void GeminiRangeProposer::evictExpiredLocked() const {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_cache.begin(); it != m_cache.end(); ) {
        if (it->second.expiresAt <= now) it = m_cache.erase(it);
        else ++it;
    }
}

std::optional<IRangeProposer::Output> GeminiRangeProposer::getCached(const std::string& key) const {
    std::lock_guard lock(m_cacheMutex);
    auto it = m_cache.find(key);
    if (it == m_cache.end()) return std::nullopt;
    if (it->second.expiresAt <= std::chrono::steady_clock::now()) {
        m_cache.erase(it); return std::nullopt;
    }
    return it->second.result;
}

void GeminiRangeProposer::putCached(const std::string& key, const Output& result) const {
    const auto ttl = std::chrono::seconds(std::max(1, m_cacheCfg.ttlSeconds));
    const auto maxEntries = static_cast<size_t>(std::max(1, m_cacheCfg.maxEntries));
    std::lock_guard lock(m_cacheMutex);
    if (m_cache.size() >= maxEntries) evictExpiredLocked();
    m_cache[key] = CachedResult{result, std::chrono::steady_clock::now() + ttl};
}

// ── Stale cleanup ──────────────────────────────────────────────────────────

void GeminiRangeProposer::cleanupStaleEvalDirsOnce() const {
    std::lock_guard lock(m_cleanupMutex);
    if (m_cleanupDone) return;
    m_cleanupDone = true;
    if (m_cfg.staleRuntimeTtlHours <= 0) return;
    try {
        const fs::path rtDir(m_cfg.runtimeDir);
        if (!fs::exists(rtDir) || !fs::is_directory(rtDir)) return;
        const auto now = fs::file_time_type::clock::now();
        const auto ttl = std::chrono::hours(m_cfg.staleRuntimeTtlHours);
        for (const auto& e : fs::directory_iterator(rtDir)) {
            if (!e.is_directory()) continue;
            const auto name = e.path().filename().string();
            if (name.rfind("eval-", 0) != 0) continue;
            std::error_code ec;
            if ((now - fs::last_write_time(e.path(), ec)) > ttl && !ec)
                fs::remove_all(e.path(), ec);
        }
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Warning, std::string("backtest_proposer stale cleanup: ") + e.what());
    }
}

// ── Input JSON builder ─────────────────────────────────────────────────────

std::string GeminiRangeProposer::buildInputJson(
    const RangeProposalRequest& req,
    const PromptContextAggregates& aggs,
    const std::string& evalId) const
{
    json j;
    j["eval_id"] = evalId;
    j["symbol"] = req.symbol;
    j["interval"] = req.interval;
    j["strategy_id"] = req.strategyId;
    j["model"] = m_cfg.model;

    // Build tunable_params list from default_ranges
    json tunableArr = json::array();
    for (const auto& r : req.defaultRanges) tunableArr.push_back(r.name);
    j["tunable_params"] = tunableArr;

    // current_values — use adapter-provided values where available, midpoints otherwise
    json curVals = json::object();
    for (const auto& r : req.defaultRanges) {
        auto it = req.currentValues.find(r.name);
        curVals[r.name] = (it != req.currentValues.end()) ? it->second : (r.min + r.max) / 2.0;
    }
    j["current_values"] = curVals;

    // prompt_context_aggregates — ONLY from promptContext slice
    j["prompt_context_aggregates"] = {
        {"ret_30d_pct",     aggs.ret30dPct},
        {"atr_pct_current", aggs.atrPctCurrent},
        {"avg_volume_usd",  aggs.avgVolumeUsd},
        {"trend_direction", aggs.trendDirection},
        {"realized_vol_30d", aggs.realizedVol},
        {"num_candles",     aggs.numCandles},
    };

    // default_ranges
    json rangesArr = json::array();
    for (const auto& r : req.defaultRanges) {
        rangesArr.push_back({
            {"name", r.name}, {"min", r.min}, {"max", r.max},
            {"step", r.step}, {"is_integer", r.isInteger},
        });
    }
    j["default_ranges"] = rangesArr;

    // constraints — tells Gemini which parameter pairs must satisfy an ordering
    json constraintsArr = json::array();
    for (const auto& c : req.constraints) {
        constraintsArr.push_back({
            {"left",  c.left},
            {"right", c.right},
            {"kind",  c.kind == ParamConstraint::Kind::LessThan ? "less_than" : "less_than_or_equal"},
        });
    }
    j["constraints"] = constraintsArr;

    // signal context — direction and bar timestamp for Gemini reasoning
    j["signal"] = {
        {"direction",      req.signalDirection == strategy::Signal::Direction::Long ? "long" : "short"},
        {"bar_open_time",  req.signalBarOpenTimeMs},
    };

    // budget — max grid size so Gemini knows how tightly to focus ranges
    j["budget"] = {{"max_total_combos", req.maxTotalCombos}};

    return j.dump();
}

// ── Subprocess runner (mirrored from gemini_filter.cpp) ───────────────────

std::string GeminiRangeProposer::runPythonModule(
    const std::string& inputPath,
    const std::string& outputPath,
    int timeoutSeconds) const
{
    const std::string moduleName = m_cfg.moduleName;
    const std::string pythonPath = m_cfg.pythonPath;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0)) throw std::runtime_error("CreatePipe stdout");
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&errR, &errW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); throw std::runtime_error("CreatePipe stderr"); }
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = quoteWinArg(pythonPath) + " -m " + quoteWinArg(moduleName)
                    + " " + quoteWinArg(inputPath) + " " + quoteWinArg(outputPath);
    std::vector<char> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back('\0');

    STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW; si.hStdError = errW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const char* wd = m_cfg.workingDirectory.empty() ? nullptr : m_cfg.workingDirectory.c_str();
    const BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, wd, &si, &pi);
    CloseHandle(outW); CloseHandle(errW);
    if (!ok) { CloseHandle(outR); CloseHandle(errR); throw std::runtime_error("CreateProcess failed"); }

    Logger::instance().log(LogLevel::Info, "backtest_proposer subprocess started cmd=" + quoteStr(cmd));
    std::string diag;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    for (;;) {
        readAvailable(errR, diag);
        const DWORD w = WaitForSingleObject(pi.hProcess, 50);
        if (w == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            readAvailable(errR, diag);
            if (!diag.empty()) logSubprocess(diag, LogLevel::Warning);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            CloseHandle(outR); CloseHandle(errR);
            throw std::runtime_error("backtest_proposer subprocess timeout");
        }
    }
    readAvailable(errR, diag);
    if (!diag.empty()) logSubprocess(diag);
    DWORD exitCode = 1; GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(outR); CloseHandle(errR);

    // Read output file even on non-zero exits. The Python module writes
    // structured failures there so the controller can preserve the reason.
    std::ifstream ifs(outputPath);
    if (!ifs) {
        if (exitCode != 0) {
            throw std::runtime_error("backtest_proposer exit code " + std::to_string(exitCode));
        }
        throw std::runtime_error("cannot read output file: " + outputPath);
    }
    return std::string{std::istreambuf_iterator<char>(ifs), {}};

#else
    int errPipe[2] = {-1, -1};
    if (pipe(errPipe) != 0) throw std::runtime_error("pipe failed");
    const pid_t child = fork();
    if (child < 0) { close(errPipe[0]); close(errPipe[1]); throw std::runtime_error("fork failed"); }
    if (child == 0) {
        dup2(errPipe[1], STDERR_FILENO);
        close(errPipe[0]); close(errPipe[1]);
        if (!m_cfg.workingDirectory.empty()) chdir(m_cfg.workingDirectory.c_str());
        std::vector<std::string> ownedArgs = {pythonPath, "-m", moduleName, inputPath, outputPath};
        std::vector<char*> argv;
        for (auto& a : ownedArgs) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(errPipe[1]);
    (void)setNB(errPipe[0]);

    std::string diag;
    bool errOpen = true, childDone = false, timedOut = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (!childDone || errOpen) {
        drainFd(errPipe[0], diag, errOpen);
        if (!childDone) {
            pid_t w = waitpid(child, &status, WNOHANG);
            if (w == child) childDone = true;
            else if (!childDone && std::chrono::steady_clock::now() >= deadline) {
                timedOut = true; kill(child, SIGKILL); waitpid(child, &status, 0); childDone = true;
            }
        }
        if (!childDone || errOpen) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!diag.empty()) logSubprocess(timedOut ? diag : diag, timedOut ? LogLevel::Warning : LogLevel::Subprocess);
    std::ifstream ifs(outputPath);
    if (!ifs) {
        if (timedOut) throw std::runtime_error("backtest_proposer subprocess timeout");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw std::runtime_error("backtest_proposer exit code " + std::to_string(WEXITSTATUS(status)));
        throw std::runtime_error("cannot read output file: " + outputPath);
    }
    if (timedOut) throw std::runtime_error("backtest_proposer subprocess timeout");
    return std::string{std::istreambuf_iterator<char>(ifs), {}};
#endif
}

// ── Output parser ──────────────────────────────────────────────────────────

IRangeProposer::Result GeminiRangeProposer::parseOutput(
    const std::string& rawJson,
    const std::vector<std::string>& tunableParams) const
{
    try {
        const auto j = json::parse(rawJson);

        // Error path
        if (j.value("error", false)) {
            const std::string code = j.value("error_code", "?");
            const std::string reason = j.value("reason", "?");
            Logger::instance().log(LogLevel::Warning,
                "backtest_proposer returned error code=" + code +
                " reason=" + reason);
            return failure(failureReasonFromPythonError(code), reason);
        }

        if (!j.contains("ranges") || !j.at("ranges").is_array())
            throw std::runtime_error("missing 'ranges' array");

        Output out;
        out.notes = j.value("notes", "");

        for (const auto& r : j.at("ranges")) {
            const std::string name = r.at("name").get<std::string>();
            // Validate name is in tunableParams
            if (std::find(tunableParams.begin(), tunableParams.end(), name) == tunableParams.end()) {
                throw std::runtime_error("unknown param in output: " + name);
            }
            ParamRange pr;
            pr.name = name;
            pr.min = r.at("min").get<double>();
            pr.max = r.at("max").get<double>();
            pr.step = r.at("step").get<double>();
            pr.isInteger = r.at("is_integer").get<bool>();

            if (!std::isfinite(pr.min) || !std::isfinite(pr.max) || !std::isfinite(pr.step))
                throw std::runtime_error("non-finite value in param range: " + name);
            if (pr.min > pr.max)
                throw std::runtime_error("min > max for param: " + name);
            if (pr.step <= 0.0)
                throw std::runtime_error("step <= 0 for param: " + name);
            if (pr.isInteger) {
                const double fmin = std::floor(pr.min), fmax = std::floor(pr.max),
                             fstep = std::floor(pr.step);
                if (pr.min != fmin || pr.max != fmax || pr.step != fstep)
                    throw std::runtime_error("is_integer=true but non-integer bound for param: " + name);
            }

            out.ranges.push_back(std::move(pr));
        }
        return out;
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Warning,
            "backtest_proposer output parse failed: " + std::string(e.what()));
        return failure(IRangeProposer::FailureReason::InvalidResponse, e.what());
    }
}

// ── Public interface ───────────────────────────────────────────────────────

IRangeProposer::Result GeminiRangeProposer::propose(
    const RangeProposalRequest& req)
{
    // req.promptContext and req.deadline are both populated by BacktestGateController
    // before calling this method.  Delegate to the full implementation.
    const auto deadline = req.deadline.time_since_epoch().count() != 0
        ? req.deadline
        : std::chrono::steady_clock::now() + std::chrono::seconds(m_cfg.timeoutSeconds + 2);
    return proposeWithPartitions(req, req.promptContext, deadline);
}

IRangeProposer::Result GeminiRangeProposer::proposeWithPartitions(
    const RangeProposalRequest& req,
    const std::vector<Kline>& promptContext,
    std::chrono::steady_clock::time_point deadline)
{
    // 1. Compute aggregates exclusively from promptContext
    const auto aggs = computePromptAggregates(promptContext);

    // 2. Check cache
    const auto cacheKey = buildCacheKey(req, aggs);
    if (auto cached = getCached(cacheKey)) {
        Logger::instance().log(LogLevel::Info,
            "backtest_proposer cache hit symbol=" + req.symbol + " strategy=" + req.strategyId);
        return *cached;
    }

    cleanupStaleEvalDirsOnce();

    // 3. Build eval dir
    const std::string evalId = makeEvalId();
    const fs::path rtDir = fs::path(m_cfg.runtimeDir) / ("eval-" + evalId);
    try {
        fs::create_directories(rtDir);
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Warning,
            "backtest_proposer cannot create eval dir: " + std::string(e.what()));
        return failure(IRangeProposer::FailureReason::InternalError, e.what());
    }
    const std::string inputPath = (rtDir / "input.json").string();
    const std::string outputPath = (rtDir / "output.json").string();

    // 4. Write input JSON
    try {
        const std::string inputJson = buildInputJson(req, aggs, evalId);
        std::ofstream ofs(inputPath);
        if (!ofs) throw std::runtime_error("cannot write input.json");
        ofs << inputJson;
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Warning,
            "backtest_proposer input write failed: " + std::string(e.what()));
        return failure(IRangeProposer::FailureReason::InternalError, e.what());
    }

    // 5. Compute remaining time for subprocess
    const auto now = std::chrono::steady_clock::now();
    const int remainSecs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count());
    const int timeoutSec = std::min(m_cfg.timeoutSeconds, std::max(1, remainSecs - 1));

    Logger::instance().log(LogLevel::Info,
        "BACKTEST_GATE_GEMINI_START eval_id=" + evalId +
        " symbol=" + req.symbol +
        " strategy=" + req.strategyId +
        " timeout_sec=" + std::to_string(timeoutSec));

    // 6. Run subprocess
    std::string rawOutput;
    try {
        rawOutput = runPythonModule(inputPath, outputPath, timeoutSec);
    } catch (const std::exception& e) {
        const std::string message = e.what();
        const auto reason = message.find("timeout") != std::string::npos
            ? IRangeProposer::FailureReason::Timeout
            : IRangeProposer::FailureReason::Unavailable;
        Logger::instance().log(LogLevel::Warning,
            "BACKTEST_GATE_GEMINI_FAIL eval_id=" + evalId + " reason=" + quoteStr(e.what()));
        return failure(reason, message);
    }

    // 7. Parse & validate
    std::vector<std::string> tunableParams;
    for (const auto& r : req.defaultRanges) tunableParams.push_back(r.name);
    auto result = parseOutput(rawOutput, tunableParams);

    if (const auto* out = std::get_if<IRangeProposer::Output>(&result)) {
        Logger::instance().log(LogLevel::Info,
            "BACKTEST_GATE_GEMINI_OK eval_id=" + evalId +
            " num_ranges=" + std::to_string(out->ranges.size()) +
            " notes=" + quoteStr(out->notes.substr(0, 80)));
        putCached(cacheKey, *out);
    } else {
        const auto& f = std::get<IRangeProposer::Failure>(result);
        Logger::instance().log(LogLevel::Warning,
            "BACKTEST_GATE_GEMINI_INVALID eval_id=" + evalId +
            " reason=" + quoteStr(f.message));
    }

    return result;
}

}  // namespace backtest
