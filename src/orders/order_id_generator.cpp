#include "orders/order_id_generator.h"

#include <openssl/rand.h>

#include <chrono>
#include <iomanip>
#include <regex>
#include <sstream>

namespace {

constexpr size_t kMaxClientOrderIdLen = 36;
constexpr size_t kMaxNamespaceLen = 8;
const std::regex kClientOrderIdPattern("^[A-Za-z0-9._:@/\\-]{1,36}$");
const std::regex kNamespacePattern("^[A-Za-z0-9_]{1,8}$");

int64_t unixMsNow() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

} // namespace

OrderIdGenerator::OrderIdGenerator(std::string nameSpace)
    : m_namespace(std::move(nameSpace)) {}

compat::expected<void, BinanceError> OrderIdGenerator::validateClientOrderId(const ClientOrderId& id) const {
    if (!std::regex_match(id, kClientOrderIdPattern)) {
        return compat::unexpected(BinanceError::fromApiResponse(
            -90003, "Invalid clientOrderId format. Allowed charset: [A-Za-z0-9._:@/\\-], length 1..36"));
    }
    return {};
}

compat::expected<ClientOrderId, BinanceError> OrderIdGenerator::generateClientOrderId() {
    if (!std::regex_match(m_namespace, kNamespacePattern)) {
        return compat::unexpected(BinanceError::fromApiResponse(
            -90004, "Invalid client id namespace. Expected [A-Za-z0-9_], length 1..8"));
    }

    const auto ts = unixMsNow();
    uint32_t entropy = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&entropy), sizeof(entropy)) != 1) {
        return compat::unexpected(BinanceError::fromApiResponse(
            -90008, "Failed to generate cryptographic randomness for clientOrderId"));
    }
    std::ostringstream out;
    out << "btb_" << m_namespace << "_" << ts << "_";
    out << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << entropy;
    auto id = out.str();
    if (id.size() > kMaxClientOrderIdLen) {
        return compat::unexpected(BinanceError::fromApiResponse(
            -90005, "Generated clientOrderId exceeds 36 characters"));
    }
    return id;
}

CorrelationId OrderIdGenerator::generateCorrelationId() {
    const auto ts = unixMsNow();
    const auto seq = m_counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream out;
    out << "corr_" << ts << "_" << seq;
    return out.str();
}
