#include "risk/risk_metrics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace engine {

namespace {

constexpr double kSecondsPerYear = 365.0 * 24.0 * 60.0 * 60.0;
constexpr double kMillisPerYear = kSecondsPerYear * 1000.0;

double stdDev(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double accum = 0.0;
    for (const double v : values) {
        const double d = v - mean;
        accum += d * d;
    }
    const double variance = accum / static_cast<double>(values.size() - 1);
    return variance > 0.0 ? std::sqrt(variance) : 0.0;
}

bool isTradeClose(const EquityPoint& p) {
    return p.source == "trade_close";
}

} // namespace

RiskMetrics::RiskMetrics(double riskFreeRateAnnual, int minDataPoints, std::chrono::minutes sampleInterval)
    : m_riskFreeRate(riskFreeRateAnnual),
      m_minDataPoints(std::max(2, minDataPoints)),
      m_sampleInterval(std::max(sampleInterval, std::chrono::minutes{1})) {}

RiskMetricsResult RiskMetrics::compute(
    const std::vector<SampledEquityPoint>& points,
    std::string_view windowKind,
    int64_t windowStartMs,
    int64_t windowEndMs,
    std::string_view basis) const {
    RiskMetricsResult out;
    out.windowKind = std::string(windowKind);
    out.windowStartMs = windowStartMs;
    out.windowEndMs = windowEndMs;
    out.basis = std::string(basis);
    out.computedAtMs = windowEndMs;
    out.dataPoints = static_cast<int>(points.size());

    if (points.size() < static_cast<size_t>(m_minDataPoints)) {
        out.valid = false;
        return out;
    }
    if (points.front().equity <= 0.0 || points.back().equity <= 0.0) {
        out.valid = false;
        return out;
    }
    const double annualReturnValue = annualizedReturn(points);
    if (!std::isfinite(annualReturnValue)) {
        out.valid = false;
        return out;
    }

    const auto returns = periodReturns(points);
    out.annualReturn = annualReturnValue;
    out.excessReturn = out.annualReturn - m_riskFreeRate;
    out.stdDevAll = annualizedStdDev(returns);
    out.stdDevDownside = annualizedDownsideStdDev(returns);
    out.ulcerIndex = computeUlcerIndex(points);
    out.maxDrawdown = computeMaxDrawdown(points);

    out.sharpeRatio = out.stdDevAll > 0.0 ? (out.excessReturn / out.stdDevAll) : 0.0;
    out.sortinoRatio = out.stdDevDownside > 0.0
        ? (out.excessReturn / out.stdDevDownside)
        : std::numeric_limits<double>::infinity();
    out.upi = out.ulcerIndex > 0.0
        ? (out.excessReturn / out.ulcerIndex)
        : std::numeric_limits<double>::infinity();

    out.valid = true;
    return out;
}

std::vector<double> RiskMetrics::periodReturns(const std::vector<SampledEquityPoint>& points) const {
    std::vector<double> out;
    if (points.size() < 2) {
        return out;
    }
    out.reserve(points.size() - 1);
    for (size_t i = 1; i < points.size(); ++i) {
        const double prev = points[i - 1].equity;
        const double curr = points[i].equity;
        if (prev <= 0.0) {
            continue;
        }
        out.push_back((curr - prev) / prev);
    }
    return out;
}

double RiskMetrics::annualizedReturn(const std::vector<SampledEquityPoint>& points) const {
    if (points.size() < 2) {
        return NAN;
    }
    const int64_t elapsedMs = points.back().timestampMs - points.front().timestampMs;
    if (elapsedMs <= 0) {
        return NAN;
    }
    const double years = static_cast<double>(elapsedMs) / kMillisPerYear;
    if (years <= 0.0) {
        return NAN;
    }
    const double ratio = points.back().equity / points.front().equity;
    if (ratio <= 0.0) {
        return NAN;
    }
    return std::pow(ratio, 1.0 / years) - 1.0;
}

double RiskMetrics::annualizedStdDev(const std::vector<double>& returns) const {
    const double base = stdDev(returns);
    if (base <= 0.0) {
        return 0.0;
    }
    const double periodsPerYear = kSecondsPerYear / (60.0 * static_cast<double>(m_sampleInterval.count()));
    return base * std::sqrt(periodsPerYear);
}

double RiskMetrics::annualizedDownsideStdDev(const std::vector<double>& returns) const {
    std::vector<double> downside;
    downside.reserve(returns.size());
    for (const double r : returns) {
        if (r < 0.0) {
            downside.push_back(r);
        }
    }
    const double base = stdDev(downside);
    if (base <= 0.0) {
        return 0.0;
    }
    const double periodsPerYear = kSecondsPerYear / (60.0 * static_cast<double>(m_sampleInterval.count()));
    return base * std::sqrt(periodsPerYear);
}

double RiskMetrics::computeUlcerIndex(const std::vector<SampledEquityPoint>& points) {
    if (points.empty()) {
        return 0.0;
    }
    double high = points.front().equity;
    double sumSquares = 0.0;
    size_t n = 0;
    for (const auto& p : points) {
        high = std::max(high, p.equity);
        if (high <= 0.0) {
            continue;
        }
        const double dd = (p.equity - high) / high;
        sumSquares += dd * dd;
        ++n;
    }
    if (n == 0) {
        return 0.0;
    }
    return std::sqrt(sumSquares / static_cast<double>(n));
}

double RiskMetrics::computeMaxDrawdown(const std::vector<SampledEquityPoint>& points) {
    if (points.empty()) {
        return 0.0;
    }
    double high = points.front().equity;
    double worst = 0.0;
    for (const auto& p : points) {
        high = std::max(high, p.equity);
        if (high <= 0.0) {
            continue;
        }
        const double dd = (p.equity - high) / high;
        worst = std::min(worst, dd);
    }
    return worst;
}

std::vector<SampledEquityPoint> sampleEquity(
    const std::vector<EquityPoint>& raw,
    int64_t windowStartMs,
    int64_t windowEndMs,
    std::chrono::minutes sampleInterval) {
    if (windowEndMs < windowStartMs || sampleInterval.count() <= 0) {
        return {};
    }
    const int64_t bucketMs = std::chrono::duration_cast<std::chrono::milliseconds>(sampleInterval).count();
    if (bucketMs <= 0) {
        return {};
    }

    struct BucketChoice {
        SampledEquityPoint point;
        bool tradeClose{false};
        int64_t id{0};
    };

    std::unordered_map<int64_t, BucketChoice> byBucket;
    byBucket.reserve(raw.size());

    for (const auto& p : raw) {
        if (p.timestampMs < windowStartMs || p.timestampMs > windowEndMs) {
            continue;
        }
        const int64_t bucket = (p.timestampMs - windowStartMs) / bucketMs;
        const bool tradeClose = isTradeClose(p);
        auto it = byBucket.find(bucket);
        if (it == byBucket.end()) {
            byBucket.emplace(
                bucket,
                BucketChoice{
                    .point = SampledEquityPoint{p.timestampMs, p.equity},
                    .tradeClose = tradeClose,
                    .id = p.id,
                });
            continue;
        }
        const BucketChoice& current = it->second;
        bool replace = false;
        if (p.timestampMs > current.point.timestampMs) {
            replace = true;
        } else if (p.timestampMs == current.point.timestampMs) {
            if (tradeClose && !current.tradeClose) {
                replace = true;
            } else if (tradeClose == current.tradeClose && p.id > current.id) {
                replace = true;
            }
        }
        if (replace) {
            it->second = BucketChoice{
                .point = SampledEquityPoint{p.timestampMs, p.equity},
                .tradeClose = tradeClose,
                .id = p.id,
            };
        }
    }

    std::vector<SampledEquityPoint> sampled;
    sampled.reserve(byBucket.size());
    for (const auto& [bucket, best] : byBucket) {
        (void)bucket;
        sampled.push_back(best.point);
    }
    std::sort(
        sampled.begin(),
        sampled.end(),
        [](const SampledEquityPoint& a, const SampledEquityPoint& b) {
            return a.timestampMs < b.timestampMs;
        });
    return sampled;
}

} // namespace engine
