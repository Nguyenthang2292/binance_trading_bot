#include "logger.h"
#include <iostream>

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    if (m_logFile.is_open()) {
        m_logFile.close();
    }
}

void Logger::setLogFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_logFile.is_open()) {
        m_logFile.close();
    }
    m_logFile.open(filepath, std::ios::app);
}

void Logger::setMinLevel(LogLevel level) {
    m_minLevel = level;
}

std::string Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG  ";
        case LogLevel::INFO:    return "INFO   ";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR  ";
        case LogLevel::TRADE:   return "TRADE  ";
        default:                return "UNKNOWN";
    }
}

std::string Logger::getTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::getColor(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG:   return "\033[36m";
        case LogLevel::INFO:    return "\033[32m";
        case LogLevel::WARNING: return "\033[33m";
        case LogLevel::ERROR:   return "\033[31m";
        case LogLevel::TRADE:   return "\033[35m";
        default:                return "\033[0m";
    }
}

std::string Logger::getReset() const {
    return "\033[0m";
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < m_minLevel) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    std::string timestamp = getTimestamp();
    std::string levelStr = levelToString(level);
    std::string color = getColor(level);
    std::string reset = getReset();

    std::string formatted = color + "[" + timestamp + "] [" + levelStr + "] " + message + reset;

    std::cout << formatted << std::endl;

    if (m_logFile.is_open()) {
        m_logFile << "[" << timestamp << "] [" << levelStr << "] " << message << std::endl;
    }
}
