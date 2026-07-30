/*
 * Copyright 2026 PixelsDB.
 */

#include "ScanComparator.h"

namespace pixels
{
namespace inspector
{

bool compareOrderedCandidates(
        const ScanPlan &plan, const ScanCandidate &left,
        const ScanCandidate &right,
        const ScanNonNullValueComparator &compareValue,
        int &comparison)
{
    if (left.keys.size() != plan.order.size()
        || right.keys.size() != plan.order.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < plan.order.size(); ++index)
    {
        const ScanOrderKey &key = plan.order[index];
        const bool leftNull = left.keys[index] == "null";
        const bool rightNull = right.keys[index] == "null";
        if (leftNull || rightNull)
        {
            if (leftNull != rightNull)
            {
                comparison = leftNull == !key.nullsLast ? -1 : 1;
                return true;
            }
            continue;
        }
        if (!compareValue(
                    index, left.keys[index], right.keys[index],
                    comparison))
        {
            return false;
        }
        if (comparison != 0)
        {
            if (key.descending)
            {
                comparison = -comparison;
            }
            return true;
        }
    }
    comparison = left.absoluteRow < right.absoluteRow ? -1
                 : left.absoluteRow > right.absoluteRow ? 1 : 0;
    return true;
}

} // namespace inspector
} // namespace pixels
