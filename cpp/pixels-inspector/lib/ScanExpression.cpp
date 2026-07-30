/*
 * Copyright 2026 PixelsDB.
 */

#include "ScanExpression.h"

#include <vector>

namespace pixels
{
namespace inspector
{
namespace
{

ScanTruth logicalNot(ScanTruth value)
{
    if (value == ScanTruth::UNKNOWN)
    {
        return value;
    }
    return value == ScanTruth::TRUE_VALUE
           ? ScanTruth::FALSE_VALUE : ScanTruth::TRUE_VALUE;
}

ScanTruth logicalAnd(ScanTruth left, ScanTruth right)
{
    if (left == ScanTruth::FALSE_VALUE
        || right == ScanTruth::FALSE_VALUE)
    {
        return ScanTruth::FALSE_VALUE;
    }
    return left == ScanTruth::UNKNOWN || right == ScanTruth::UNKNOWN
           ? ScanTruth::UNKNOWN : ScanTruth::TRUE_VALUE;
}

ScanTruth logicalOr(ScanTruth left, ScanTruth right)
{
    if (left == ScanTruth::TRUE_VALUE
        || right == ScanTruth::TRUE_VALUE)
    {
        return ScanTruth::TRUE_VALUE;
    }
    return left == ScanTruth::UNKNOWN || right == ScanTruth::UNKNOWN
           ? ScanTruth::UNKNOWN : ScanTruth::FALSE_VALUE;
}

} // namespace

bool evaluateScanExpression(
        const ScanPlan &plan, const ScanLeafEvaluator &evaluateLeaf,
        ScanTruth &truth)
{
    std::vector<ScanTruth> stack;
    std::size_t predicateIndex = 0;
    for (const ScanExpressionNode &node : plan.expression)
    {
        if (node.kind == ScanNodeKind::TRUE_VALUE)
        {
            stack.push_back(ScanTruth::TRUE_VALUE);
            continue;
        }
        if (node.kind == ScanNodeKind::PREDICATE)
        {
            ScanTruth leaf = ScanTruth::UNKNOWN;
            if (!evaluateLeaf(predicateIndex++, leaf))
            {
                return false;
            }
            stack.push_back(leaf);
            continue;
        }
        if (stack.size() < node.childCount)
        {
            return false;
        }
        if (node.kind == ScanNodeKind::NOT)
        {
            stack.back() = logicalNot(stack.back());
            continue;
        }
        const std::size_t first = stack.size() - node.childCount;
        ScanTruth combined =
                node.kind == ScanNodeKind::AND
                ? ScanTruth::TRUE_VALUE : ScanTruth::FALSE_VALUE;
        for (std::size_t index = first; index < stack.size(); ++index)
        {
            combined = node.kind == ScanNodeKind::AND
                       ? logicalAnd(combined, stack[index])
                       : logicalOr(combined, stack[index]);
        }
        stack.resize(first);
        stack.push_back(combined);
    }
    if (stack.size() != 1)
    {
        return false;
    }
    truth = stack.front();
    return true;
}

bool evaluateScanTruthSet(
        const ScanPlan &plan,
        const ScanLeafTruthSetEvaluator &evaluateLeaf,
        std::uint8_t &truthSet)
{
    std::vector<std::uint8_t> stack;
    std::size_t predicateIndex = 0;
    const auto combine = [](
                                 std::uint8_t left,
                                 std::uint8_t right,
                                 bool conjunction)
    {
        std::uint8_t result = 0;
        for (std::uint8_t leftValue = 0; leftValue < 3; ++leftValue)
        {
            if ((left & (1U << leftValue)) == 0)
            {
                continue;
            }
            for (std::uint8_t rightValue = 0;
                 rightValue < 3; ++rightValue)
            {
                if ((right & (1U << rightValue)) == 0)
                {
                    continue;
                }
                const ScanTruth value =
                        conjunction
                        ? logicalAnd(
                                static_cast<ScanTruth>(leftValue),
                                static_cast<ScanTruth>(rightValue))
                        : logicalOr(
                                static_cast<ScanTruth>(leftValue),
                                static_cast<ScanTruth>(rightValue));
                result |= static_cast<std::uint8_t>(
                        1U << static_cast<std::uint8_t>(value));
            }
        }
        return result;
    };
    for (const ScanExpressionNode &node : plan.expression)
    {
        if (node.kind == ScanNodeKind::TRUE_VALUE)
        {
            stack.push_back(
                    1U << static_cast<std::uint8_t>(
                            ScanTruth::TRUE_VALUE));
            continue;
        }
        if (node.kind == ScanNodeKind::PREDICATE)
        {
            std::uint8_t leaf = 0;
            if (!evaluateLeaf(predicateIndex++, leaf) || leaf == 0)
            {
                return false;
            }
            stack.push_back(leaf);
            continue;
        }
        if (stack.size() < node.childCount)
        {
            return false;
        }
        if (node.kind == ScanNodeKind::NOT)
        {
            std::uint8_t inverted = 0;
            for (std::uint8_t value = 0; value < 3; ++value)
            {
                if ((stack.back() & (1U << value)) != 0)
                {
                    inverted |= static_cast<std::uint8_t>(
                            1U << static_cast<std::uint8_t>(
                                    logicalNot(
                                            static_cast<ScanTruth>(
                                                    value))));
                }
            }
            stack.back() = inverted;
            continue;
        }
        const std::size_t first = stack.size() - node.childCount;
        std::uint8_t combined =
                node.kind == ScanNodeKind::AND
                ? static_cast<std::uint8_t>(
                        1U << static_cast<std::uint8_t>(
                                ScanTruth::TRUE_VALUE))
                : static_cast<std::uint8_t>(
                        1U << static_cast<std::uint8_t>(
                                ScanTruth::FALSE_VALUE));
        for (std::size_t index = first; index < stack.size(); ++index)
        {
            combined = combine(
                    combined, stack[index],
                    node.kind == ScanNodeKind::AND);
        }
        stack.resize(first);
        stack.push_back(combined);
    }
    if (stack.size() != 1)
    {
        return false;
    }
    truthSet = stack.front();
    return true;
}

} // namespace inspector
} // namespace pixels
