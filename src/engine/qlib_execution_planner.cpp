#include "engine/qlib_execution_planner.h"

#include "logger.h"
#include "orchestration/sqlite_helpers.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

namespace engine {

namespace {

constexpr int kExpectedSchemaVersion = 7;
constexpr int64_t kPlanPollTimeoutMs = 500;

struct ExecutionSlice {
    std::string sliceId;
    int sliceIndex{0};
    int64_t dueAtMs{0};
    std::string side;
    std::string quantity;
};

struct ReadyPlan {
    std::string planId;
    std::vector<ExecutionSlice> slices;
};

std::string sideToDb(OrderSide side) {
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

std::string positionSideToDb(PositionSide side) {
    switch (side) {
        case PositionSide::Both:
            return "BOTH";
        case PositionSide::Long:
            return "LONG";
        case PositionSide::Short:
            return "SHORT";
    }
    return "BOTH";
}

std::string metadataJson(const std::optional<OrderMetadata>& metadata) {
    if (!metadata.has_value()) {
        return "{}";
    }
    std::ostringstream out;
    out << "{";
    bool first = true;
    auto field = [&](std::string_view key, const auto& value) {
        if (!value.has_value()) {
            return;
        }
        if (!first) {
            out << ",";
        }
        first = false;
        out << "\"" << key << "\":\"" << *value << "\"";
    };
    field("timeframe", metadata->timeframe);
    field("comment", metadata->comment);
    field("strategy_tag", metadata->strategyTag);
    if (metadata->magic.has_value()) {
        if (!first) {
            out << ",";
        }
        out << "\"magic\":" << *metadata->magic;
    }
    out << "}";
    return out.str();
}

void configureConnection(sqlite3* db) {
    sqlite3_busy_timeout(db, 5000);
    orchestration::sqlite_helpers::execOrThrow(db, "PRAGMA journal_mode = WAL;");
    orchestration::sqlite_helpers::execOrThrow(db, "PRAGMA foreign_keys = ON;");
    orchestration::sqlite_helpers::execOrThrow(db, "PRAGMA synchronous = NORMAL;");
}

void ensureExecutionSchema(sqlite3* db) {
    const int version = orchestration::sqlite_helpers::readUserVersion(db);
    if (version != kExpectedSchemaVersion) {
        throw std::runtime_error(
            "qlib execution schema mismatch: expected " + std::to_string(kExpectedSchemaVersion) +
            " got " + std::to_string(version));
    }
    orchestration::sqlite_helpers::execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS qlib_execution_requests ("
        "request_id TEXT PRIMARY KEY,"
        "symbol TEXT NOT NULL,"
        "side TEXT NOT NULL,"
        "quantity TEXT NOT NULL,"
        "position_side TEXT NOT NULL,"
        "metadata_json TEXT,"
        "status TEXT NOT NULL CHECK (status IN ('pending','succeeded','expired','failed')),"
        "created_at_ms INTEGER NOT NULL,"
        "deadline_ms INTEGER NOT NULL,"
        "error TEXT"
        ");");
    orchestration::sqlite_helpers::execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS qlib_execution_plans ("
        "plan_id TEXT PRIMARY KEY,"
        "request_id TEXT NOT NULL,"
        "algorithm TEXT NOT NULL,"
        "status TEXT NOT NULL CHECK (status IN ('running','succeeded','failed','expired')),"
        "generated_at_ms INTEGER NOT NULL,"
        "expires_at_ms INTEGER NOT NULL,"
        "total_quantity TEXT NOT NULL,"
        "slice_count INTEGER NOT NULL,"
        "error TEXT,"
        "FOREIGN KEY (request_id) REFERENCES qlib_execution_requests(request_id)"
        ");");
    orchestration::sqlite_helpers::execOrThrow(
        db,
        "CREATE TABLE IF NOT EXISTS qlib_execution_slices ("
        "slice_id TEXT PRIMARY KEY,"
        "plan_id TEXT NOT NULL,"
        "slice_index INTEGER NOT NULL,"
        "due_at_ms INTEGER NOT NULL,"
        "side TEXT NOT NULL,"
        "quantity TEXT NOT NULL,"
        "status TEXT NOT NULL CHECK (status IN ('pending','submitted','filled','failed','revoked')),"
        "revoked_at_ms INTEGER,"
        "revoke_reason TEXT,"
        "FOREIGN KEY (plan_id) REFERENCES qlib_execution_plans(plan_id)"
        ");");
}

std::string makeRequestId(const std::string& symbol) {
    static std::atomic<uint64_t> counter{0};
    const int64_t now = orchestration::sqlite_helpers::nowMs();
    return "qlib_exec_" + symbol + "_" + std::to_string(now) + "_" + std::to_string(counter.fetch_add(1));
}

void insertRequest(sqlite3* db, const std::string& requestId, const MarketOrderDraft& draft, int64_t nowMs, int64_t deadlineMs) {
    const char* sql =
        "INSERT INTO qlib_execution_requests("
        "request_id, symbol, side, quantity, position_side, metadata_json, status, created_at_ms, deadline_ms, error"
        ") VALUES(?, ?, ?, ?, ?, ?, 'pending', ?, ?, NULL);";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        throw std::runtime_error(orchestration::sqlite_helpers::sqliteError(db, "prepare qlib execution request failed"));
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);

    const std::string side = sideToDb(draft.side);
    const std::string positionSide = positionSideToDb(draft.positionSide);
    const std::string quantity(draft.quantity.value());
    const std::string metadata = metadataJson(draft.metadata);

    orchestration::sqlite_helpers::bindText(stmt.get(), 1, requestId);
    orchestration::sqlite_helpers::bindText(stmt.get(), 2, draft.symbol);
    orchestration::sqlite_helpers::bindText(stmt.get(), 3, side);
    orchestration::sqlite_helpers::bindText(stmt.get(), 4, quantity);
    orchestration::sqlite_helpers::bindText(stmt.get(), 5, positionSide);
    orchestration::sqlite_helpers::bindText(stmt.get(), 6, metadata);
    sqlite3_bind_int64(stmt.get(), 7, nowMs);
    sqlite3_bind_int64(stmt.get(), 8, deadlineMs);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(orchestration::sqlite_helpers::sqliteError(db, "insert qlib execution request failed"));
    }
}

std::optional<ReadyPlan> fetchReadyPlan(sqlite3* db, const std::string& requestId, std::string& reason) {
    const char* sql =
        "SELECT p.plan_id, p.expires_at_ms "
        "FROM qlib_execution_plans p "
        "WHERE p.request_id = ? AND p.status = 'succeeded' "
        "ORDER BY p.generated_at_ms DESC LIMIT 1;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        reason = sqlite3_errmsg(db);
        return std::nullopt;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    orchestration::sqlite_helpers::bindText(stmt.get(), 1, requestId);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        reason = "plan not ready";
        return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
        reason = sqlite3_errmsg(db);
        return std::nullopt;
    }
    ReadyPlan plan;
    plan.planId = orchestration::sqlite_helpers::columnText(stmt.get(), 0);
    const int64_t expiresAtMs = sqlite3_column_int64(stmt.get(), 1);
    if (expiresAtMs <= orchestration::sqlite_helpers::nowMs()) {
        reason = "plan expired";
        return std::nullopt;
    }
    const char* sliceSql =
        "SELECT slice_id, slice_index, due_at_ms, side, quantity "
        "FROM qlib_execution_slices "
        "WHERE plan_id = ? AND status = 'pending' "
        "ORDER BY slice_index ASC;";
    sqlite3_stmt* rawSliceStmt = nullptr;
    if (sqlite3_prepare_v2(db, sliceSql, -1, &rawSliceStmt, nullptr) != SQLITE_OK || rawSliceStmt == nullptr) {
        reason = sqlite3_errmsg(db);
        return std::nullopt;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> sliceStmt(rawSliceStmt, sqlite3_finalize);
    orchestration::sqlite_helpers::bindText(sliceStmt.get(), 1, plan.planId);
    while (sqlite3_step(sliceStmt.get()) == SQLITE_ROW) {
        plan.slices.push_back(ExecutionSlice{
            .sliceId = orchestration::sqlite_helpers::columnText(sliceStmt.get(), 0),
            .sliceIndex = sqlite3_column_int(sliceStmt.get(), 1),
            .dueAtMs = sqlite3_column_int64(sliceStmt.get(), 2),
            .side = orchestration::sqlite_helpers::columnText(sliceStmt.get(), 3),
            .quantity = orchestration::sqlite_helpers::columnText(sliceStmt.get(), 4),
        });
    }
    if (plan.slices.empty()) {
        reason = "plan has no slices";
        return std::nullopt;
    }
    reason = "plan ready";
    return plan;
}

void markRequest(sqlite3* db, const std::string& requestId, std::string_view status, std::string_view error) {
    const char* sql = "UPDATE qlib_execution_requests SET status = ?, error = ? WHERE request_id = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        Logger::instance().log(
            LogLevel::Warning,
            "[QlibExecutionPlanner] failed to prepare request status update request_id=" + requestId);
        return;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    orchestration::sqlite_helpers::bindText(stmt.get(), 1, status);
    orchestration::sqlite_helpers::bindText(stmt.get(), 2, error);
    orchestration::sqlite_helpers::bindText(stmt.get(), 3, requestId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        Logger::instance().log(
            LogLevel::Warning,
            "[QlibExecutionPlanner] failed to update request status request_id=" + requestId +
                " error=" + sqlite3_errmsg(db));
    }
}

void markSlice(sqlite3* db, const std::string& sliceId, std::string_view status, std::string_view reason) {
    const char* sql =
        "UPDATE qlib_execution_slices "
        "SET status = ?, revoked_at_ms = CASE WHEN ? = 'revoked' THEN ? ELSE revoked_at_ms END, "
        "revoke_reason = CASE WHEN ? = 'revoked' THEN ? ELSE revoke_reason END "
        "WHERE slice_id = ?;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        Logger::instance().log(LogLevel::Warning, "[QlibExecutionPlanner] failed to prepare slice update slice_id=" + sliceId);
        return;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    orchestration::sqlite_helpers::bindText(stmt.get(), 1, status);
    orchestration::sqlite_helpers::bindText(stmt.get(), 2, status);
    sqlite3_bind_int64(stmt.get(), 3, orchestration::sqlite_helpers::nowMs());
    orchestration::sqlite_helpers::bindText(stmt.get(), 4, status);
    orchestration::sqlite_helpers::bindText(stmt.get(), 5, reason);
    orchestration::sqlite_helpers::bindText(stmt.get(), 6, sliceId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        Logger::instance().log(LogLevel::Warning, "[QlibExecutionPlanner] failed to update slice slice_id=" + sliceId);
    }
}

void revokePendingSlices(sqlite3* db, const std::string& planId, std::string_view reason) {
    const char* sql =
        "UPDATE qlib_execution_slices "
        "SET status = 'revoked', revoked_at_ms = ?, revoke_reason = ? "
        "WHERE plan_id = ? AND status = 'pending';";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        Logger::instance().log(LogLevel::Warning, "[QlibExecutionPlanner] failed to prepare pending slice revocation plan_id=" + planId);
        return;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    sqlite3_bind_int64(stmt.get(), 1, orchestration::sqlite_helpers::nowMs());
    orchestration::sqlite_helpers::bindText(stmt.get(), 2, reason);
    orchestration::sqlite_helpers::bindText(stmt.get(), 3, planId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        Logger::instance().log(LogLevel::Warning, "[QlibExecutionPlanner] failed to revoke pending slices plan_id=" + planId);
    }
}

bool latestDecisionContradicts(sqlite3* db, const MarketOrderDraft& draft, std::string& reason) {
    if (!draft.metadata || !draft.metadata->strategyTag || !draft.metadata->timeframe) {
        return false;
    }
    const char* sql =
        "SELECT action, direction "
        "FROM qlib_strategy_decisions d "
        "JOIN qlib_strategy_runs r ON r.run_id = d.run_id AND r.status = 'succeeded' "
        "WHERE d.strategy_id = ? AND d.symbol = ? AND d.interval = ? "
        "ORDER BY d.generated_at_ms DESC LIMIT 1;";
    sqlite3_stmt* rawStmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &rawStmt, nullptr) != SQLITE_OK || rawStmt == nullptr) {
        reason = sqlite3_errmsg(db);
        return true;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(rawStmt, sqlite3_finalize);
    orchestration::sqlite_helpers::bindText(stmt.get(), 1, *draft.metadata->strategyTag);
    orchestration::sqlite_helpers::bindText(stmt.get(), 2, draft.symbol);
    orchestration::sqlite_helpers::bindText(stmt.get(), 3, *draft.metadata->timeframe);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        return false;
    }
    if (rc != SQLITE_ROW) {
        reason = sqlite3_errmsg(db);
        return true;
    }
    const std::string action = orchestration::sqlite_helpers::columnText(stmt.get(), 0);
    const std::string direction = orchestration::sqlite_helpers::columnText(stmt.get(), 1);
    const bool expectedLongBuy = draft.side == OrderSide::Buy;
    if (expectedLongBuy && (action != "buy" || direction != "long")) {
        reason = "direction_reversed action=" + action + " direction=" + direction;
        return true;
    }
    return false;
}

} // namespace

QlibExecutionPlanner::QlibExecutionPlanner(const std::string& dbPath) : m_dbPath(dbPath) {}

QlibExecutionPlanner::QlibExecutionPlanner(const std::string& dbPath, IExecutionPlanner& nativeFallback)
    : m_dbPath(dbPath),
      m_nativeFallback(&nativeFallback) {}

QlibExecutionPlanner::~QlibExecutionPlanner() = default;

boost::asio::awaitable<OrdersResult<NormalPlacementResult>> QlibExecutionPlanner::executeMarket(MarketOrderDraft draft) {
    sqlite3* rawDb = nullptr;
    const int openRc = sqlite3_open_v2(m_dbPath.c_str(), &rawDb, SQLITE_OPEN_READWRITE, nullptr);
    if (openRc != SQLITE_OK || rawDb == nullptr) {
        if (rawDb) {
            sqlite3_close(rawDb);
        }
        co_return std::unexpected(BinanceError::fromApiResponse(-92000, "qlib execution db open failed"));
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(rawDb, sqlite3_close);

    try {
        configureConnection(db.get());
        ensureExecutionSchema(db.get());
        const int64_t createdAtMs = orchestration::sqlite_helpers::nowMs();
        const int64_t deadlineMs = createdAtMs + kPlanPollTimeoutMs;
        const std::string requestId = makeRequestId(draft.symbol);

        orchestration::sqlite_helpers::execOrThrow(db.get(), "BEGIN IMMEDIATE;");
        insertRequest(db.get(), requestId, draft, createdAtMs, deadlineMs);
        orchestration::sqlite_helpers::execOrThrow(db.get(), "COMMIT;");

        std::string reason = "plan not ready";
        while (orchestration::sqlite_helpers::nowMs() < deadlineMs) {
            auto readyPlan = fetchReadyPlan(db.get(), requestId, reason);
            if (readyPlan.has_value()) {
                markRequest(db.get(), requestId, "succeeded", "");
                Logger::instance().log(
                    LogLevel::Info,
                    "[QlibExecutionPlanner] plan ready request_id=" + requestId +
                        " plan_id=" + readyPlan->planId +
                        " slices=" + std::to_string(readyPlan->slices.size()));
                if (m_nativeFallback == nullptr) {
                    co_return std::unexpected(BinanceError::fromApiResponse(
                        -92002,
                        "qlib plan ready but slice executor fallback is not configured"));
                }

                OrdersResult<NormalPlacementResult> lastResult =
                    std::unexpected(BinanceError::fromApiResponse(-92004, "qlib plan had no submitted slices"));
                double cumulativeQuantity = 0.0;
                const double approvedQuantity = draft.quantity.toDouble();
                const std::string expectedSide = sideToDb(draft.side);

                for (const auto& slice : readyPlan->slices) {
                    if (slice.side != expectedSide) {
                        markSlice(db.get(), slice.sliceId, "failed", "side mismatch");
                        co_return std::unexpected(BinanceError::fromApiResponse(-92005, "qlib slice side mismatch"));
                    }
                    auto sliceQty = DecimalString::parse(slice.quantity);
                    if (!sliceQty) {
                        markSlice(db.get(), slice.sliceId, "failed", "invalid quantity");
                        co_return std::unexpected(sliceQty.error());
                    }
                    cumulativeQuantity += sliceQty->toDouble();
                    if (cumulativeQuantity > approvedQuantity + 1e-12) {
                        markSlice(db.get(), slice.sliceId, "failed", "quantity exceeds parent");
                        revokePendingSlices(db.get(), readyPlan->planId, "quantity exceeds parent");
                        co_return std::unexpected(BinanceError::fromApiResponse(-92006, "qlib slice quantity exceeds parent"));
                    }

                    const int64_t now = orchestration::sqlite_helpers::nowMs();
                    if (slice.dueAtMs > now) {
                        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
                        timer.expires_after(std::chrono::milliseconds(slice.dueAtMs - now));
                        co_await timer.async_wait(boost::asio::use_awaitable);
                    }

                    std::string revokeReason;
                    if (latestDecisionContradicts(db.get(), draft, revokeReason)) {
                        if (revokeReason.empty()) {
                            revokeReason = "latest decision unavailable";
                        }
                        revokePendingSlices(db.get(), readyPlan->planId, revokeReason);
                        Logger::instance().log(
                            LogLevel::Warning,
                            "[QLIB_EXEC][SLICE_REVOKED] request=" + requestId +
                                " slice=" + std::to_string(slice.sliceIndex) +
                                " reason=" + revokeReason);
                        co_return std::unexpected(BinanceError::fromApiResponse(-92007, "qlib execution revoked: " + revokeReason));
                    }

                    MarketOrderDraft sliceDraft = draft;
                    sliceDraft.quantity = *sliceQty;
                    markSlice(db.get(), slice.sliceId, "submitted", "");
                    Logger::instance().log(
                        LogLevel::Info,
                        "[QLIB_EXEC][SLICE_SUBMITTED] request=" + requestId +
                            " slice=" + std::to_string(slice.sliceIndex) +
                            " qty=" + slice.quantity);
                    lastResult = co_await m_nativeFallback->executeMarket(std::move(sliceDraft));
                    if (!lastResult) {
                        markSlice(db.get(), slice.sliceId, "failed", lastResult.error().toString());
                        revokePendingSlices(db.get(), readyPlan->planId, "slice submission failed");
                        co_return lastResult;
                    }
                    if (lastResult->state != PlacementState::Accepted) {
                        markSlice(db.get(), slice.sliceId, "failed", "placement not accepted");
                        revokePendingSlices(db.get(), readyPlan->planId, "slice placement not accepted");
                        co_return lastResult;
                    }
                    markSlice(db.get(), slice.sliceId, "filled", "");
                }
                co_return lastResult;
            }
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(25));
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
        markRequest(db.get(), requestId, "expired", reason);
        if (m_nativeFallback != nullptr) {
            Logger::instance().log(
                LogLevel::Warning,
                "[QlibExecutionPlanner] plan not ready before deadline, using native fallback request_id=" + requestId +
                    " reason=" + reason);
            co_return co_await m_nativeFallback->executeMarket(std::move(draft));
        }
        co_return std::unexpected(BinanceError::fromApiResponse(
            -92001,
            "qlib execution plan not ready: " + reason));
    } catch (const std::exception& e) {
        std::string ignored;
        (void)orchestration::sqlite_helpers::execSql(db.get(), "ROLLBACK;", ignored);
        Logger::instance().log(LogLevel::Warning, std::string("[QlibExecutionPlanner] fail closed: ") + e.what());
        co_return std::unexpected(BinanceError::fromApiResponse(-92003, e.what()));
    }
}

} // namespace engine
