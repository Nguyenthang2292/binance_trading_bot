#pragma once

#include "common/expected_compat.h"
#include "types/error.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <variant>

using Symbol = std::string;
using ClientOrderId = std::string;
using ClientAlgoId = std::string;
using CorrelationId = std::string;
using RawOrderParams = std::unordered_map<std::string, std::string>;

enum class ResponseType { ACK, RESULT };
enum class PositionMode { Unknown, OneWay, Hedge };
enum class PlacementState { Accepted, Rejected, UnknownPendingReconcile };
enum class OrderLifecycle { Normal, Algo };
enum class OrderPoolKind { Open, History };
enum class OrderErrorCategory {
    Validation,
    Unsupported,
    ExchangeReject,
    RateLimit,
    Auth,
    Network,
    Timeout,
    CanceledBeforeSend,
    Parse,
    Journal,
    Unknown
};

struct ValidationIssue {
    enum class Severity { Error, Warning, Skipped };
    Severity severity{Severity::Error};
    std::string code;
    std::string message;
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    std::optional<std::chrono::milliseconds> exchangeInfoAge;

    bool hasErrors() const {
        for (const auto& issue : issues) {
            if (issue.severity == ValidationIssue::Severity::Error) {
                return true;
            }
        }
        return false;
    }
};

struct NormalOrderIdentity {
    Symbol symbol;
    std::optional<int64_t> orderId;
    std::optional<ClientOrderId> clientOrderId;

    bool operator==(const NormalOrderIdentity& other) const = default;
};

struct AlgoOrderIdentity {
    Symbol symbol;
    std::optional<int64_t> algoId;
    std::optional<ClientAlgoId> clientAlgoId;

    bool operator==(const AlgoOrderIdentity& other) const = default;
};

using OrderIdentity = std::variant<NormalOrderIdentity, AlgoOrderIdentity>;

struct OrderMetadata {
    std::optional<int64_t> magic;
    std::optional<std::string> comment;
    std::optional<std::string> strategyTag;
};

template <typename T>
using OrdersResult = std::expected<T, BinanceError>;

class OrderJournal;

struct OrdersConfig {
    std::string clientIdNamespace;
    bool allowBestEffortJournal{false};
    ResponseType defaultResponseType{ResponseType::ACK};
    std::chrono::milliseconds recvWindow{5000};
    bool allowRawTimestampOverride{false};
    PositionMode positionMode{PositionMode::Unknown};
    std::shared_ptr<OrderJournal> journal;
    bool journalIsDurable{false};
    std::string journalPath{"orders_journal.log"};
};
