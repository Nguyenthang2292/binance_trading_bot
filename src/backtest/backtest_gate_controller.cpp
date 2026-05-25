#include "backtest/backtest_gate_controller.h"
#include "backtest/plateau_finder.h"
#include "backtest/walk_forward.h"
#include "logger.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace backtest {

namespace {

using json = nlohmann::json;

int computeSlowestParamMax(const StrategyParamSpec& spec) {
    int slowest = 1;
    for (const auto& r : spec.defaults) {
        // Treat integer params as candidate "period" knobs.
        if (r.isInteger) {
            slowest = std::max(slowest, static_cast<int>(std::floor(r.max)));
        }
    }
    return slowest;
}

bool deadlineExceeded(std::chrono::steady_clock::time_point deadline) {
    return std::chrono::steady_clock::now() > deadline;
}

DropDetail makeDrop(DropReason r, std::string msg, int combos,
                    std::chrono::steady_clock::time_point start) {
    DropDetail d;
    d.reason = r;
    d.message = std::move(msg);
    d.combosEvaluated = combos;
    d.wallTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return d;
}

DropReason mapRangeFailure(IRangeProposer::FailureReason reason) {
    switch (reason) {
        case IRangeProposer::FailureReason::Unavailable:
            return DropReason::GeminiUnavailable;
        case IRangeProposer::FailureReason::Timeout:
            return DropReason::GeminiTimeout;
        case IRangeProposer::FailureReason::InvalidResponse:
            return DropReason::GeminiInvalidResponse;
        case IRangeProposer::FailureReason::InternalError:
            return DropReason::InternalError;
    }
    return DropReason::InternalError;
}

std::string dropReasonStr(DropReason r) {
    switch (r) {
        case DropReason::NotEligible:           return "NotEligible";
        case DropReason::InsufficientData:      return "InsufficientData";
        case DropReason::GeminiUnavailable:     return "GeminiUnavailable";
        case DropReason::GeminiTimeout:         return "GeminiTimeout";
        case DropReason::GeminiInvalidResponse: return "GeminiInvalidResponse";
        case DropReason::ComboBudgetExhausted:  return "ComboBudgetExhausted";
        case DropReason::NoComboPassedFilter:   return "NoComboPassedFilter";
        case DropReason::NoPlateauFound:        return "NoPlateauFound";
        case DropReason::MajorityVoteFailed:    return "MajorityVoteFailed";
        case DropReason::DeadlineExceeded:      return "DeadlineExceeded";
        case DropReason::InternalError:         return "InternalError";
    }
    return "InternalError";
}

void logStructured(std::string_view event, const std::string& symbol,
                   const std::string& strategyId, const std::string& interval,
                   const json& extra = json::object()) {
    json payload = {
        {"event", event},
        {"symbol", symbol},
        {"strategy_id", strategyId},
        {"interval", interval},
    };
    if (extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            payload[it.key()] = it.value();
        }
    }
    Logger::instance().log(LogLevel::Info, payload.dump());
}

json paramPointToJson(const ParamPoint& point) {
    std::map<std::string, double> ordered(point.begin(), point.end());
    json j = json::object();
    for (const auto& [name, value] : ordered) {
        j[name] = value;
    }
    return j;
}

std::string paramPointKey(const ParamPoint& point) {
    return paramPointToJson(point).dump();
}

// ── Phase 7: score the grid ───────────────────────────────────────────────
//
// For each combo, runs the engine over every walk-forward fold (IS + OOS),
// applies the min-trades and OOS/IS Sortino filters, and accumulates per-combo
// scores. Aborts early on deadline; the caller maps that to DeadlineExceeded.
struct GridScoreOutcome {
    std::vector<ScoredPoint> scoredGrid;
    int combosEvaluated{0};
    int minTradesRejects{0};
    int oosIsRejects{0};
    bool deadlineHit{false};
    std::string deadlineLocation;
};

GridScoreOutcome scoreGrid(
    const IOptimizableStrategy& adapter,
    const BacktestEngine& engine,
    const std::vector<ParamPoint>& grid,
    const std::vector<WalkForwardFold>& folds,
    const BacktestGateFiltersConfig& filters,
    const BacktestGateRequest& req,
    std::chrono::steady_clock::time_point deadline) {

    GridScoreOutcome out;
    out.scoredGrid.reserve(grid.size());

    for (const auto& point : grid) {
        if (deadlineExceeded(deadline)) {
            out.deadlineHit = true;
            out.deadlineLocation = "mid-grid evaluation";
            return out;
        }

        double sumIS = 0.0;
        double sumOOS = 0.0;
        bool everyFoldPassed = true;

        for (const auto& fold : folds) {
            if (deadlineExceeded(deadline)) {
                out.deadlineHit = true;
                out.deadlineLocation = "mid-fold evaluation";
                return out;
            }
            const auto isStats = engine.runFold(
                adapter, req.symbol, req.interval, fold.inSample, point,
                req.baseConfig, req.originalDirection, req.symbolMeta);
            const auto oosStats = engine.runFold(
                adapter, req.symbol, req.interval, fold.outOfSample, point,
                req.baseConfig, req.originalDirection, req.symbolMeta);

            if (isStats.numTrades < filters.minTradesPerFold ||
                oosStats.numTrades < filters.minTradesPerFold) {
                out.minTradesRejects++;
                everyFoldPassed = false;
                break;
            }
            sumIS += isStats.sortino;
            sumOOS += oosStats.sortino;
        }

        ScoredPoint sp;
        sp.point = point;
        sp.passedFilters = false;
        sp.isSortino = 0.0;
        sp.oosSortino = 0.0;

        if (everyFoldPassed) {
            const double meanIS = sumIS / static_cast<double>(folds.size());
            const double meanOOS = sumOOS / static_cast<double>(folds.size());
            sp.isSortino = meanIS;
            sp.oosSortino = meanOOS;

            const bool finiteOk = std::isfinite(meanIS) && std::isfinite(meanOOS);
            const bool bothPositive = meanIS > 0.0 && meanOOS > 0.0;
            const double floor = std::max(
                filters.oosIsRatioThreshold * meanIS,
                filters.minOosSortino);
            if (finiteOk && bothPositive && meanOOS >= floor) {
                sp.passedFilters = true;
            } else {
                out.oosIsRejects++;
            }
        }
        out.scoredGrid.push_back(sp);
        out.combosEvaluated++;
    }

    return out;
}

// Memoized adapter call — keyed by serialized param point. Same evalKlines and
// baseConfig are used throughout the vote + pass phases, so a hash table avoids
// re-evaluating identical points.
const strategy::Signal& evaluateCached(
    const IOptimizableStrategy& adapter,
    const ParamPoint& point,
    const BacktestGateRequest& req,
    const std::vector<Kline>& evalKlines,
    std::unordered_map<std::string, strategy::Signal>& cache) {
    const std::string key = paramPointKey(point);
    auto it = cache.find(key);
    if (it == cache.end()) {
        auto [insertedIt, _] = cache.emplace(
            key,
            adapter.evaluateWith(req.symbol, req.interval, evalKlines, point, req.baseConfig));
        it = insertedIt;
    }
    return it->second;
}

// ── Phase 9: majority vote on signal bar T ─────────────────────────────────
struct VoteOutcome {
    int matching{0};
    int total{0};
};

VoteOutcome runVote(
    const IOptimizableStrategy& adapter,
    const std::vector<ParamPoint>& votePoints,
    const std::vector<Kline>& evalKlines,
    const BacktestGateRequest& req,
    std::unordered_map<std::string, strategy::Signal>& cache) {
    VoteOutcome out;
    for (const auto& point : votePoints) {
        const auto& voteSig = evaluateCached(adapter, point, req, evalKlines, cache);
        if (voteSig.direction == req.originalDirection &&
            voteSig.confidence >= req.baseConfig.minConfidence &&
            voteSig.atr > 0.0) {
            out.matching++;
        }
        out.total++;
    }
    return out;
}

// ── Phase 10: assemble the final PassResult ────────────────────────────────
PassResult buildPassResult(
    const IOptimizableStrategy& adapter,
    const PlateauResult& plateau,
    const VoteOutcome& vote,
    const std::vector<Kline>& evalKlines,
    const BacktestGateRequest& req,
    int combosEvaluated,
    std::chrono::steady_clock::time_point start,
    std::unordered_map<std::string, strategy::Signal>& cache) {

    PassResult pass;
    pass.direction = req.originalDirection;
    pass.optimizedParams = plateau.center;
    pass.centerSortinoIS = plateau.centerSortinoIS;
    pass.centerSortinoOOS = plateau.centerSortinoOOS;
    pass.plateauVotePass = vote.matching;
    pass.plateauVoteTotal = vote.total;
    pass.combosEvaluated = combosEvaluated;
    pass.wallTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Recompute ATR on full window through bar T using optimized params (cached).
    const auto& centerSig = evaluateCached(adapter, plateau.center, req, evalKlines, cache);
    pass.atr = centerSig.atr > 0.0 ? centerSig.atr : req.originalAtr;

    const auto slIt = plateau.center.find("sl_multiplier");
    const auto tpIt = plateau.center.find("tp_multiplier");
    pass.slMultiplier = slIt != plateau.center.end() ? slIt->second : req.baseConfig.slMultiplier;
    pass.tpMultiplier = tpIt != plateau.center.end() ? tpIt->second : req.baseConfig.tpMultiplier;
    pass.riskPct = req.baseConfig.riskPct;  // not tuned in v1.1

    if (pass.direction == strategy::Signal::Direction::Long) {
        pass.initialStopPrice = req.currentPrice - pass.slMultiplier * pass.atr;
    } else {
        pass.initialStopPrice = req.currentPrice + pass.slMultiplier * pass.atr;
    }

    return pass;
}

}  // namespace

BacktestGateController::BacktestGateController(
    IHistoricalWindowProvider& dataProvider,
    IRangeProposer& rangeProposer,
    std::unordered_map<std::string, std::unique_ptr<IOptimizableStrategy>> adapters,
    BacktestGateConfig cfg,
    BacktestEngine::Config engineCfg)
    : m_dataProvider(dataProvider),
      m_rangeProposer(rangeProposer),
      m_adapters(std::move(adapters)),
      m_cfg(std::move(cfg)),
      m_engineCfg(std::move(engineCfg)) {}

BacktestGateResult BacktestGateController::evaluate(const BacktestGateRequest& req) const {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(m_cfg.deadlineSeconds);
    int combosEvaluated = 0;

    logStructured("BACKTEST_GATE_ENTER", req.symbol, req.strategyId, req.interval);

    // Helper: emit a DROP structured event and return the drop.
    auto emitDrop = [&](DropReason r, std::string msg, int combos) -> BacktestGateResult {
        auto d = makeDrop(r, msg, combos, start);
        logStructured("BACKTEST_GATE_DROP", req.symbol, req.strategyId, req.interval, {
            {"reason", dropReasonStr(r)},
            {"message", msg},
            {"combos", combos},
            {"wall_ms", d.wallTime.count()},
        });
        return d;
    };

    // 1. Adapter lookup
    auto adapterIt = m_adapters.find(req.strategyId);
    if (adapterIt == m_adapters.end()) {
        return emitDrop(DropReason::NotEligible,
                        "no adapter for strategy '" + req.strategyId + "'", 0);
    }
    const auto& adapter = *adapterIt->second;

    // 2. Data window sizing
    const auto spec = adapter.spec(req.baseConfig);
    const int slowestMax = computeSlowestParamMax(spec);
    const int uncappedRequiredBars = std::max(
        m_cfg.data.windowMinCandles,
        m_cfg.data.windowSlowestMultiplier * slowestMax);
    const int requiredBars = m_cfg.data.windowMaxCandles > 0
        ? std::min(uncappedRequiredBars, m_cfg.data.windowMaxCandles)
        : uncappedRequiredBars;

    auto window = m_dataProvider.closedWindow(
        req.symbol, req.interval, requiredBars, req.signalBarOpenTime);

    auto restFailureEvent = [](const std::string& reason) -> std::string_view {
        if (reason == "timeout") {
            return "BACKTEST_GATE_REST_TIMEOUT";
        }
        if (reason.rfind("budget_exceeded", 0) == 0) {
            return "BACKTEST_GATE_REST_BUDGET_EXCEEDED";
        }
        if (reason == "signal_bar_missing") {
            return "BACKTEST_GATE_REST_SIGNAL_BAR_MISSING";
        }
        if (reason == "insufficient_history") {
            return "BACKTEST_GATE_REST_INSUFFICIENT_HISTORY";
        }
        return "BACKTEST_GATE_REST_ERROR";
    };

    if (!window.sufficient) {
        if (window.source == "rest") {
            logStructured(restFailureEvent(window.errorReason), req.symbol, req.strategyId, req.interval, {
                {"error_reason", window.errorReason},
                {"pages_used", window.restPagesUsed},
                {"wall_time_ms", window.restWallTimeMs.count()},
                {"available_bars", window.availableBars},
                {"required_bars", window.requiredBars},
            });
        }
        return emitDrop(DropReason::InsufficientData,
                        "have " + std::to_string(window.availableBars) +
                        " bars, need " + std::to_string(window.requiredBars), 0);
    }
    if (window.source == "rest") {
        logStructured("BACKTEST_GATE_REST_BACKFILL_OK", req.symbol, req.strategyId, req.interval, {
            {"pages_used", window.restPagesUsed},
            {"wall_time_ms", window.restWallTimeMs.count()},
            {"available_bars", window.availableBars},
            {"required_bars", window.requiredBars},
            {"cache_writeback_failed", window.errorReason == "cache_writeback_failed"},
        });
    }
    if (!window.closedKlines.empty()) {
        logStructured("BACKTEST_GATE_DATA_READY", req.symbol, req.strategyId, req.interval, {
            {"history_source", m_cfg.data.historySource},
            {"provider_source", window.source},
            {"window_size", window.closedKlines.size()},
            {"required_bars", requiredBars},
            {"uncapped_required_bars", uncappedRequiredBars},
            {"window_max_candles", m_cfg.data.windowMaxCandles},
            {"available_bars", window.availableBars},
            {"first_bar", window.closedKlines.front().openTime},
            {"last_bar", window.closedKlines.back().openTime},
        });
    }

    if (deadlineExceeded(deadline)) {
        return emitDrop(DropReason::DeadlineExceeded, "after data fetch", 0);
    }

    // 3. Partition
    auto partitions = PartitionBuilder::build(
        window.closedKlines, m_cfg.walkForward.promptContextFraction);
    if (!partitions.valid) {
        return emitDrop(DropReason::InsufficientData, "partitions invalid after split", 0);
    }

    // 4. Range proposal
    RangeProposalRequest proposalReq;
    proposalReq.symbol = req.symbol;
    proposalReq.interval = req.interval;
    proposalReq.strategyId = req.strategyId;
    proposalReq.tunableParams = spec.tunableParams;
    proposalReq.defaultRanges = spec.defaults;
    proposalReq.constraints = spec.constraints;
    proposalReq.currentValues = spec.currentValues;
    proposalReq.baseAtrPeriod = req.baseConfig.atrPeriod;
    proposalReq.baseMinConfidence = req.baseConfig.minConfidence;
    proposalReq.signalDirection = req.originalDirection;
    proposalReq.signalBarOpenTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            req.signalBarOpenTime.time_since_epoch()).count();
    proposalReq.maxTotalCombos = m_cfg.budget.maxTotalCombos;
    proposalReq.promptContext = partitions.promptContext;
    proposalReq.deadline = deadline;

    auto proposalResult = m_rangeProposer.propose(proposalReq);
    if (const auto* failure = std::get_if<IRangeProposer::Failure>(&proposalResult)) {
        return emitDrop(mapRangeFailure(failure->reason), failure->message, 0);
    }
    const auto& proposal = std::get<IRangeProposer::Output>(proposalResult);
    logStructured("BACKTEST_GATE_GEMINI_OK", req.symbol, req.strategyId, req.interval, {
        {"ranges_count", proposal.ranges.size()},
        {"prompt_cutoff_bar", partitions.promptContext.empty() ? 0LL : partitions.promptContext.back().openTime},
    });

    if (deadlineExceeded(deadline)) {
        return emitDrop(DropReason::DeadlineExceeded, "after range proposal", 0);
    }

    // 5. Grid + budget clamp
    auto ranges = proposal.ranges;
    if (!ParameterSpace::clampToBudget(ranges, spec.constraints, m_cfg.budget.maxTotalCombos)) {
        return emitDrop(DropReason::ComboBudgetExhausted,
                        "grid exceeds max_total_combos even after step widening", 0);
    }
    auto grid = ParameterSpace::grid(ranges, spec.constraints);
    if (grid.empty()) {
        return emitDrop(DropReason::NoComboPassedFilter, "grid empty after constraint filtering", 0);
    }

    // 6. Walk-forward folds (over calibration only)
    auto folds = WalkForwardSplitter::split(
        partitions.calibrationWindow,
        m_cfg.walkForward.folds,
        m_cfg.walkForward.isFraction);
    if (folds.empty()) {
        return emitDrop(DropReason::InsufficientData,
                        "walk-forward could not produce folds from calibration window", 0);
    }
    logStructured("BACKTEST_GATE_GRID_BUILT", req.symbol, req.strategyId, req.interval, {
        {"combos_after_clamp", grid.size()},
        {"folds", folds.size()},
    });

    // Fold IS must have at least slowestMax+1 bars so the slowest indicator
    // can produce a valid first value on bar index 0.
    const int minFoldBars = slowestMax + 1;
    for (const auto& f : folds) {
        if (static_cast<int>(f.inSample.size()) < minFoldBars) {
            return emitDrop(DropReason::InsufficientData,
                            "fold IS has " + std::to_string(f.inSample.size()) +
                            " bars, need >= " + std::to_string(minFoldBars) +
                            " (slowest param max=" + std::to_string(slowestMax) + ")", 0);
        }
    }

    // 7. Score grid across all folds (delegated)
    BacktestEngine engine(m_engineCfg);
    auto score = scoreGrid(adapter, engine, grid, folds, m_cfg.filters, req, deadline);
    combosEvaluated = score.combosEvaluated;
    if (score.deadlineHit) {
        return emitDrop(DropReason::DeadlineExceeded, score.deadlineLocation, combosEvaluated);
    }

    const auto survivorCount = static_cast<int>(std::count_if(
        score.scoredGrid.begin(),
        score.scoredGrid.end(),
        [](const ScoredPoint& sp) { return sp.passedFilters; }));
    logStructured("BACKTEST_GATE_FILTER_DONE", req.symbol, req.strategyId, req.interval, {
        {"combos_surviving", survivorCount},
        {"min_trades_rejects", score.minTradesRejects},
        {"oos_is_rejects", score.oosIsRejects},
    });
    if (survivorCount == 0) {
        return emitDrop(DropReason::NoComboPassedFilter,
                        "no combo survived filters", combosEvaluated);
    }

    if (deadlineExceeded(deadline)) {
        return emitDrop(DropReason::DeadlineExceeded, "before plateau", combosEvaluated);
    }

    // 8. Plateau finding
    auto plateauOpt = PlateauFinder::find(
        score.scoredGrid, spec.constraints,
        m_cfg.plateau.neighborhoodRadius,
        m_cfg.plateau.maxNeighborhoodSize,
        m_cfg.plateau.minPassFraction);
    if (!plateauOpt) {
        return emitDrop(DropReason::NoPlateauFound,
                        "no robust plateau detected", combosEvaluated);
    }
    auto plateau = *plateauOpt;
    logStructured("BACKTEST_GATE_PLATEAU", req.symbol, req.strategyId, req.interval, {
        {"plateau_size", plateau.survivors.size()},
        {"center_params", paramPointToJson(plateau.center)},
        {"center_oos_sortino", plateau.centerSortinoOOS},
    });

    // 9. Majority vote on bar T (delegated)
    auto votePoints = plateau.survivors;
    const std::string centerKey = paramPointKey(plateau.center);
    const bool centerInSurvivors = std::any_of(
        votePoints.begin(), votePoints.end(),
        [&](const ParamPoint& p) { return paramPointKey(p) == centerKey; });
    if (!centerInSurvivors) {
        votePoints.push_back(plateau.center);
    }

    const auto& evalKlines = window.closedKlines;  // ends at bar T
    std::unordered_map<std::string, strategy::Signal> evalCache;
    const auto vote = runVote(adapter, votePoints, evalKlines, req, evalCache);

    const double voteRatio = vote.total > 0
        ? static_cast<double>(vote.matching) / static_cast<double>(vote.total)
        : 0.0;
    logStructured("BACKTEST_GATE_VOTE", req.symbol, req.strategyId, req.interval, {
        {"vote_pass", vote.matching},
        {"vote_total", vote.total},
        {"threshold", m_cfg.vote.thresholdFraction},
        {"decision", voteRatio >= m_cfg.vote.thresholdFraction ? "pass" : "drop"},
    });

    if (vote.total == 0 || voteRatio < m_cfg.vote.thresholdFraction) {
        return emitDrop(DropReason::MajorityVoteFailed,
                        "vote " + std::to_string(vote.matching) + "/" +
                        std::to_string(vote.total), combosEvaluated);
    }

    // 10. Build pass result (delegated)
    auto pass = buildPassResult(
        adapter, plateau, vote, evalKlines, req, combosEvaluated, start, evalCache);

    logStructured("BACKTEST_GATE_PASS", req.symbol, req.strategyId, req.interval, {
        {"combos", pass.combosEvaluated},
        {"vote_pass", pass.plateauVotePass},
        {"vote_total", pass.plateauVoteTotal},
        {"oos_sortino", pass.centerSortinoOOS},
        {"wall_ms", pass.wallTime.count()},
    });
    return pass;
}

}  // namespace backtest
