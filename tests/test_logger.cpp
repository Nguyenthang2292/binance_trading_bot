#include <gtest/gtest.h>
#include "logger.h"
#include <fstream>
#include <cstdio>

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::instance().setMinLevel(LogLevel::Debug);
    }

    void TearDown() override {
        Logger::instance().setLogFile("");
        Logger::instance().setMinLevel(LogLevel::Debug);
    }
};

TEST_F(LoggerTest, SingletonReturnsSameInstance) {
    Logger& a = Logger::instance();
    Logger& b = Logger::instance();
    EXPECT_EQ(&a, &b);
}

TEST_F(LoggerTest, AllLevelsOutputWithoutCrash) {
    Logger::instance().log(LogLevel::Debug, "debug msg");
    Logger::instance().log(LogLevel::Info, "info msg");
    Logger::instance().log(LogLevel::Warning, "warn msg");
    Logger::instance().log(LogLevel::Error, "error msg");
    Logger::instance().log(LogLevel::Trade, "trade msg");
    SUCCEED();
}

TEST_F(LoggerTest, MinLevelFiltersLowerMessages) {
    Logger::instance().setMinLevel(LogLevel::Warning);

    Logger::instance().log(LogLevel::Debug, "should be filtered");
    Logger::instance().log(LogLevel::Info, "should be filtered");
    Logger::instance().log(LogLevel::Warning, "should appear");

    SUCCEED();
}

TEST_F(LoggerTest, WriteToLogFile) {
    const char* testFile = "/tmp/bot_test.log";
    std::remove(testFile);

    Logger::instance().setLogFile(testFile);
    Logger::instance().log(LogLevel::Info, "Test log message");
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
