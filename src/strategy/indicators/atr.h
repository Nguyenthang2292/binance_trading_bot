#pragma once

#include "types/market.h"

#include <vector>

namespace strategy::indicators {

std::vector<double> atr(const std::vector<Kline>& klines, int period = 14);
double lastAtr(const std::vector<Kline>& klines, int period = 14);

} // namespace strategy::indicators

