#include "orders/order_journal.h"
#include "orders/order_service_utils.h"
#include "logger.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <chrono>
#include <filesystem>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::string sanitizeToken(std::string value) {
    for (char& c : value) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return value;
}

std::vector<std::string> splitTabs(std::string_view line) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
            parts.emplace_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

int toInt(const std::string& text) {
    return std::stoi(text);
}

int64_t toInt64(const std::string& text) {
    return std::stoll(text);
}

OrderSide parseSide(int value) {
    return value == 1 ? OrderSide::Sell : OrderSide::Buy;
}

OrderType parseType(int value) {
    switch (value) {
        case 0: return OrderType::Limit;
        case 1: return OrderType::Market;
        case 2: return OrderType::Stop;
        case 3: return OrderType::StopMarket;
        case 4: return OrderType::TakeProfit;
        case 5: return OrderType::TakeProfitMarket;
        case 6: return OrderType::TrailingStopMarket;
        default: return OrderType::Market;
    }
}

PositionSide parsePositionSide(int value) {
    switch (value) {
        case 1: return PositionSide::Long;
        case 2: return PositionSide::Short;
        default: return PositionSide::Both;
    }
}

PlacementState parsePlacementState(int value) {
    switch (value) {
        case 0: return PlacementState::Accepted;
        case 1: return PlacementState::Rejected;
        default: return PlacementState::UnknownPendingReconcile;
    }
}

int64_t unixMsNow() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::string errnoMessage(int value) {
    return std::strerror(value);
}

BinanceError journalIoError(int code, std::string_view action, int savedErrno) {
    std::ostringstream message;
    message << action << ": " << errnoMessage(savedErrno);
    return BinanceError::fromApiResponse(code, message.str());
}

std::expected<void, BinanceError> appendDurably(const std::string& path, const std::string& data) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(BinanceError::fromApiResponse(
                -90011,
                "Failed to create durable journal directory: " + ec.message()));
        }
    }
#ifdef _WIN32
    const int fd = _open(path.c_str(), _O_APPEND | _O_CREAT | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd == -1) {
        return std::unexpected(journalIoError(-90011, "Failed to open durable journal", errno));
    }

    size_t offset = 0;
    while (offset < data.size()) {
        const auto remaining = data.size() - offset;
        const auto chunk = static_cast<unsigned int>(
            std::min(remaining, static_cast<size_t>(std::numeric_limits<unsigned int>::max())));
        const int written = _write(fd, data.data() + offset, chunk);
        if (written <= 0) {
            const int savedErrno = errno;
            (void)_close(fd);
            return std::unexpected(journalIoError(-90012, "Failed to write durable journal", savedErrno));
        }
        offset += static_cast<size_t>(written);
    }

    if (_commit(fd) == -1) {
        const int savedErrno = errno;
        (void)_close(fd);
        return std::unexpected(journalIoError(-90013, "Failed to flush durable journal", savedErrno));
    }
    if (_close(fd) == -1) {
        return std::unexpected(journalIoError(-90013, "Failed to close durable journal", errno));
    }
    return {};
#else
    const int fd = ::open(path.c_str(), O_APPEND | O_CREAT | O_WRONLY, 0600);
    if (fd == -1) {
        return std::unexpected(journalIoError(-90011, "Failed to open durable journal", errno));
    }

    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(fd, data.data() + offset, data.size() - offset);
        if (written <= 0) {
            const int savedErrno = errno;
            (void)::close(fd);
            return std::unexpected(journalIoError(-90012, "Failed to write durable journal", savedErrno));
        }
        offset += static_cast<size_t>(written);
    }

    if (::fsync(fd) == -1) {
        const int savedErrno = errno;
        (void)::close(fd);
        return std::unexpected(journalIoError(-90013, "Failed to flush durable journal", savedErrno));
    }
    if (::close(fd) == -1) {
        return std::unexpected(journalIoError(-90013, "Failed to close durable journal", errno));
    }
    return {};
#endif
}

} // namespace

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
    it->second.responseTimestampMs = unixMsNow();
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

DurableOrderJournal::DurableOrderJournal(std::string path)
    : m_path(std::move(path)) {
    auto loaded = loadFromFile();
    if (!loaded) {
        throw std::runtime_error("durable journal initialization failed: " + loaded.error().toString());
    }
    auto probe = appendDurably(m_path, "");
    if (!probe) {
        throw std::runtime_error("durable journal probe failed: " + probe.error().toString());
    }
}

std::expected<void, BinanceError> DurableOrderJournal::appendRecord(const std::string& op,
                                                                    const CorrelationId& correlationId,
                                                                    PlacementState state,
                                                                    std::optional<int64_t> binanceOrderId,
                                                                    const JournalEntry* entry) {
    std::ostringstream out;
    if (op == "R") {
        out << "R\t"
            << sanitizeToken(entry->correlationId) << '\t'
            << sanitizeToken(entry->symbol) << '\t'
            << sanitizeToken(entry->clientOrderId) << '\t'
            << sanitizeToken(entry->orderCategory) << '\t'
            << static_cast<int>(entry->side) << '\t'
            << static_cast<int>(entry->type) << '\t'
            << static_cast<int>(entry->positionSide) << '\t'
            << sanitizeToken(entry->quantity) << '\t'
            << sanitizeToken(entry->price) << '\t'
            << sanitizeToken(entry->requestParams) << '\t'
            << entry->sendTimestampMs << '\t'
            << static_cast<int>(entry->state) << '\t';
        if (entry->binanceOrderId.has_value()) {
            out << *entry->binanceOrderId;
        }
        out << '\t';
        if (entry->metadata.has_value() && entry->metadata->magic.has_value()) {
            out << *entry->metadata->magic;
        }
        out << '\t';
        if (entry->metadata.has_value() && entry->metadata->comment.has_value()) {
            out << sanitizeToken(*entry->metadata->comment);
        }
        out << '\t';
        if (entry->metadata.has_value() && entry->metadata->strategyTag.has_value()) {
            out << sanitizeToken(*entry->metadata->strategyTag);
        }
        out << '\t';
        if (entry->metadata.has_value() && entry->metadata->timeframe.has_value()) {
            out << sanitizeToken(*entry->metadata->timeframe);
        }
        out << '\n';
    } else {
        out << "U\t"
            << sanitizeToken(correlationId) << '\t'
            << static_cast<int>(state) << '\t';
        if (binanceOrderId.has_value()) {
            out << *binanceOrderId;
        }
        out << '\t' << unixMsNow() << '\n';
    }

    if (!out.good()) {
        return std::unexpected(BinanceError::fromApiResponse(-90012, "Failed to write durable journal"));
    }
    return appendDurably(m_path, out.str());
}

std::expected<void, BinanceError> DurableOrderJournal::loadFromFile() {
    std::ifstream in(m_path);
    if (!in.is_open()) {
        return {};
    }

    std::string line;
    int badLines = 0;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }

        try {
            auto parts = splitTabs(line);
            if (parts.empty()) {
                continue;
            }

            if (parts[0] == "R") {
                if (parts.size() < 13) {
                    continue;
                }
                JournalEntry entry;
                entry.correlationId = parts[1];
                entry.symbol = parts[2];
                entry.clientOrderId = parts[3];
                entry.orderCategory = parts[4];
                entry.side = parseSide(toInt(parts[5]));
                entry.type = parseType(toInt(parts[6]));
                entry.positionSide = parsePositionSide(toInt(parts[7]));
                entry.quantity = parts[8];
                entry.price = parts[9];
                entry.requestParams = parts[10];
                entry.sendTimestampMs = toInt64(parts[11]);
                entry.state = parsePlacementState(toInt(parts[12]));
                if (parts.size() > 13 && !parts[13].empty()) {
                    entry.binanceOrderId = toInt64(parts[13]);
                }

                if (parts.size() > 14) {
                    OrderMetadata metadata;
                    bool hasMetadata = false;
                    if (!parts[14].empty()) {
                        metadata.magic = toInt64(parts[14]);
                        hasMetadata = true;
                    }
                    if (parts.size() > 15 && !parts[15].empty()) {
                        metadata.comment = parts[15];
                        hasMetadata = true;
                    }
                    if (parts.size() > 16 && !parts[16].empty()) {
                        metadata.strategyTag = parts[16];
                        hasMetadata = true;
                    }
                    if (parts.size() > 17 && !parts[17].empty()) {
                        metadata.timeframe = parts[17];
                        hasMetadata = true;
                    } else if (!metadata.timeframe.has_value()) {
                        if (const auto derived = orders::detail::extractTimeframe(std::optional<OrderMetadata>{metadata});
                            derived.has_value()) {
                            metadata.timeframe = *derived;
                            hasMetadata = true;
                        }
                    }
                    if (hasMetadata) {
                        entry.metadata = metadata;
                    }
                }

                m_correlationByClientId[entry.clientOrderId] = entry.correlationId;
                m_entriesByCorrelationId[entry.correlationId] = std::move(entry);
                continue;
            }

            if (parts[0] == "U" && parts.size() >= 3) {
                const auto it = m_entriesByCorrelationId.find(parts[1]);
                if (it == m_entriesByCorrelationId.end()) {
                    continue;
                }
                it->second.state = parsePlacementState(toInt(parts[2]));
                if (parts.size() > 3 && !parts[3].empty()) {
                    it->second.binanceOrderId = toInt64(parts[3]);
                } else {
                    it->second.binanceOrderId = std::nullopt;
                }
                if (parts.size() > 4 && !parts[4].empty()) {
                    it->second.responseTimestampMs = toInt64(parts[4]);
                }
            }
        } catch (const std::exception&) {
            ++badLines;
        }
    }

    if (badLines > 0) {
        Logger::instance().log(
            LogLevel::Warning,
            "durable journal ignored corrupt lines count=" + std::to_string(badLines));
    }

    return {};
}

std::expected<void, BinanceError> DurableOrderJournal::recordIntent(JournalEntry entry) {
    std::scoped_lock lock(m_mutex);
    const auto correlationId = entry.correlationId;
    const auto clientOrderId = entry.clientOrderId;
    if (auto saved = appendRecord("R", correlationId, entry.state, entry.binanceOrderId, &entry); !saved) {
        return std::unexpected(saved.error());
    }
    m_entriesByCorrelationId[correlationId] = std::move(entry);
    m_correlationByClientId[clientOrderId] = correlationId;
    return {};
}

std::expected<void, BinanceError> DurableOrderJournal::updateState(
    CorrelationId id,
    PlacementState state,
    std::optional<int64_t> binanceOrderId) {
    std::scoped_lock lock(m_mutex);
    auto it = m_entriesByCorrelationId.find(id);
    if (it == m_entriesByCorrelationId.end()) {
        return std::unexpected(BinanceError::fromApiResponse(-90002, "Journal entry not found"));
    }

    if (auto saved = appendRecord("U", id, state, binanceOrderId, nullptr); !saved) {
        return std::unexpected(saved.error());
    }

    it->second.state = state;
    it->second.binanceOrderId = binanceOrderId;
    it->second.responseTimestampMs = unixMsNow();
    return {};
}

std::expected<std::vector<JournalEntry>, BinanceError> DurableOrderJournal::pendingReconcile() {
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

std::expected<std::optional<JournalEntry>, BinanceError> DurableOrderJournal::findByClientOrderId(
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
