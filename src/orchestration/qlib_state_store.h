#pragma once

#include "orchestration/runtime_ports.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct sqlite3;

namespace orchestration {

struct QlibStateStoreConfig {
    std::string dbPath{"data/qlib_predictions.db"};
    std::string modelId{"lightgbm_1h_v1"};
    std::string interval{"1h"};
    std::chrono::seconds reloadInterval{5};
    double canaryRiskMultiplier{0.25};
};

// IMPORTANT: QlibStateStore uses enable_shared_from_this for timer safety.
// It MUST be allocated via std::make_shared (or the create() factory below).
// Stack allocation or unique_ptr ownership will cause std::bad_weak_ptr at runtime
// when startReloadLoop() is called.
class QlibStateStore final : public IExecutionStatePort, public std::enable_shared_from_this<QlibStateStore> {
public:
    // Preferred construction path — guarantees shared_ptr ownership required by
    // startReloadLoop() / weak_from_this().
    [[nodiscard]] static std::shared_ptr<QlibStateStore> create(QlibStateStoreConfig config);

    explicit QlibStateStore(QlibStateStoreConfig config);
    ~QlibStateStore();

    QlibStateStore(const QlibStateStore&) = delete;
    QlibStateStore& operator=(const QlibStateStore&) = delete;

    void initializeSchema();
    void initializeRuntimeStateIfMissing();
    void startReloadLoop(boost::asio::io_context& ioc);
    void stopReloadLoop();
    bool setExecutionMode(ExecutionMode mode, std::string rollbackReason = {});

    RuntimeStateSnapshot snapshot() const override;
    double canaryRiskMultiplier() const override { return m_config.canaryRiskMultiplier; }
    const std::string& dbPath() const { return m_config.dbPath; }
    const std::string& modelId() const { return m_config.modelId; }
    const std::string& interval() const { return m_config.interval; }

private:
    void reloadStateOnce();
    void scheduleReload();
    RuntimeStateSnapshot loadSnapshotLocked() const;
    static std::string modeToDb(ExecutionMode mode);
    static ExecutionMode modeFromDb(const std::string& modeText);

    QlibStateStoreConfig m_config;
    sqlite3* m_db{nullptr};
    mutable std::mutex m_mutex;
    RuntimeStateSnapshot m_snapshot;

    std::atomic<bool> m_reloadRunning{false};
    std::unique_ptr<boost::asio::steady_timer> m_reloadTimer;
};

} // namespace orchestration
