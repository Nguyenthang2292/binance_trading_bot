#pragma once

#include <mutex>
#include <string>
#include <vector>

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
};

class ProcessManager final : public IProcessRunner {
public:
    explicit ProcessManager(ProcessManagerConfig config);

    ProcessResult spawnWithRetry(const std::vector<std::string>& cmd) override;

private:
    ProcessResult spawnOnce(const std::vector<std::string>& cmd, const std::string& logPath);
    std::string makeLogPath(const std::string& tag, int attemptNumber) const;

    ProcessManagerConfig m_config;
    std::mutex m_mutex;
};

} // namespace orchestration
