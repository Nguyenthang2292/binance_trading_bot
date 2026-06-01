#include "strategy/istrategy.h"
#include "strategy/strategy_config.h"

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#elif __has_include(<winsqlite/winsqlite3.h>)
#include <winsqlite/winsqlite3.h>
#else
#error "SQLite headers not found"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

const std::vector<std::string> kDefaultIntervals{"1h"};

enum class ConfidenceMode {
    Rank,
    Absolute,
};

enum class FailMode {
    Open,
    Closed,
};

struct QlibSignalParams {
    std::string source{"sqlite"};
    std::string dbPath{"data/qlib_predictions.db"};
    std::string modelId{"lightgbm_1h_v1"};
    int maxArtifactAgeSeconds{7200};
    int maxDataAgeSeconds{3600};
    double longThreshold{0.003};
    double shortThreshold{-0.003};
    ConfidenceMode confidenceMode{ConfidenceMode::Rank};
    double minConfidencePercentile{0.6};
    bool dryRun{false};
    FailMode failMode{FailMode::Open};
    double scoreToConfidenceScale{0.01};
};

struct PredictionRow {
    int64_t asofOpenTimeMs{0};
    int64_t generatedAtMs{0};
    double score{0.0};
    std::optional<int64_t> rank;
    std::optional<double> scorePercentile;
};

struct QueryResult {
    bool ok{true};
    std::optional<PredictionRow> row;
    std::string error;
};

int64_t nowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

std::string fmtDouble(double value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

const nlohmann::json& paramsObject(const nlohmann::json& j) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!j.contains("params") || !j.at("params").is_object()) {
        return empty;
    }
    return j.at("params");
}

std::optional<double> readOptionalDouble(sqlite3_stmt* stmt, int column) {
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_double(stmt, column);
}

std::optional<int64_t> readOptionalInt64(sqlite3_stmt* stmt, int column) {
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(stmt, column);
}

ConfidenceMode parseConfidenceMode(std::string_view value) {
    if (value == "rank") {
        return ConfidenceMode::Rank;
    }
    if (value == "absolute") {
        return ConfidenceMode::Absolute;
    }
    throw std::invalid_argument("confidence_mode must be rank or absolute");
}

FailMode parseFailMode(std::string_view value) {
    if (value == "open") {
        return FailMode::Open;
    }
    if (value == "closed") {
        return FailMode::Closed;
    }
    throw std::invalid_argument("fail_mode must be open or closed");
}

std::unordered_map<std::string, std::chrono::seconds> parseHoldDurationsByInterval(const nlohmann::json& j) {
    std::unordered_map<std::string, std::chrono::seconds> out;
    const auto field = j.find("max_hold_duration_by_interval_seconds");
    if (field == j.end() || !field->is_object()) {
        return out;
    }

    for (auto it = field->begin(); it != field->end(); ++it) {
        if (!it.value().is_number_integer()) {
            throw std::invalid_argument("max_hold_duration_by_interval_seconds values must be integer seconds");
        }
        out.emplace(it.key(), std::chrono::seconds(it.value().get<int64_t>()));
    }
    return out;
}

strategy::StrategyConfig parseConfig(const nlohmann::json& j) {
    strategy::StrategyConfig cfg;
    cfg.name = j.value("name", "Qlib LightGBM Signal");
    cfg.type = j.value("type", "qlib_model_signal");
    const auto& params = paramsObject(j);
    cfg.adapterId = params.value("model_id", std::string{});
    cfg.intervals = j.value("intervals", kDefaultIntervals);
    if (cfg.intervals.empty()) {
        cfg.intervals = kDefaultIntervals;
    }
    cfg.scanInterval = std::chrono::seconds(j.value("scan_interval_seconds", 900));
    cfg.maxHoldDuration = std::chrono::seconds(j.value("max_hold_duration_seconds", 86400));
    cfg.riskPct = j.value("risk_pct", 0.01);
    cfg.slMultiplier = j.value("sl_multiplier", 1.5);
    cfg.tpMultiplier = j.value("tp_multiplier", 3.0);
    cfg.takeProfitPercent = j.value("takeProfitPercent", j.value("take_profit_percent", 20.0));
    cfg.leverage = j.value("leverage", 10);
    cfg.minNotional = j.value("min_notional", 1.0);
    cfg.atrPeriod = j.value("atr_period", 14);
    cfg.minConfidence = j.value("min_confidence", 0.0);
    cfg.maxHoldDurationByInterval = parseHoldDurationsByInterval(j);
    return cfg;
}

QlibSignalParams parseParams(const nlohmann::json& j) {
    QlibSignalParams p;
    const auto& params = paramsObject(j);
    p.source = params.value("source", p.source);
    p.dbPath = params.value("db_path", p.dbPath);
    p.modelId = params.value("model_id", p.modelId);
    p.maxArtifactAgeSeconds = params.value("max_artifact_age_seconds", p.maxArtifactAgeSeconds);
    p.maxDataAgeSeconds = params.value("max_data_age_seconds", p.maxDataAgeSeconds);
    p.longThreshold = params.value("long_threshold", p.longThreshold);
    p.shortThreshold = params.value("short_threshold", p.shortThreshold);
    p.confidenceMode = parseConfidenceMode(params.value("confidence_mode", std::string("rank")));
    p.minConfidencePercentile = params.value("min_confidence_percentile", p.minConfidencePercentile);
    p.dryRun = params.value("dry_run", p.dryRun);
    p.failMode = parseFailMode(params.value("fail_mode", std::string("open")));
    p.scoreToConfidenceScale = params.value("score_to_confidence_scale", p.scoreToConfidenceScale);
    return p;
}

void validateConfig(const strategy::StrategyConfig& cfg, const QlibSignalParams& params) {
    if (cfg.riskPct <= 0.0) {
        throw std::invalid_argument("risk_pct must be > 0");
    }
    if (cfg.slMultiplier <= 0.0) {
        throw std::invalid_argument("sl_multiplier must be > 0");
    }
    if (cfg.tpMultiplier < 0.0) {
        throw std::invalid_argument("tp_multiplier must be >= 0");
    }
    if (cfg.minConfidence < 0.0 || cfg.minConfidence > 1.0) {
        throw std::invalid_argument("min_confidence must be in [0,1]");
    }
    if (params.source != "sqlite") {
        throw std::invalid_argument("params.source must be sqlite");
    }
    if (params.dbPath.empty()) {
        throw std::invalid_argument("params.db_path must be non-empty");
    }
    if (params.modelId.empty()) {
        throw std::invalid_argument("params.model_id must be non-empty");
    }
    if (params.maxArtifactAgeSeconds <= 0) {
        throw std::invalid_argument("params.max_artifact_age_seconds must be > 0");
    }
    if (params.maxDataAgeSeconds <= 0) {
        throw std::invalid_argument("params.max_data_age_seconds must be > 0");
    }
    if (params.longThreshold < params.shortThreshold) {
        throw std::invalid_argument("params.long_threshold must be >= params.short_threshold");
    }
    if (params.minConfidencePercentile < 0.0 || params.minConfidencePercentile > 1.0) {
        throw std::invalid_argument("params.min_confidence_percentile must be in [0,1]");
    }
    if (params.confidenceMode == ConfidenceMode::Absolute && params.scoreToConfidenceScale <= 0.0) {
        throw std::invalid_argument("params.score_to_confidence_scale must be > 0 for absolute confidence mode");
    }
}

QueryResult queryLatestPrediction(
    const std::string& dbPath,
    const std::string& modelId,
    std::string_view symbol,
    std::string_view interval) {
    QueryResult out;
    sqlite3* rawDb = nullptr;
    const int openRc = sqlite3_open_v2(dbPath.c_str(), &rawDb, SQLITE_OPEN_READONLY, nullptr);
    if (openRc != SQLITE_OK || rawDb == nullptr) {
        out.ok = false;
        out.error = rawDb ? sqlite3_errmsg(rawDb) : "sqlite3_open_v2 failed";
        if (rawDb) {
            sqlite3_close(rawDb);
        }
        return out;
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    const char* sql =
        "SELECT asof_open_time_ms, generated_at_ms, score, rank, score_percentile "
        "FROM qlib_predictions "
        "WHERE model_id = ? AND symbol = ? AND interval = ? "
        "ORDER BY generated_at_ms DESC "
        "LIMIT 1;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        out.ok = false;
        out.error = sqlite3_errmsg(db.get());
        return out;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);

    sqlite3_bind_text(stmt.get(), 1, modelId.c_str(), static_cast<int>(modelId.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, symbol.data(), static_cast<int>(symbol.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, interval.data(), static_cast<int>(interval.size()), SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        out.row = std::nullopt;
        return out;
    }
    if (rc != SQLITE_ROW) {
        out.ok = false;
        out.error = sqlite3_errmsg(db.get());
        return out;
    }

    PredictionRow row;
    row.asofOpenTimeMs = sqlite3_column_int64(stmt.get(), 0);
    row.generatedAtMs = sqlite3_column_int64(stmt.get(), 1);
    row.score = sqlite3_column_double(stmt.get(), 2);
    row.rank = readOptionalInt64(stmt.get(), 3);
    row.scorePercentile = readOptionalDouble(stmt.get(), 4);
    out.row = row;
    return out;
}

std::string buildReason(
    const std::string& modelId,
    const PredictionRow& row,
    int64_t artifactAgeSeconds,
    int64_t dataAgeSeconds,
    bool dryRun,
    std::string_view wouldBeDirection = "") {
    std::ostringstream out;
    out << "qlib model=" << modelId
        << " score=" << fmtDouble(row.score, 4)
        << " pct=";
    if (row.scorePercentile.has_value()) {
        out << fmtDouble(*row.scorePercentile, 2);
    } else {
        out << "na";
    }
    out << " rank=";
    if (row.rank.has_value()) {
        out << *row.rank;
    } else {
        out << "na";
    }
    out << " artifact_age_s=" << artifactAgeSeconds
        << " data_age_s=" << dataAgeSeconds
        << " dry_run=" << (dryRun ? "true" : "false");
    if (!wouldBeDirection.empty()) {
        out << " would_be=" << wouldBeDirection;
    }
    return out.str();
}

bool intervalConfigured(const strategy::StrategyConfig& cfg, std::string_view interval) {
    return std::find(cfg.intervals.begin(), cfg.intervals.end(), interval) != cfg.intervals.end();
}

class QlibModelSignalStrategy final : public strategy::IStrategy {
public:
    QlibModelSignalStrategy(strategy::StrategyConfig cfg, QlibSignalParams params)
        : m_cfg(std::move(cfg)),
          m_params(std::move(params)) {}

    const strategy::StrategyConfig& config() const override {
        return m_cfg;
    }

    strategy::Signal evaluate(
        std::string_view symbol,
        std::string_view interval,
        const std::vector<Kline>& klines) const override {
        (void)klines;
        if (!intervalConfigured(m_cfg, interval)) {
            return {};
        }

        const QueryResult qr = queryLatestPrediction(m_params.dbPath, m_params.modelId, symbol, interval);
        if (!qr.ok || !qr.row.has_value()) {
            if (m_params.failMode == FailMode::Closed) {
                std::cerr << "qlib_model_signal warning: "
                          << (qr.ok ? "prediction not found" : qr.error)
                          << " model_id=" << m_params.modelId
                          << " symbol=" << symbol
                          << " interval=" << interval
                          << std::endl;
            }
            return {};
        }

        const auto& row = *qr.row;
        const int64_t nowMs = nowEpochMs();
        const int64_t artifactAgeSeconds = std::max<int64_t>(0, (nowMs - row.generatedAtMs) / 1000);
        const int64_t dataAgeSeconds = std::max<int64_t>(0, (nowMs - row.asofOpenTimeMs) / 1000);

        const std::string reason = buildReason(
            m_params.modelId,
            row,
            artifactAgeSeconds,
            dataAgeSeconds,
            m_params.dryRun);

        if (artifactAgeSeconds > m_params.maxArtifactAgeSeconds) {
            return strategy::Signal{.reason = reason};
        }
        if (dataAgeSeconds > m_params.maxDataAgeSeconds) {
            return strategy::Signal{.reason = reason};
        }

        double confidence = 0.0;
        if (m_params.confidenceMode == ConfidenceMode::Rank) {
            confidence = row.scorePercentile.has_value() ? clamp01(*row.scorePercentile) : 0.0;
        } else {
            confidence = clamp01(std::abs(row.score) / m_params.scoreToConfidenceScale);
        }

        bool longEligible = true;
        bool shortEligible = true;
        if (m_params.minConfidencePercentile > 0.0) {
            if (row.scorePercentile.has_value()) {
                const double pct = clamp01(*row.scorePercentile);
                longEligible = pct >= m_params.minConfidencePercentile;
                shortEligible = pct <= (1.0 - m_params.minConfidencePercentile);
            } else {
                longEligible = false;
                shortEligible = false;
            }
        }

        strategy::Signal::Direction direction = strategy::Signal::Direction::None;
        if (row.score >= m_params.longThreshold) {
            if (longEligible) {
                direction = strategy::Signal::Direction::Long;
            }
        } else if (row.score <= m_params.shortThreshold) {
            if (shortEligible) {
                direction = strategy::Signal::Direction::Short;
            }
        }

        if (m_params.dryRun) {
            const std::string_view wouldBe =
                direction == strategy::Signal::Direction::Long ? "Long" :
                direction == strategy::Signal::Direction::Short ? "Short" : "None";
            const std::string dryReason = buildReason(
                m_params.modelId, row, artifactAgeSeconds, dataAgeSeconds, true, wouldBe);
            return strategy::Signal{
                .direction = direction,
                .confidence = confidence,
                .reason = dryReason,
            };
        }

        return strategy::Signal{
            .direction = direction,
            .confidence = confidence,
            .reason = reason,
        };
    }

private:
    strategy::StrategyConfig m_cfg;
    QlibSignalParams m_params;
};

} // namespace

extern "C" {

__declspec(dllexport) strategy::IStrategy* createStrategy(const char* config_json) {
    try {
        const auto j = nlohmann::json::parse(config_json == nullptr ? "{}" : config_json);
        auto cfg = parseConfig(j);
        auto params = parseParams(j);
        validateConfig(cfg, params);
        return new QlibModelSignalStrategy(std::move(cfg), std::move(params));
    } catch (...) {
        return nullptr;
    }
}

__declspec(dllexport) void destroyStrategy(strategy::IStrategy* strategy) {
    delete strategy;
}

__declspec(dllexport) const char* strategyType() {
    return "qlib_model_signal";
}

__declspec(dllexport) const char* pluginVersion() {
    return "1.0.0";
}

__declspec(dllexport) int strategyPluginAbiVersion() {
    return strategy::kStrategyPluginAbiVersion;
}
} // extern "C"
