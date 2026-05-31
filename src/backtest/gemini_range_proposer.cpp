/**
 * @file gemini_range_proposer.cpp
 * @brief Python subprocess-backed implementation for parameter range proposal.
 */

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
#include <unordered_set>
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

/**
 * @brief Generate a random evaluation identifier string.
 *
 * The identifier is formatted similarly to a UUID (hex with dashes)
 * and is used to create per-evaluation runtime directories.
 *
 * @return A hexadecimal string serving as a unique eval id.
 */

std::string fmt6(double v) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(6) << v;
    return o.str();
}

/**
 * @brief Format a double with fixed six decimal places.
 *
 * Used when serializing numeric values into cache keys and logs
 * where consistent decimal width improves comparability.
 *
 * @param v The value to format.
 * @return The formatted string.
 */

std::string quoteStr(std::string_view v) {
    std::ostringstream o;
    o << std::quoted(std::string(v));
    return o.str();
}

/**
 * @brief Quote a string for safe logging or command construction.
 *
 * Converts a string_view to a quoted std::string using
 * std::quoted for consistent escaping behavior.
 *
 * @param v The input string view.
 * @return A quoted std::string.
 */

void appendSortedCurrentValues(
    std::ostringstream& key,
    const std::unordered_map<std::string, double>& currentValues) {
    std::vector<std::pair<std::string, double>> entries(
        currentValues.begin(), currentValues.end());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    for (const auto& [name, value] : entries) {
        key << "|cv:" << name << "=" << fmt6(value);
    }
}

/**
 * @brief Append the sorted `currentValues` map into a key stream.
 *
 * The map is sorted by key name to ensure deterministic key
 * generation for caching. Each entry is formatted as `|cv:name=val`.
 *
 * @param key Output stream to which formatted entries are appended.
 * @param currentValues Map of current parameter values.
 */

void logSubprocess(std::string_view diag, LogLevel level = LogLevel::Subprocess) {
    std::istringstream lines{std::string(diag)};
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty())
            Logger::instance().log(level, "backtest_proposer.py | " + line);
    }
}

/**
 * @brief Log subprocess output line-by-line.
 *
 * Splits multi-line diagnostics and forwards each non-empty line
 * to the global Logger instance with the provided log level.
 *
 * @param diag Multi-line diagnostic text.
 * @param level The log level to use when emitting lines.
 */

IRangeProposer::Failure failure(
    IRangeProposer::FailureReason reason,
    std::string message) {
    return IRangeProposer::Failure{
        .reason = reason,
        .message = std::move(message),
    };
}

/**
 * @brief Convenience helper to build a Failure result.
 *
 * @param reason The categorized failure reason.
 * @param message A human-readable message describing the failure.
 * @return An IRangeProposer::Failure instance.
 */

IRangeProposer::FailureReason failureReasonFromPythonError(std::string_view code) {
    if (code == "invalid_input" || code == "invalid_response") {
        return IRangeProposer::FailureReason::InvalidResponse;
    }
    if (code == "timeout") {
        return IRangeProposer::FailureReason::Timeout;
    }
    return IRangeProposer::FailureReason::Unavailable;
}

/**
 * @brief Map a Python-side error code to a FailureReason enum.
 *
 * Recognizes a small set of string error codes produced by the
 * Python proposer module and maps them to internal failure kinds.
 * Unknown codes map to `Unavailable`.
 *
 * @param code Error code string from Python output.
 * @return Mapped FailureReason.
 */

#ifdef _WIN32
std::string quoteWinArg(std::string_view v) {
    bool needs = v.empty();
    for (char c : v) if (c == ' ' || c == '\t' || c == '"') { needs = true; break; }
    if (!needs) return std::string(v);
    std::string out; out.reserve(v.size() + 2); out.push_back('"');
    for (char c : v) { if (c == '"') out += "\\\""; else out.push_back(c); }
    out.push_back('"'); return out;
}

/**
 * @brief Quote a Windows command-line argument when necessary.
 *
 * Adds surrounding quotes and escapes internal quotes when the
 * argument contains whitespace or quote characters.
 *
 * @param v Input argument string view.
 * @return Properly quoted string for CreateProcess usage.
 */

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

/**
 * @brief Read all currently-available data from a Windows pipe handle.
 *
 * Appends any available bytes to `out` without blocking.
 *
 * @param h The pipe HANDLE to read from.
 * @param out String buffer to append read bytes into.
 */
#else
bool setNB(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    return f >= 0 && fcntl(fd, F_SETFL, f | O_NONBLOCK) == 0;
}
/**
 * @brief Set a POSIX file descriptor to non-blocking mode.
 *
 * @param fd File descriptor to modify.
 * @return True on success, false otherwise.
 */
void drainFd(int fd, std::string& out, bool& open) {
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

/**
 * @brief Drain a non-blocking file descriptor into a string buffer.
 *
 * Reads repeatedly until the descriptor would block or is closed,
 * appending data to `out`. If the descriptor reaches EOF it will be
 * closed and `open` set to false.
 *
 * @param fd File descriptor to read from.
 * @param out Output buffer to append read bytes.
 * @param open In/out flag indicating whether the descriptor is still open.
 */
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

/**
 * @brief Compute lightweight aggregates from a Kline prompt context.
 *
 * This function extracts features used by the Gemini proposer such as
 * ATR percentage, 30-day return, average volume (USD proxy), trend
 * direction (up/down/flat), and realized volatility over the window.
 * It is intentionally conservative and only uses the provided `ctx`.
 *
 * @param ctx Time-ordered vector of Kline objects (oldest first).
 * @return A PromptContextAggregates structure populated with metrics.
 */

// ── GeminiRangeProposer ────────────────────────────────────────────────────

GeminiRangeProposer::GeminiRangeProposer(BacktestGateGeminiConfig cfg, BacktestGateCacheConfig cacheCfg)
    : m_cfg(std::move(cfg)), m_cacheCfg(std::move(cacheCfg)) {}

/**
 * @brief Construct a GeminiRangeProposer instance.
 *
 * @param cfg Configuration controlling the Gemini/Python subprocess and timeouts.
 * @param cacheCfg Caching policy for previously-computed proposals.
 */

// ── Cache helpers ──────────────────────────────────────────────────────────

std::string GeminiRangeProposer::buildCacheKey(
    const RangeProposalRequest& req,
    const PromptContextAggregates& aggs) const
{
    // key v1.3|symbol|strategyId|interval|signal-context|prompt-aggs|base-config|current-values
    std::ostringstream k;
    k << "v1.3"
      << "|" << req.symbol
      << "|" << req.strategyId
      << "|" << req.interval
      << "|dir=" << (req.signalDirection == strategy::Signal::Direction::Long ? "long" :
                      req.signalDirection == strategy::Signal::Direction::Short ? "short" : "none")
      << "|asof=" << req.signalBarOpenTimeMs
      << "|ret=" << fmt6(aggs.ret30dPct)
      << "|atr=" << fmt6(aggs.atrPctCurrent)
      << "|trend=" << aggs.trendDirection
      << "|n=" << aggs.numCandles
      << "|base_atr=" << req.baseAtrPeriod
      << "|base_min_conf=" << fmt6(req.baseMinConfidence)
      << "|model=" << m_cfg.model;
    appendSortedCurrentValues(k, req.currentValues);
    return k.str();
}

/**
 * @brief Build a deterministic cache key for a range proposal request.
 *
 * The key encodes symbol, strategy id, interval, prompt aggregates,
 * base configuration and current parameter values (sorted) so equal
 * semantic requests produce identical keys.
 *
 * @param req The original range proposal request.
 * @param aggs Aggregates computed from the prompt context.
 * @return A string suitable as a cache key.
 */

void GeminiRangeProposer::evictExpiredLocked() const {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_cache.begin(); it != m_cache.end(); ) {
        if (it->second.expiresAt <= now) it = m_cache.erase(it);
        else ++it;
    }
}

/**
 * @brief Remove expired entries from the in-memory cache.
 *
 * Must be called while holding the cache mutex; it safely erases
 * entries whose expiration timestamp is in the past.
 */

std::optional<IRangeProposer::Output> GeminiRangeProposer::getCached(const std::string& key) const {
    std::lock_guard lock(m_cacheMutex);
    auto it = m_cache.find(key);
    if (it == m_cache.end()) return std::nullopt;
    if (it->second.expiresAt <= std::chrono::steady_clock::now()) {
        m_cache.erase(it); return std::nullopt;
    }
    return it->second.result;
}

/**
 * @brief Retrieve a cached proposal if available and not expired.
 *
 * @param key Cache key previously produced by `buildCacheKey`.
 * @return Optional Output if present and valid, std::nullopt otherwise.
 */

void GeminiRangeProposer::putCached(const std::string& key, const Output& result) const {
    const auto ttl = std::chrono::seconds(std::max(1, m_cacheCfg.ttlSeconds));
    const auto maxEntries = static_cast<size_t>(std::max(1, m_cacheCfg.maxEntries));
    std::lock_guard lock(m_cacheMutex);
    const auto expiresAt = std::chrono::steady_clock::now() + ttl;

    auto existing = m_cache.find(key);
    if (existing != m_cache.end()) {
        existing->second = CachedResult{result, expiresAt};
        return;
    }

    if (m_cache.size() >= maxEntries) {
        evictExpiredLocked();
    }
    if (m_cache.size() >= maxEntries) {
        const auto oldest = std::min_element(
            m_cache.begin(), m_cache.end(),
            [](const auto& a, const auto& b) {
                return a.second.expiresAt < b.second.expiresAt;
            });
        if (oldest != m_cache.end()) {
            m_cache.erase(oldest);
        }
    }

    m_cache.emplace(key, CachedResult{result, expiresAt});
}

/**
 * @brief Store a proposal result in the in-memory cache.
 *
 * Observes `m_cacheCfg` for TTL and maximum entries. If the cache
 * is full the oldest entry is evicted.
 *
 * @param key Cache key.
 * @param result Proposal output to cache.
 */

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

/**
 * @brief Remove stale runtime evaluation directories once per process.
 *
 * The operation is guarded to run only once; it deletes directories
 * named `eval-<id>` older than the configured TTL.
 */

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

    // budget — max grid size and outer timeout for the Python proposer.
    j["budget"] = {
        {"max_total_combos", req.maxTotalCombos},
        {"timeout_seconds", m_cfg.timeoutSeconds},
    };

    return j.dump();
}

/**
 * @brief Build the JSON payload passed to the Python proposer.
 *
 * The JSON includes evaluation metadata, current parameter values,
 * prompt context aggregates, default ranges, constraints, signal
 * context, and a budget object describing time/combination limits.
 *
 * @param req The range proposal request describing tunables.
 * @param aggs Aggregates computed from prompt context.
 * @param evalId Unique evaluation identifier to embed in payload.
 * @return Serialized JSON string.
 */

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
        readAvailable(outR, diag);
        readAvailable(errR, diag);
        const DWORD w = WaitForSingleObject(pi.hProcess, 50);
        if (w == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            readAvailable(outR, diag);
            readAvailable(errR, diag);
            if (!diag.empty()) logSubprocess(diag, LogLevel::Warning);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            CloseHandle(outR); CloseHandle(errR);
            throw std::runtime_error("backtest_proposer subprocess timeout");
        }
    }
    readAvailable(outR, diag);
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
        if (!m_cfg.workingDirectory.empty() && chdir(m_cfg.workingDirectory.c_str()) != 0) {
            const std::string err = "chdir failed: " + std::string(std::strerror(errno)) + "\n";
            (void)::write(STDERR_FILENO, err.data(), err.size());
            _exit(127);
        }
        std::vector<std::string> ownedArgs = {pythonPath, "-m", moduleName, inputPath, outputPath};
        std::vector<char*> argv;
        for (auto& a : ownedArgs) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        const std::string err = "execvp failed: " + std::string(std::strerror(errno)) + "\n";
        (void)::write(STDERR_FILENO, err.data(), err.size());
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
    if (!diag.empty()) {
        const std::string prefixed = timedOut ? "[timeout] " + diag : diag;
        logSubprocess(prefixed, timedOut ? LogLevel::Warning : LogLevel::Subprocess);
    }
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

/**
 * @brief Run the configured Python module as a subprocess.
 *
 * Writes diagnostics to the Logger and returns the contents of the
 * Python module's `outputPath` file. Throws on errors or timeouts.
 * Platform-specific implementations handle process creation and
 * non-blocking I/O semantics.
 *
 * @param inputPath Path to the JSON input file for the module.
 * @param outputPath Path the module will write output to.
 * @param timeoutSeconds Maximum allowed runtime for the subprocess.
 * @return Contents of the output file as a string.
 */

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

        std::unordered_set<std::string> seen;
        for (const auto& r : j.at("ranges")) {
            const std::string name = r.at("name").get<std::string>();
            // Validate name is in tunableParams
            if (std::find(tunableParams.begin(), tunableParams.end(), name) == tunableParams.end()) {
                throw std::runtime_error("unknown param in output: " + name);
            }
            if (!seen.insert(name).second) {
                throw std::runtime_error("duplicate param in output: " + name);
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
        if (seen.size() != tunableParams.size()) {
            throw std::runtime_error("missing tunable params in output");
        }
        return out;
    } catch (const std::exception& e) {
        Logger::instance().log(LogLevel::Warning,
            "backtest_proposer output parse failed: " + std::string(e.what()));
        return failure(IRangeProposer::FailureReason::InvalidResponse, e.what());
    }
}

/**
 * @brief Parse the JSON output produced by the Python proposer.
 *
 * Validates the structure, converts entries to ParamRange values,
 * and performs sanity checks (finite numbers, min<=max, step>0).
 * On parse or validation failure a Failure result is returned.
 *
 * @param rawJson Raw JSON string produced by the Python process.
 * @param tunableParams List of expected parameter names.
 * @return Either an Output with ranges or a Failure describing the error.
 */

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

/**
 * @brief High-level entry point to produce a parameter range proposal.
 *
 * This convenience wrapper forwards to `proposeWithPartitions`, using
 * the request's deadline when present or a computed timeout otherwise.
 *
 * @param req Range proposal request.
 * @return Result containing either `Output` or `Failure`.
 */

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

    if (std::chrono::steady_clock::now() >= deadline) {
        return failure(IRangeProposer::FailureReason::Timeout, "deadline already expired");
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
    if (now >= deadline) {
        return failure(IRangeProposer::FailureReason::Timeout, "deadline already expired");
    }
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

        std::error_code cleanupEc;
        fs::remove_all(rtDir, cleanupEc);
        if (cleanupEc) {
            Logger::instance().log(
                LogLevel::Warning,
                "backtest_proposer eval-dir cleanup failed eval_id=" + evalId +
                    " path=" + quoteStr(rtDir.string()) +
                    " error=" + quoteStr(cleanupEc.message()));
        }
    } else {
        const auto& f = std::get<IRangeProposer::Failure>(result);
        Logger::instance().log(LogLevel::Warning,
            "BACKTEST_GATE_GEMINI_INVALID eval_id=" + evalId +
            " reason=" + quoteStr(f.message));
    }

    return result;
}

/**
 * @brief Full implementation of range proposal using provided prompt context.
 *
 * Steps:
 *  - compute aggregates from `promptContext`
 *  - check in-memory cache
 *  - prepare eval directory and write input JSON
 *  - run the Python module and parse its output
 *  - cache successful outputs and clean up runtime directory.
 *
 * @param req The original proposal request.
 * @param promptContext Slice of Kline used to compute prompt aggregates.
 * @param deadline Absolute time point by which processing must finish.
 * @return Either an Output with ranges or a Failure describing the issue.
 */

}  // namespace backtest
