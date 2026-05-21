#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

namespace orchestration {

struct ProcessResult {
    int exitCode{-1};
    bool timedOut{false};
    bool succeeded{false};
    std::string logPath;
};

struct ProcessManagerConfig {
    int maxAttempts{3};
    int backoffBaseSeconds{30};
    int timeoutSeconds{300};
    std::string logDir{"logs/qlib_orch"};
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;
    virtual ProcessResult spawnWithRetry(const std::vector<std::string>& cmd) = 0;

    virtual bool startDaemon(const std::string& name, const std::vector<std::string>& cmd) = 0;
    virtual bool isDaemonRunning(const std::string& name) = 0;
    virtual void stopDaemon(const std::string& name) = 0;
};

class ProcessManager final : public IProcessRunner {
public:
    explicit ProcessManager(ProcessManagerConfig config);

    ProcessResult spawnWithRetry(const std::vector<std::string>& cmd) override;

    bool startDaemon(const std::string& name, const std::vector<std::string>& cmd) override;
    bool isDaemonRunning(const std::string& name) override;
    void stopDaemon(const std::string& name) override;
    virtual ~ProcessManager();

private:
    ProcessResult spawnOnce(const std::vector<std::string>& cmd, const std::string& logPath);
    std::string makeLogPath(const std::string& tag, int attemptNumber) const;

    ProcessManagerConfig m_config;
    mutable std::mutex m_mutex;

#if defined(_WIN32)
    struct DaemonCtx {
        void* hProcess{nullptr};
        void* jobHandle{nullptr};
    };
#else
    struct DaemonCtx {
        int pid{-1};
    };
#endif
    std::unordered_map<std::string, DaemonCtx> m_daemons;
};

} // namespace orchestration
