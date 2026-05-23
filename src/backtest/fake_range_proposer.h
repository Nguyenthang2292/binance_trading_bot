#pragma once

#include "backtest/range_proposer.h"

namespace backtest {

// Returns the adapter-provided defaults unchanged. Useful for Phase 2 tests
// before the Python+Gemini proposer is wired in.
class FakeRangeProposer : public IRangeProposer {
public:
    Result propose(const RangeProposalRequest& req) override {
        return Output{
            .ranges = req.defaultRanges,
            .notes  = "Fake proposal — uses adapter defaults"
        };
    }
};

}  // namespace backtest
