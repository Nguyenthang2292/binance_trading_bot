#pragma once

#include "account/account_snapshot.h"
#include "risk/risk_types.h"

#include <boost/asio/awaitable.hpp>

namespace engine {

class IRiskPort {
public:
    virtual ~IRiskPort() = default;

    virtual bool canOpenPosition() const = 0;
    virtual void onPositionClosed(const account::AccountSnapshot& snapshot, int64_t timestampMs) = 0;
    virtual void onScanCycle(const account::AccountSnapshot& snapshot, int64_t timestampMs) = 0;
    virtual boost::asio::awaitable<void> maybeRecompute(int64_t nowMs) = 0;
    virtual RiskStatus currentStatus() const = 0;
};

class NoOpRiskPort final : public IRiskPort {
public:
    bool canOpenPosition() const override { return true; }
    void onPositionClosed(const account::AccountSnapshot&, int64_t) override {}
    void onScanCycle(const account::AccountSnapshot&, int64_t) override {}
    boost::asio::awaitable<void> maybeRecompute(int64_t) override { co_return; }
    RiskStatus currentStatus() const override { return RiskStatus::OK; }
};

} // namespace engine

