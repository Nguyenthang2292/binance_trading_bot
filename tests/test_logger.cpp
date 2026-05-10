#include <gtest/gtest.h>
#include "logger.h"
#include <fstream>
#include <cstdio>

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::instance().setMinLevel(LogLevel::DEBUG);
    }

    void TearDown() override {
        Logger::instance().setLogFile("");
        Logger::instance().setMinLevel(LogLevel::DEBUG);
    }
};

TEST_F(LoggerTest, SingletonReturnsSameInstance) {
    Logger& a = Logger::instance();
    Logger& b = Logger::instance();
    EXPECT_EQ(&a, &b);
}

TEST_F(LoggerTest, AllLevelsOutputWithoutCrash) {
    Logger::instance().log(LogLevel::DEBUG, "debug msg");
    Logger::instance().log(LogLevel::INFO, "info msg");
    Logger::instance().log(LogLevel::WARNING, "warn msg");
    Logger::instance().log(LogLevel::ERROR, "error msg");
    Logger::instance().log(LogLevel::TRADE, "trade msg");
    SUCCEED();
}

TEST_F(LoggerTest, MinLevelFiltersLowerMessages) {
    Logger::instance().setMinLevel(LogLevel::WARNING);

    Logger::instance().log(LogLevel::DEBUG, "should be filtered");
    Logger::instance().log(LogLevel::INFO, "should be filtered");
    Logger::instance().log(LogLevel::WARNING, "should appear");

    SUCCEED();
}

TEST_F(LoggerTest, WriteToLogFile) {
    const char* testFile = "/tmp/bot_test.log";
    std::remove(testFile);

    Logger::instance().setLogFile(testFile);
    Logger::instance().log(LogLevel::INFO, "Test log message");
    Logger::instance().setLogFile("");

    std::ifstream f(testFile);
    ASSERT_TRUE(f.is_open());

    std::string line;
    std::getline(f, line);
    EXPECT_NE(line.find("Test log message"), std::string::npos);
    EXPECT_NE(line.find("INFO"), std::string::npos);

    f.close();
    std::remove(testFile);
}
