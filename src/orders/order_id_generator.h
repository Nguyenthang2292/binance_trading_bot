#pragma once

#include "common/expected_compat.h"
#include "orders/order_types.h"
#include "types/error.h"

#include <atomic>
#include <string>

class OrderIdGenerator {
public:
    explicit OrderIdGenerator(std::string nameSpace);

    std::expected<ClientOrderId, BinanceError> generateClientOrderId();
    CorrelationId generateCorrelationId();
    std::expected<void, BinanceError> validateClientOrderId(const ClientOrderId& id) const;

private:
    std::string m_namespace;
    std::atomic<uint64_t> m_counter{0};
};
