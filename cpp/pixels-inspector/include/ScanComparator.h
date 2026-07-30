/*
 * Copyright 2026 PixelsDB.
 *
 * One total-order owner for heap retention, final sort, and continuation.
 */

#ifndef PIXELS_INSPECTOR_SCANCOMPARATOR_H
#define PIXELS_INSPECTOR_SCANCOMPARATOR_H

#include "ScanRuntime.h"

#include <cstddef>
#include <functional>

namespace pixels
{
namespace inspector
{

using ScanNonNullValueComparator =
        std::function<bool(
                std::size_t, const std::string &,
                const std::string &, int &)>;

[[nodiscard]] bool compareOrderedCandidates(
        const ScanPlan &plan, const ScanCandidate &left,
        const ScanCandidate &right,
        const ScanNonNullValueComparator &compareValue,
        int &comparison);

} // namespace inspector
} // namespace pixels

#endif
