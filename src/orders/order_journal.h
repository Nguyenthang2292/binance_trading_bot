#pragma once

#include "common/expected_compat.h"
#include "orders/order_types.h"
#include "types/error.h"

#include <mutex>
#include <optional>
#include <unordered_map>

struct JournalEntry {
    CorrelationId correlationId;
    Symbol symbol;
    ClientOrderId clientOrderId;
    std::string orderCategory;
    OrderSide side{OrderSide::Buy};
    OrderType type{OrderType::Market};
    PositionSide positionSide{PositionSide::Both};
    std::string quantity;
    std::string price;
    std::string requestParams;
    int64_t sendTimestampMs{0};
    std::optional<int64_t> responseTimestampMs;
    PlacementState state{PlacementState::UnknownPendingReconcile};
    std::optional<int64_t> binanceOrderId;
    std::optional<OrderMetadata> metadata;
};

class OrderJournal {
public:
    virtual ~OrderJournal() = default;

    virtual compat::expected<void, BinanceError> recordIntent(JournalEntry entry) = 0;
    virtual compat::expected<void, BinanceError> updateState(CorrelationId id,
                                                          PlacementState state,
                                                          std::optional<int64_t> binanceOrderId = std::nullopt) = 0;
    virtual compat::expected<std::vector<JournalEntry>, BinanceError> pendingReconcile() = 0;
    virtual compat::expected<std::optional<JournalEntry>, BinanceError> findByClientOrderId(
        const ClientOrderId& clientOrderId) = 0;
};

class InMemoryOrderJournal final : public OrderJournal {
public:
    compat::expected<void, BinanceError> recordIntent(JournalEntry entry) override;
    compat::expected<void, BinanceError> updateState(CorrelationId id,
                                                  PlacementState state,
                                                  std::optional<int64_t> binanceOrderId) override;
    compat::expected<std::vector<JournalEntry>, BinanceError> pendingReconcile() override;
    compat::expected<std::optional<JournalEntry>, BinanceError> findByClientOrderId(
        const ClientOrderId& clientOrderId) override;

private:
    std::mutex m_mutex;
    std::unordered_map<CorrelationId, JournalEntry> m_entriesByCorrelationId;
    std::unordered_map<ClientOrderId, CorrelationId> m_correlationByClientId;
};

class DurableOrderJournal final : public OrderJournal {
public:
    explicit DurableOrderJournal(std::string path);

    compat::expected<void, BinanceError> recordIntent(JournalEntry entry) override;
    compat::expected<void, BinanceError> updateState(CorrelationId id,
                                                  PlacementState state,
                                                  std::optional<int64_t> binanceOrderId) override;
    compat::expected<std::vector<JournalEntry>, BinanceError> pendingReconcile() override;
    compat::expected<std::optional<JournalEntry>, BinanceError> findByClientOrderId(
        const ClientOrderId& clientOrderId) override;

private:
    std::mutex m_mutex;
    std::string m_path;
    std::unordered_map<CorrelationId, JournalEntry> m_entriesByCorrelationId;
    std::unordered_map<ClientOrderId, CorrelationId> m_correlationByClientId;

    compat::expected<void, BinanceError> appendRecord(const std::string& op,
                                                   const CorrelationId& correlationId,
                                                   PlacementState state,
                                                   std::optional<int64_t> binanceOrderId,
                                                   const JournalEntry* entry);
    compat::expected<void, BinanceError> loadFromFile();
};
