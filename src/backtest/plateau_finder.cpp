/**
 * @file plateau_finder.cpp
 * @brief Plateau detection over scored parameter grids.
 */

#include "backtest/plateau_finder.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace backtest {

namespace {

// Neighborhood membership is evaluated in discrete grid indices, not raw float
// deltas. Each dimension's unique values are sorted from scoredGrid, then points
// are mapped to nearest indices so +/- radius checks remain stable across steps.

std::map<std::string, std::vector<double>> extractDimensions(const std::vector<ScoredPoint>& grid) {
    std::map<std::string, std::set<double>> dims;
    for (const auto& sp : grid) {
        for (const auto& [k, v] : sp.point) {
            dims[k].insert(v);
        }
    }
    
    std::map<std::string, std::vector<double>> result;
    for (const auto& [k, vSet] : dims) {
        result[k] = std::vector<double>(vSet.begin(), vSet.end());
    }
    return result;
}

int nearestValueIndex(const std::vector<double>& values, double target) {
    if (values.empty()) return -1;
    auto it = std::lower_bound(values.begin(), values.end(), target);
    if (it == values.begin()) return 0;
    if (it == values.end()) return static_cast<int>(values.size() - 1);

    const int hi = static_cast<int>(std::distance(values.begin(), it));
    const int lo = hi - 1;
    return std::abs(values[hi] - target) < std::abs(target - values[lo]) ? hi : lo;
}

using IndexedPoint = std::map<std::string, int>;

IndexedPoint indexPoint(
    const ParamPoint& point,
    const std::map<std::string, std::vector<double>>& dims) {
    IndexedPoint indexed;
    for (const auto& [dim, values] : dims) {
        auto it = point.find(dim);
        if (it == point.end()) continue;
        const int idx = nearestValueIndex(values, it->second);
        if (idx >= 0) indexed.emplace(dim, idx);
    }
    return indexed;
}

bool sameAllDimsExcept(
    const IndexedPoint& lhs,
    const IndexedPoint& rhs,
    const std::string& excludedDim) {
    for (const auto& [dim, lhsIdx] : lhs) {
        if (dim == excludedDim) continue;
        const auto rhsIt = rhs.find(dim);
        if (rhsIt == rhs.end() || rhsIt->second != lhsIdx) {
            return false;
        }
    }
    return true;
}

bool sameDims(
    const IndexedPoint& lhs,
    const IndexedPoint& rhs,
    const std::vector<std::string>& dims) {
    for (const auto& dim : dims) {
        const auto lhsIt = lhs.find(dim);
        const auto rhsIt = rhs.find(dim);
        if (lhsIt == lhs.end() || rhsIt == rhs.end() || lhsIt->second != rhsIt->second) {
            return false;
        }
    }
    return true;
}

double getSensitivity(
    const std::vector<ScoredPoint>& grid,
    const std::unordered_map<const ScoredPoint*, IndexedPoint>& pointIndices,
    const ScoredPoint& best,
    const std::string& dim,
    const std::vector<double>& dimVals) {
    // Find gradient around best.
    const auto bestIt = pointIndices.find(&best);
    if (bestIt == pointIndices.end()) return 0.0;

    const auto bestDimIt = bestIt->second.find(dim);
    if (bestDimIt == bestIt->second.end()) return 0.0;
    const int idx = bestDimIt->second;
    
    // Find point with 'dim' one step down
    double oosDown = 0.0, oosUp = 0.0;
    bool foundDown = false, foundUp = false;
    
    if (idx > 0 && idx <= static_cast<int>(dimVals.size() - 1)) {
        const int idxDown = idx - 1;
        for (const auto& sp : grid) {
            const auto spIdxIt = pointIndices.find(&sp);
            if (spIdxIt == pointIndices.end()) continue;
            const auto dimIt = spIdxIt->second.find(dim);
            if (dimIt == spIdxIt->second.end()) continue;
            const bool match = dimIt->second == idxDown &&
                sameAllDimsExcept(spIdxIt->second, bestIt->second, dim);
            if (match) { oosDown = sp.oosSortino; foundDown = true; break; }
        }
    }
    
    if (idx >= 0 && idx + 1 < static_cast<int>(dimVals.size())) {
        const int idxUp = idx + 1;
        for (const auto& sp : grid) {
            const auto spIdxIt = pointIndices.find(&sp);
            if (spIdxIt == pointIndices.end()) continue;
            const auto dimIt = spIdxIt->second.find(dim);
            if (dimIt == spIdxIt->second.end()) continue;
            const bool match = dimIt->second == idxUp &&
                sameAllDimsExcept(spIdxIt->second, bestIt->second, dim);
            if (match) { oosUp = sp.oosSortino; foundUp = true; break; }
        }
    }
    
    const double bestOos = best.oosSortino;
    
    double gradDown = foundDown ? std::abs(bestOos - oosDown) : 0.0;
    double gradUp = foundUp ? std::abs(bestOos - oosUp) : 0.0;
    
    return std::max(gradDown, gradUp);
}

} // namespace

std::optional<PlateauResult> PlateauFinder::find(
    const std::vector<ScoredPoint>& scoredGrid,
    const std::vector<ParamConstraint>& constraints,
    int neighborhoodRadius,
    int maxNeighborhoodSize,
    double minPassFraction) {
    
    if (scoredGrid.empty()) return std::nullopt;

    // 1. Pick best surviving
    const ScoredPoint* best = nullptr;
    for (const auto& sp : scoredGrid) {
        if (sp.passedFilters) {
            if (!best || sp.oosSortino > best->oosSortino) {
                best = &sp;
            }
        }
    }
    
    if (!best) return std::nullopt;

    // 2. Build neighborhood
    auto dims = extractDimensions(scoredGrid);
    std::unordered_map<const ScoredPoint*, IndexedPoint> pointIndices;
    pointIndices.reserve(scoredGrid.size());
    for (const auto& sp : scoredGrid) {
        pointIndices.emplace(&sp, indexPoint(sp.point, dims));
    }

    std::vector<std::string> activeDims;
    for (const auto& [k, v] : dims) {
        activeDims.push_back(k);
    }
    
    // Check neighborhood size
    int size = 1;
    for (size_t i = 0; i < activeDims.size(); ++i) {
        size *= (2 * neighborhoodRadius + 1); // Approximate, bounded by grid edges
    }
    
    if (size > maxNeighborhoodSize) {
        // Restrict to top-N most sensitive
        std::vector<std::pair<double, std::string>> sensitivities;
        for (const auto& dim : activeDims) {
            sensitivities.push_back({getSensitivity(scoredGrid, pointIndices, *best, dim, dims[dim]), dim});
        }
        
        // Sort descending by sensitivity
        std::sort(sensitivities.rbegin(), sensitivities.rend());
        
        // Keep dims until size <= maxNeighborhoodSize
        activeDims.clear();
        int curSize = 1;
        for (const auto& s : sensitivities) {
            int dimSize = std::min<int>(dims[s.second].size(), 2 * neighborhoodRadius + 1);
            if (curSize * dimSize <= maxNeighborhoodSize || activeDims.empty()) {
                curSize *= dimSize;
                activeDims.push_back(s.second);
            }
        }
    }
    
    // Collect neighborhood points
    const auto bestIndexedIt = pointIndices.find(best);
    if (bestIndexedIt == pointIndices.end()) return std::nullopt;

    std::vector<std::string> inactiveDims;
    inactiveDims.reserve(dims.size());
    for (const auto& [dim, _] : dims) {
        if (std::find(activeDims.begin(), activeDims.end(), dim) == activeDims.end()) {
            inactiveDims.push_back(dim);
        }
    }

    std::vector<const ScoredPoint*> neighborhood;
    for (const auto& sp : scoredGrid) {
        const auto spIndexedIt = pointIndices.find(&sp);
        if (spIndexedIt == pointIndices.end()) continue;

        bool inNeighborhood = true;
        for (const auto& dim : activeDims) {
            const auto spDimIt = spIndexedIt->second.find(dim);
            const auto bestDimIt = bestIndexedIt->second.find(dim);
            if (spDimIt == spIndexedIt->second.end() || bestDimIt == bestIndexedIt->second.end()) {
                inNeighborhood = false;
                break;
            }
            const int dist = std::abs(spDimIt->second - bestDimIt->second);
            if (dist > neighborhoodRadius) {
                inNeighborhood = false;
                break;
            }
        }
        
        // For inactive dims, it must match the best point exactly
        if (inNeighborhood && !sameDims(spIndexedIt->second, bestIndexedIt->second, inactiveDims)) {
            inNeighborhood = false;
        }
        
        if (inNeighborhood) {
            neighborhood.push_back(&sp);
        }
    }
    
    // 4. Survivors
    std::vector<ParamPoint> survivors;
    for (const auto* sp : neighborhood) {
        if (sp->passedFilters) {
            survivors.push_back(sp->point);
        }
    }
    
    // 5. Check fraction
    if (neighborhood.empty()) return std::nullopt;
    double passFrac = static_cast<double>(survivors.size()) / neighborhood.size();
    if (passFrac < minPassFraction) return std::nullopt;
    
    // 6. Center = arithmetic mean
    ParamPoint center;
    for (const auto& [k, v] : dims) {
        double sum = 0.0;
        for (const auto& s : survivors) {
            sum += s.at(k);
        }
        double mean = sum / survivors.size();
        
        // Snap to nearest in grid
        auto it = std::min_element(v.begin(), v.end(), [mean](double a, double b) {
            return std::abs(a - mean) < std::abs(b - mean);
        });
        center[k] = *it;
    }
    
    // Revalidate constraints
    // If invalid, fallback to the best point
    bool valid = true;
    for (const auto& c : constraints) {
        if (center.count(c.left) && center.count(c.right)) {
            if (c.kind == ParamConstraint::Kind::LessThan) {
                if (!(center[c.left] < center[c.right])) valid = false;
            } else {
                if (!(center[c.left] <= center[c.right])) valid = false;
            }
        }
    }
    
    if (!valid) {
        center = best->point;
    }
    
    PlateauResult res;
    res.center = center;
    res.survivors = std::move(survivors);
    
    std::vector<std::string> allDims;
    allDims.reserve(dims.size());
    for (const auto& [dim, _] : dims) {
        allDims.push_back(dim);
    }
    const auto centerIndexed = indexPoint(center, dims);

    // Find sortino for center if it was evaluated
    for (const auto& sp : scoredGrid) {
        const auto spIndexedIt = pointIndices.find(&sp);
        if (spIndexedIt != pointIndices.end() && sameDims(spIndexedIt->second, centerIndexed, allDims)) {
            res.centerSortinoIS = sp.isSortino;
            res.centerSortinoOOS = sp.oosSortino;
            break;
        }
    }
    
    return res;
}

}  // namespace backtest
