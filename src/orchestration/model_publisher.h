#pragma once

#include <string>

namespace orchestration {

struct ModelPublishRequest {
    std::string dbPath{"data/qlib_predictions.db"};
    std::string modelId;
    std::string interval;
    std::string runId;
    int horizonBars{4};
    std::string stagingDir;
    std::string artifactsDir;
    std::string manifestPath;
};

class ModelPublisher {
public:
    static bool publish(const ModelPublishRequest& request, std::string& error);
};

} // namespace orchestration
