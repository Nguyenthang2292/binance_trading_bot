#include <gtest/gtest.h>

#include "orders/order_types.h"

#include <type_traits>

namespace {

static_assert(std::is_same_v<ClientAlgoId, std::string>);
static_assert(!std::is_aggregate_v<CloseByMarketDraft>);

} // namespace

TEST(OrderTypesTest, ClientAlgoIdAliasExists) {
    SUCCEED();
}
