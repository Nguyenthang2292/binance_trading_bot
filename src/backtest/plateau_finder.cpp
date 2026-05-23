#include "backtest/plateau_finder.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace backtest {

namespace {

// Helper to calculate Euclidean distance in grid steps.
// For this, we assume parameters are numeric and we check steps.
// However, the v2.0 design says "build neighborhood: +/- radius steps in each tunable dim".
// To do this simply, we find the closest point in the grid or just build it.
// Wait, the grid might have varying steps, but since it was built from ParamRange, 
// points that are "within radius steps" can be found by just sorting and taking indices,
// or by collecting the unique values for each dimension from the scoredGrid.

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

double getSensitivity(const std::vector<ScoredPoint>& grid, const ParamPoint& best, const std::string& dim, const std::vector<double>& dimVals) {
    // Find gradient around best.
    double bestVal = best.at(dim);
    auto it = std::find(dimVals.begin(), dimVals.end(), bestVal);
    if (it == dimVals.end()) return 0.0;
    
    size_t idx = std::distance(dimVals.begin(), it);
    
    // Find point with 'dim' one step down
    double oosDown = 0.0, oosUp = 0.0;
    bool foundDown = false, foundUp = false;
    
    if (idx > 0) {
        double valDown = dimVals[idx - 1];
        for (const auto& sp : grid) {
            bool match = true;
            for (const auto& [k, v] : best) {
                if (k == dim) {
                    if (sp.point.at(k) != valDown) { match = false; break; }
                } else {
                    if (sp.point.at(k) != v) { match = false; break; }
                }
            }
            if (match) { oosDown = sp.oosSortino; foundDown = true; break; }
        }
    }
    
    if (idx + 1 < dimVals.size()) {
        double valUp = dimVals[idx + 1];
        for (const auto& sp : grid) {
            bool match = true;
            for (const auto& [k, v] : best) {
                if (k == dim) {
                    if (sp.point.at(k) != valUp) { match = false; break; }
                } else {
                    if (sp.point.at(k) != v) { match = false; break; }
                }
            }
            if (match) { oosUp = sp.oosSortino; foundUp = true; break; }
        }
    }
    
    double bestOos = 0.0;
    for (const auto& sp : grid) {
        if (sp.point == best) {
            bestOos = sp.oosSortino;
            break;
        }
    }
    
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
            sensitivities.push_back({getSensitivity(scoredGrid, best->point, dim, dims[dim]), dim});
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
    std::vector<const ScoredPoint*> neighborhood;
    for (const auto& sp : scoredGrid) {
        bool inNeighborhood = true;
        for (const auto& dim : activeDims) {
            double val = sp.point.at(dim);
            double bestVal = best->point.at(dim);
            
            auto itVal = std::find(dims[dim].begin(), dims[dim].end(), val);
            auto itBest = std::find(dims[dim].begin(), dims[dim].end(), bestVal);
            
            if (itVal != dims[dim].end() && itBest != dims[dim].end()) {
                int dist = std::abs(static_cast<int>(std::distance(dims[dim].begin(), itVal)) - 
                                    static_cast<int>(std::distance(dims[dim].begin(), itBest)));
                if (dist > neighborhoodRadius) {
                    inNeighborhood = false;
                    break;
                }
            }
        }
        
        // For inactive dims, it must match the best point exactly
        for (const auto& [k, v] : dims) {
            if (std::find(activeDims.begin(), activeDims.end(), k) == activeDims.end()) {
                const auto spIt   = sp.point.find(k);
                const auto bestIt = best->point.find(k);
                if (spIt == sp.point.end() || bestIt == best->point.end() ||
                    spIt->second != bestIt->second) {
                    inNeighborhood = false;
                    break;
                }
            }
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
    
    // Find sortino for center if it was evaluated
    for (const auto& sp : scoredGrid) {
        if (sp.point == center) {
            res.centerSortinoIS = sp.isSortino;
            res.centerSortinoOOS = sp.oosSortino;
            break;
        }
    }
    
    return res;
}

}  // namespace backtest
