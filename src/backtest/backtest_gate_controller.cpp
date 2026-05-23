#include "backtest/backtest_gate_controller.h"
#include "backtest/plateau_finder.h"
#include "backtest/walk_forward.h"
#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace backtest {

namespace {

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
                   const std::string& extra = "") {
    std::ostringstream o;
    o << R"({"event":")" << event << R"(","symbol":")" << symbol
      << R"(","strategy_id":")" << strategyId
      << R"(","interval":")" << interval << '"';
    if (!extra.empty()) o << ',' << extra;
    o << '}';
    Logger::instance().log(LogLevel::Info, o.str());
}

std::string paramPointJson(const ParamPoint& point) {
    std::ostringstream o;
    o << '{';
    bool first = true;
    for (const auto& [name, value] : point) {
        if (!first) {
            o << ',';
        }
        first = false;
        o << '"' << name << "\":" << value;
    }
    o << '}';
    return o.str();
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
        std::ostringstream extra;
        extra << R"("reason":")" << dropReasonStr(r) << '"'
              << R"(,"message":")" << msg << '"'
              << R"(,"combos":)" << combos
              << R"(,"wall_ms":)" << d.wallTime.count();
        logStructured("BACKTEST_GATE_DROP", req.symbol, req.strategyId, req.interval, extra.str());
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
    const int requiredBars = std::max(
        m_cfg.data.windowMinCandles,
        m_cfg.data.windowSlowestMultiplier * slowestMax);

    auto window = m_dataProvider.closedWindow(
        req.symbol, req.interval, requiredBars, req.signalBarOpenTime);
    if (!window.sufficient) {
        return emitDrop(DropReason::InsufficientData,
                        "have " + std::to_string(window.availableBars) +
                        " bars, need " + std::to_string(window.requiredBars), 0);
    }
    if (!window.closedKlines.empty()) {
        std::ostringstream extra;
        extra << R"("history_source":")" << m_cfg.data.historySource << '"'
              << R"(,"window_size":)" << window.closedKlines.size()
              << R"(,"required_bars":)" << requiredBars
              << R"(,"available_bars":)" << window.availableBars
              << R"(,"first_bar":)" << window.closedKlines.front().openTime
              << R"(,"last_bar":)" << window.closedKlines.back().openTime;
        logStructured("BACKTEST_GATE_DATA_READY", req.symbol, req.strategyId, req.interval, extra.str());
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
    {
        std::ostringstream extra;
        extra << R"("ranges_count":)" << proposal.ranges.size()
              << R"(,"prompt_cutoff_bar":)"
              << (partitions.promptContext.empty() ? 0 : partitions.promptContext.back().openTime);
        logStructured("BACKTEST_GATE_GEMINI_OK", req.symbol, req.strategyId, req.interval, extra.str());
    }

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
    {
        std::ostringstream extra;
        extra << R"("combos_after_clamp":)" << grid.size()
              << R"(,"folds":)" << folds.size();
        logStructured("BACKTEST_GATE_GRID_BUILT", req.symbol, req.strategyId, req.interval, extra.str());
    }

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

    // 7. Backtest each combo across all folds
    BacktestEngine engine(m_engineCfg);
    std::vector<ScoredPoint> scoredGrid;
    scoredGrid.reserve(grid.size());
    int minTradesRejects = 0;
    int oosIsRejects = 0;

    for (const auto& point : grid) {
        if (deadlineExceeded(deadline)) {
            return emitDrop(DropReason::DeadlineExceeded, "mid-grid evaluation", combosEvaluated);
        }

        double sumIS = 0.0;
        double sumOOS = 0.0;
        bool everyFoldPassed = true;

        for (const auto& fold : folds) {
            if (deadlineExceeded(deadline)) {
                return emitDrop(DropReason::DeadlineExceeded, "mid-fold evaluation", combosEvaluated);
            }
            const auto isStats = engine.runFold(
                adapter, req.symbol, req.interval, fold.inSample, point,
                req.baseConfig, req.originalDirection, req.symbolMeta);
            const auto oosStats = engine.runFold(
                adapter, req.symbol, req.interval, fold.outOfSample, point,
                req.baseConfig, req.originalDirection, req.symbolMeta);

            if (isStats.numTrades < m_cfg.filters.minTradesPerFold ||
                oosStats.numTrades < m_cfg.filters.minTradesPerFold) {
                minTradesRejects++;
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
                m_cfg.filters.oosIsRatioThreshold * meanIS,
                m_cfg.filters.minOosSortino);
            if (finiteOk && bothPositive && meanOOS >= floor) {
                sp.passedFilters = true;
            } else {
                oosIsRejects++;
            }
        }
        scoredGrid.push_back(sp);
        combosEvaluated++;
    }

    const auto survivorCount = static_cast<int>(std::count_if(
        scoredGrid.begin(),
        scoredGrid.end(),
        [](const ScoredPoint& sp) { return sp.passedFilters; }));
    {
        std::ostringstream extra;
        extra << R"("combos_surviving":)" << survivorCount
              << R"(,"min_trades_rejects":)" << minTradesRejects
              << R"(,"oos_is_rejects":)" << oosIsRejects;
        logStructured("BACKTEST_GATE_FILTER_DONE", req.symbol, req.strategyId, req.interval, extra.str());
    }
    bool anySurvivor = survivorCount > 0;
    if (!anySurvivor) {
        return emitDrop(DropReason::NoComboPassedFilter,
                        "no combo survived filters", combosEvaluated);
    }

    if (deadlineExceeded(deadline)) {
        return emitDrop(DropReason::DeadlineExceeded, "before plateau", combosEvaluated);
    }

    // 8. Plateau finding
    auto plateauOpt = PlateauFinder::find(
        scoredGrid, spec.constraints,
        m_cfg.plateau.neighborhoodRadius,
        m_cfg.plateau.maxNeighborhoodSize,
        m_cfg.plateau.minPassFraction);
    if (!plateauOpt) {
        return emitDrop(DropReason::NoPlateauFound,
                        "no robust plateau detected", combosEvaluated);
    }
    auto plateau = *plateauOpt;
    {
        std::ostringstream extra;
        extra << R"("plateau_size":)" << plateau.survivors.size()
              << R"(,"center_params":)" << paramPointJson(plateau.center)
              << R"(,"center_oos_sortino":)" << plateau.centerSortinoOOS;
        logStructured("BACKTEST_GATE_PLATEAU", req.symbol, req.strategyId, req.interval, extra.str());
    }

    // 9. Majority vote on bar T (uses full window including signal bar)
    auto votePoints = plateau.survivors;
    const bool centerInSurvivors = std::any_of(
        votePoints.begin(), votePoints.end(),
        [&](const ParamPoint& p) { return p == plateau.center; });
    if (!centerInSurvivors) {
        votePoints.push_back(plateau.center);
    }

    const auto& evalKlines = window.closedKlines;  // ends at bar T
    int totalVotes = 0;
    int matchingVotes = 0;
    for (const auto& point : votePoints) {
        const auto voteSig = adapter.evaluateWith(
            req.symbol, req.interval, evalKlines, point, req.baseConfig);
        if (voteSig.direction == req.originalDirection &&
            voteSig.confidence >= req.baseConfig.minConfidence &&
            voteSig.atr > 0.0) {
            matchingVotes++;
        }
        totalVotes++;
    }
    {
        const double voteRatio = totalVotes > 0
            ? static_cast<double>(matchingVotes) / static_cast<double>(totalVotes)
            : 0.0;
        std::ostringstream extra;
        extra << R"("vote_pass":)" << matchingVotes
              << R"(,"vote_total":)" << totalVotes
              << R"(,"threshold":)" << m_cfg.vote.thresholdFraction
              << R"(,"decision":")"
              << (voteRatio >= m_cfg.vote.thresholdFraction ? "pass" : "drop")
              << '"';
        logStructured("BACKTEST_GATE_VOTE", req.symbol, req.strategyId, req.interval, extra.str());
    }

    if (totalVotes == 0 ||
        static_cast<double>(matchingVotes) / totalVotes < m_cfg.vote.thresholdFraction) {
        return emitDrop(DropReason::MajorityVoteFailed,
                        "vote " + std::to_string(matchingVotes) + "/" +
                        std::to_string(totalVotes), combosEvaluated);
    }

    // 10. Build pass result
    PassResult pass;
    pass.direction = req.originalDirection;
    pass.optimizedParams = plateau.center;
    pass.centerSortinoIS = plateau.centerSortinoIS;
    pass.centerSortinoOOS = plateau.centerSortinoOOS;
    pass.plateauVotePass = matchingVotes;
    pass.plateauVoteTotal = totalVotes;
    pass.combosEvaluated = combosEvaluated;
    pass.wallTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Recompute ATR on full window through bar T using optimized atr_period
    const auto atrIt = plateau.center.find("atr_period");
    const int finalAtrPeriod = atrIt != plateau.center.end()
        ? static_cast<int>(atrIt->second)
        : req.baseConfig.atrPeriod;
    const auto centerSig = adapter.evaluateWith(
        req.symbol, req.interval, evalKlines, plateau.center, req.baseConfig);
    pass.atr = centerSig.atr > 0.0 ? centerSig.atr : req.originalAtr;
    (void)finalAtrPeriod;  // adapter already used it via params

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

    {
        std::ostringstream extra;
        extra << R"("combos":)" << pass.combosEvaluated
              << R"(,"vote_pass":)" << pass.plateauVotePass
              << R"(,"vote_total":)" << pass.plateauVoteTotal
              << R"(,"oos_sortino":)" << pass.centerSortinoOOS
              << R"(,"wall_ms":)" << pass.wallTime.count();
        logStructured("BACKTEST_GATE_PASS", req.symbol, req.strategyId, req.interval, extra.str());
    }
    return pass;
}

}  // namespace backtest
