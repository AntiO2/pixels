/*
 * Copyright 2026 PixelsDB.
 */

#include "ScanRuntime.h"

#include <algorithm>

namespace pixels
{
namespace inspector
{

ScanTopKResult insertTopK(
        std::vector<ScanCandidate> &candidates,
        ScanCandidate candidate, std::size_t retain,
        std::size_t maxKeyBytes, std::size_t &retainedKeyBytes,
        const ScanCandidateComparator &compare)
{
    if (retain == 0)
    {
        return ScanTopKResult::OK;
    }
    if (candidate.keyBytes > maxKeyBytes)
    {
        return ScanTopKResult::KEY_BUDGET_EXCEEDED;
    }
    bool valid = true;
    const auto better = [&compare, &valid](
                                const ScanCandidate &left,
                                const ScanCandidate &right)
    {
        int comparison = 0;
        if (!compare(left, right, comparison))
        {
            valid = false;
            return false;
        }
        return comparison < 0;
    };
    if (candidates.size() == retain)
    {
        int comparison = 0;
        if (!compare(candidate, candidates.front(), comparison))
        {
            return ScanTopKResult::COMPARISON_FAILED;
        }
        if (comparison >= 0)
        {
            return ScanTopKResult::OK;
        }
        if (retainedKeyBytes - candidates.front().keyBytes
            > maxKeyBytes - candidate.keyBytes)
        {
            return ScanTopKResult::KEY_BUDGET_EXCEEDED;
        }
        std::pop_heap(candidates.begin(), candidates.end(), better);
        retainedKeyBytes -= candidates.back().keyBytes;
        candidates.pop_back();
    }
    else if (retainedKeyBytes > maxKeyBytes - candidate.keyBytes)
    {
        return ScanTopKResult::KEY_BUDGET_EXCEEDED;
    }
    retainedKeyBytes += candidate.keyBytes;
    candidates.push_back(std::move(candidate));
    std::push_heap(candidates.begin(), candidates.end(), better);
    if (!valid)
    {
        return ScanTopKResult::COMPARISON_FAILED;
    }
    return ScanTopKResult::OK;
}

bool sortAndRetainBest(
        std::vector<ScanCandidate> &candidates, std::size_t retain,
        const ScanCandidateComparator &compare)
{
    bool valid = true;
    std::sort(
            candidates.begin(), candidates.end(),
            [&compare, &valid](
                    const ScanCandidate &left,
                    const ScanCandidate &right)
            {
                int comparison = 0;
                if (!compare(left, right, comparison))
                {
                    valid = false;
                    return false;
                }
                return comparison < 0;
            });
    if (!valid)
    {
        return false;
    }
    if (candidates.size() > retain)
    {
        candidates.resize(retain);
    }
    return true;
}

} // namespace inspector
} // namespace pixels
