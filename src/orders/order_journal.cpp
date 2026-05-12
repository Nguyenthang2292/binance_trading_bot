#include "orders/order_journal.h"

std::expected<void, BinanceError> InMemoryOrderJournal::recordIntent(JournalEntry entry) {
    std::scoped_lock lock(m_mutex);
    const auto correlationId = entry.correlationId;
    const auto clientOrderId = entry.clientOrderId;
    m_entriesByCorrelationId[correlationId] = std::move(entry);
    m_correlationByClientId[clientOrderId] = correlationId;
    return {};
}

std::expected<void, BinanceError> InMemoryOrderJournal::updateState(
    CorrelationId id,
    PlacementState state,
    std::optional<int64_t> binanceOrderId) {
    std::scoped_lock lock(m_mutex);
    auto it = m_entriesByCorrelationId.find(id);
    if (it == m_entriesByCorrelationId.end()) {
        return std::unexpected(BinanceError::fromApiResponse(-90002, "Journal entry not found"));
    }
    it->second.state = state;
    it->second.binanceOrderId = binanceOrderId;
    return {};
}

std::expected<std::vector<JournalEntry>, BinanceError> InMemoryOrderJournal::pendingReconcile() {
    std::scoped_lock lock(m_mutex);
    std::vector<JournalEntry> result;
    result.reserve(m_entriesByCorrelationId.size());
    for (const auto& [_, entry] : m_entriesByCorrelationId) {
        if (entry.state == PlacementState::UnknownPendingReconcile) {
            result.push_back(entry);
        }
    }
    return result;
}

std::expected<std::optional<JournalEntry>, BinanceError> InMemoryOrderJournal::findByClientOrderId(
    const ClientOrderId& clientOrderId) {
    std::scoped_lock lock(m_mutex);
    const auto idIt = m_correlationByClientId.find(clientOrderId);
    if (idIt == m_correlationByClientId.end()) {
        return std::optional<JournalEntry>{};
    }
    const auto entryIt = m_entriesByCorrelationId.find(idIt->second);
    if (entryIt == m_entriesByCorrelationId.end()) {
        return std::optional<JournalEntry>{};
    }
    return std::optional<JournalEntry>{entryIt->second};
}
