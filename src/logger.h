#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    TRADE
};

class Logger {
public:
    static Logger& instance();
    void log(LogLevel level, const std::string& message);
    void setLogFile(const std::string& filepath);
    void setMinLevel(LogLevel level);

private:
    Logger() = default;
    ~Logger();

    std::ofstream m_logFile;
    std::mutex m_mutex;
    LogLevel m_minLevel = LogLevel::INFO;

    std::string levelToString(LogLevel level) const;
    std::string getTimestamp() const;
    std::string getColor(LogLevel level) const;
    std::string getReset() const;
};
