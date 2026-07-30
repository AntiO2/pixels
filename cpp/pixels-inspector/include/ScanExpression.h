/*
 * Copyright 2026 PixelsDB.
 */

#ifndef PIXELS_INSPECTOR_SCANEXPRESSION_H
#define PIXELS_INSPECTOR_SCANEXPRESSION_H

#include "ScanPlan.h"

#include <cstddef>
#include <functional>

namespace pixels
{
namespace inspector
{

enum class ScanTruth : std::uint8_t
{
    FALSE_VALUE = 0,
    TRUE_VALUE = 1,
    UNKNOWN = 2
};

using ScanLeafEvaluator =
        std::function<bool(std::size_t, ScanTruth &)>;
using ScanLeafTruthSetEvaluator =
        std::function<bool(std::size_t, std::uint8_t &)>;

[[nodiscard]] bool evaluateScanExpression(
        const ScanPlan &plan, const ScanLeafEvaluator &evaluateLeaf,
        ScanTruth &truth);

[[nodiscard]] bool evaluateScanTruthSet(
        const ScanPlan &plan,
        const ScanLeafTruthSetEvaluator &evaluateLeaf,
        std::uint8_t &truthSet);

} // namespace inspector
} // namespace pixels

#endif
