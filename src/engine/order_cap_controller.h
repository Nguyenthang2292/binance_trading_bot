#pragma once

#include "account/account_snapshot.h"
#include "engine/position_tracker.h"

#include <string>

namespace engine {

enum class OrderCapFailureMode {
    Closed,
    Open,
};

struct OrderCapConfig {
    bool enabled{true};
    double maxTotalNotionalPct{0.5};
    OrderCapFailureMode failureMode{OrderCapFailureMode::Closed};
};

enum class OrderCapDecision {
    Allow,
    Block,
};

struct OrderCapResult {
    OrderCapDecision decision{OrderCapDecision::Allow};
    std::string reason;
    double totalOpenNotional{0.0};
    double proposedNotional{0.0};
    double cap{0.0};
    double totalMarginBalance{0.0};
};

class IOrderCapPort {
public:
    virtual ~IOrderCapPort() = default;

    virtual OrderCapResult check(
        double proposedNotional,
        const account::AccountSnapshot& snapshot,
        const PositionTracker& tracker) const = 0;

    virtual OrderCapFailureMode failureMode() const = 0;
};

class NoOpOrderCapPort final : public IOrderCapPort {
public:
    OrderCapResult check(
        double,
        const account::AccountSnapshot&,
        const PositionTracker&) const override {
        return {OrderCapDecision::Allow, "order cap disabled"};
    }

    OrderCapFailureMode failureMode() const override {
        return OrderCapFailureMode::Open;
    }
};

class TotalNotionalGuard final : public IOrderCapPort {
public:
    explicit TotalNotionalGuard(OrderCapConfig config);

    OrderCapResult check(
        double proposedNotional,
        const account::AccountSnapshot& snapshot,
        const PositionTracker& tracker) const override;

    OrderCapFailureMode failureMode() const override {
        return m_config.failureMode;
    }

private:
    static double remotePositionNotional(const Position& pos);
    static double sumOpenNotional(
        const account::AccountSnapshot& snapshot,
        const PositionTracker& tracker);

    OrderCapConfig m_config;
};

} // namespace engine

