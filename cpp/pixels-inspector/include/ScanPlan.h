/*
 * Copyright 2026 PixelsDB.
 *
 * Bounded, pointer-free scan-v2 plan representation.
 */

#ifndef PIXELS_INSPECTOR_SCANPLAN_H
#define PIXELS_INSPECTOR_SCANPLAN_H

#include "format/ByteSpan.h"
#include "format/FormatError.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pixels
{
namespace inspector
{

enum class ScanNodeKind : std::uint8_t
{
    TRUE_VALUE = 0,
    PREDICATE = 1,
    AND = 2,
    OR = 3,
    NOT = 4
};

struct ScanExpressionNode
{
    ScanNodeKind kind = ScanNodeKind::TRUE_VALUE;
    std::uint8_t filterOperator = 0;
    std::uint16_t childCount = 0;
    std::uint32_t column = 0;
    std::string literal;
    std::uint8_t depth = 1;
};

struct ScanOrderKey
{
    std::uint32_t column = 0;
    bool descending = false;
    bool nullsLast = false;
};

struct ScanPlan
{
    bool projectionAll = false;
    std::vector<std::uint32_t> projection;
    std::vector<ScanExpressionNode> expression;
    std::vector<ScanOrderKey> order;
    std::uint64_t offset = 0;
    std::uint32_t limit = 20;
    std::string cursor;
};

[[nodiscard]] bool parseScanPlan(
        const format::ByteSpan &packet, ScanPlan &plan,
        format::FormatError &error);

} // namespace inspector
} // namespace pixels

#endif // PIXELS_INSPECTOR_SCANPLAN_H
