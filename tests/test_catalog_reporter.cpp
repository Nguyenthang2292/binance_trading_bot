#include <gtest/gtest.h>

#include "catalog/catalog_reporter.h"

#include <chrono>

TEST(CatalogReporterTest, PrintListFormatsOutput) {
    const std::vector<catalog::StrategyCatalog::StrategyInfo> strategies{
        {
            .name = "rsi_15m",
            .type = "rsi_reversal",
            .version = "1.0.0",
            .pluginFile = "rsi.dll",
            .intervals = {"15m"},
            .scanInterval = std::chrono::seconds{3600},
            .maxHoldDuration = std::chrono::seconds{86400},
        }};

    testing::internal::CaptureStdout();
    catalog::CatalogReporter::printList(strategies, 1, "plugins");
    const auto output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("Loaded 1 strategies from 1 plugins in plugins/"), std::string::npos);
    EXPECT_NE(output.find("rsi_15m"), std::string::npos);
    EXPECT_NE(output.find("rsi_reversal"), std::string::npos);
}

TEST(CatalogReporterTest, SummaryAndRuntimeLoggingDoNotCrash) {
    catalog::StrategyCatalog::LoadSummary summary;
    summary.pluginsFound = 2;
    summary.pluginsLoaded = 1;
    summary.strategiesRegistered = 1;
    summary.errors = {"missing export"};

    const std::vector<catalog::StrategyCatalog::StrategyInfo> strategies{
        {
            .name = "s1",
            .type = "t1",
            .version = "1.0.0",
            .pluginFile = "a.dll",
            .intervals = {"15m"},
            .scanInterval = std::chrono::seconds{60},
            .maxHoldDuration = std::chrono::seconds{300},
        }};

    EXPECT_NO_THROW(catalog::CatalogReporter::logStartupSummary(summary, strategies));
    EXPECT_NO_THROW(catalog::CatalogReporter::logRuntimeStatus(strategies, 100, 2));
}
