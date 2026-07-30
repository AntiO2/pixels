/*
 * Copyright 2026 PixelsDB.
 */

#include "ScanPlan.h"

#include "format/VariableLengthDecoder.h"

#include <algorithm>
#include <limits>
#include <set>

namespace pixels
{
namespace inspector
{
namespace
{

constexpr std::uint32_t HEADER_BYTES = 48;
constexpr std::uint32_t NODE_BYTES = 20;
constexpr std::uint32_t ORDER_BYTES = 8;
constexpr std::uint32_t MAX_PACKET_BYTES = 131072;
constexpr std::uint32_t MAX_PROJECTION = 128;
constexpr std::uint32_t MAX_NODES = 64;
constexpr std::uint32_t MAX_LEAVES = 32;
constexpr std::uint32_t MAX_ORDER = 8;
constexpr std::uint32_t MAX_LITERAL = 4096;
constexpr std::uint32_t MAX_LITERAL_POOL = 65536;
constexpr std::uint32_t MAX_CURSOR = 128;
constexpr std::uint32_t MAX_LIMIT = 500;
constexpr std::uint64_t MAX_ORDERED_WINDOW = 4096;

std::uint16_t read16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(data[0])
           | static_cast<std::uint16_t>(data[1]) << 8U;
}

std::uint32_t read32(const std::uint8_t *data)
{
    return static_cast<std::uint32_t>(data[0])
           | static_cast<std::uint32_t>(data[1]) << 8U
           | static_cast<std::uint32_t>(data[2]) << 16U
           | static_cast<std::uint32_t>(data[3]) << 24U;
}

std::uint64_t read64(const std::uint8_t *data)
{
    return static_cast<std::uint64_t>(read32(data))
           | static_cast<std::uint64_t>(read32(data + 4)) << 32U;
}

bool checkedSection(
        std::uint64_t &total, std::uint64_t count, std::uint64_t width)
{
    if (count != 0
        && width > std::numeric_limits<std::uint64_t>::max() / count)
    {
        return false;
    }
    const std::uint64_t bytes = count * width;
    if (total > std::numeric_limits<std::uint64_t>::max() - bytes)
    {
        return false;
    }
    total += bytes;
    return true;
}

bool validCursorText(const std::string &cursor)
{
    return std::all_of(
            cursor.begin(), cursor.end(), [](unsigned char value)
            {
                return (value >= 'A' && value <= 'Z')
                       || (value >= 'a' && value <= 'z')
                       || (value >= '0' && value <= '9')
                       || value == '-' || value == '_';
            });
}

} // namespace

bool parseScanPlan(
        const format::ByteSpan &packet, ScanPlan &plan,
        format::FormatError &error)
{
    plan = ScanPlan();
    error.clear();
    if (packet.size() == 0 || packet.size() > MAX_PACKET_BYTES)
    {
        return format::fail(error, format::ErrorCode::OUT_OF_BOUNDS,
                    "scan packet exceeds the bounded byte limit");
    }
    if (packet.size() < HEADER_BYTES)
    {
        return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                    "scan packet header is truncated");
    }
    const std::uint8_t *data = packet.data();
    if (data[0] != 'P' || data[1] != 'X'
        || data[2] != 'S' || data[3] != 'V')
    {
        return format::fail(error, format::ErrorCode::INVALID_MAGIC,
                    "scan packet magic is invalid");
    }
    if (read16(data + 4) != 1)
    {
        return format::fail(error, format::ErrorCode::UNSUPPORTED_VERSION,
                    "scan packet version is unsupported");
    }
    if (read16(data + 6) != HEADER_BYTES
        || read32(data + 8) != packet.size())
    {
        return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                    "scan packet size is not canonical");
    }
    const std::uint32_t flags = read32(data + 12);
    const std::uint16_t projectionCount = read16(data + 16);
    const std::uint16_t nodeCount = read16(data + 18);
    const std::uint16_t orderCount = read16(data + 20);
    const std::uint32_t literalBytes = read32(data + 24);
    const std::uint16_t cursorBytes = read16(data + 28);
    if ((flags & ~1U) != 0 || read16(data + 22) != 0
        || read16(data + 30) != 0 || read32(data + 44) != 0)
    {
        return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                    "scan packet reserved fields must be zero");
    }
    plan.projectionAll = (flags & 1U) != 0;
    if (projectionCount > MAX_PROJECTION
        || (plan.projectionAll && projectionCount != 0)
        || (!plan.projectionAll && projectionCount == 0)
        || nodeCount == 0 || nodeCount > MAX_NODES
        || orderCount > MAX_ORDER
        || literalBytes > MAX_LITERAL_POOL
        || cursorBytes > MAX_CURSOR)
    {
        return format::fail(error, format::ErrorCode::OUT_OF_BOUNDS,
                    "scan packet section count is outside its bound");
    }
    std::uint64_t expected = HEADER_BYTES;
    if (!checkedSection(expected, projectionCount, 4)
        || !checkedSection(expected, nodeCount, NODE_BYTES)
        || !checkedSection(expected, orderCount, ORDER_BYTES)
        || !checkedSection(expected, literalBytes, 1)
        || !checkedSection(expected, cursorBytes, 1)
        || expected != packet.size())
    {
        return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                    "scan packet sections do not consume the packet exactly");
    }
    plan.offset = read64(data + 32);
    plan.limit = read32(data + 40);
    if (plan.limit == 0)
    {
        plan.limit = 20;
    }
    if (plan.limit > MAX_LIMIT)
    {
        return format::fail(error, format::ErrorCode::OUT_OF_BOUNDS,
                    "scan limit exceeds 500 rows");
    }
    if (orderCount != 0
        && (plan.offset > MAX_ORDERED_WINDOW
            || plan.limit > MAX_ORDERED_WINDOW - plan.offset))
    {
        return format::fail(error, format::ErrorCode::OUT_OF_BOUNDS,
                    "ordered scan window exceeds 4096 rows");
    }
    if (cursorBytes != 0 && plan.offset != 0)
    {
        return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                    "scan cursor cannot be combined with a nonzero offset");
    }

    std::size_t position = HEADER_BYTES;
    std::set<std::uint32_t> unique;
    for (std::uint16_t index = 0; index < projectionCount; ++index)
    {
        const std::uint32_t column = read32(data + position);
        position += 4;
        if (!unique.insert(column).second)
        {
            return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                        "scan projection columns must be unique");
        }
        plan.projection.push_back(column);
    }

    const std::size_t nodeStart = position;
    const std::size_t literalStart =
            nodeStart + static_cast<std::size_t>(nodeCount) * NODE_BYTES
            + static_cast<std::size_t>(orderCount) * ORDER_BYTES;
    std::vector<std::uint8_t> depths;
    std::uint32_t leaves = 0;
    std::uint32_t expectedLiteralOffset = 0;
    for (std::uint16_t index = 0; index < nodeCount; ++index)
    {
        const std::uint8_t *node = data + position;
        position += NODE_BYTES;
        ScanExpressionNode parsed;
        parsed.kind = static_cast<ScanNodeKind>(node[0]);
        parsed.filterOperator = node[1];
        parsed.childCount = read16(node + 2);
        parsed.column = read32(node + 4);
        const std::uint32_t literalOffset = read32(node + 8);
        const std::uint32_t literalSize = read32(node + 12);
        if (read32(node + 16) != 0
            || static_cast<std::uint8_t>(parsed.kind) > 4)
        {
            return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                        "scan expression node is invalid");
        }
        if (parsed.kind == ScanNodeKind::TRUE_VALUE
            || parsed.kind == ScanNodeKind::PREDICATE)
        {
            if (parsed.childCount != 0)
            {
                return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                            "scan expression leaf has children");
            }
            if (parsed.kind == ScanNodeKind::TRUE_VALUE)
            {
                if (parsed.filterOperator != 0 || parsed.column != 0
                    || literalOffset != 0 || literalSize != 0)
                {
                    return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                                "scan TRUE node is not canonical");
                }
            }
            else
            {
                ++leaves;
                if (leaves > MAX_LEAVES
                    || parsed.filterOperator > 8
                    || literalSize > MAX_LITERAL
                    || literalOffset != expectedLiteralOffset
                    || literalOffset > literalBytes
                    || literalSize > literalBytes - literalOffset)
                {
                    return format::fail(error, format::ErrorCode::OUT_OF_BOUNDS,
                                "scan predicate exceeds its bound");
                }
                const bool nullOperator =
                        parsed.filterOperator == 6
                        || parsed.filterOperator == 7;
                if (nullOperator && literalSize != 0)
                {
                    return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                                "scan predicate literal is not canonical");
                }
                parsed.literal.assign(
                        reinterpret_cast<const char *>(
                                data + literalStart + literalOffset),
                        literalSize);
                expectedLiteralOffset += literalSize;
                if (!format::VariableLengthDecoder::isValidUtf8(
                            format::ByteSpan(
                                    reinterpret_cast<const std::uint8_t *>(
                                            parsed.literal.data()),
                                    parsed.literal.size())))
                {
                    return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                                "scan predicate literal is not UTF-8");
                }
            }
            depths.push_back(1);
        }
        else
        {
            if (parsed.filterOperator != 0 || parsed.column != 0
                || literalOffset != 0 || literalSize != 0)
            {
                return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                            "scan expression branch is not canonical");
            }
            const std::uint16_t required =
                    parsed.kind == ScanNodeKind::NOT ? 1
                    : parsed.childCount;
            if ((parsed.kind == ScanNodeKind::NOT
                 && parsed.childCount != 1)
                || (parsed.kind != ScanNodeKind::NOT
                    && (parsed.childCount < 2
                        || parsed.childCount > 8))
                || depths.size() < required)
            {
                return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                            "scan expression postfix stack is invalid");
            }
            std::uint8_t depth = 0;
            for (std::uint16_t child = 0; child < required; ++child)
            {
                depth = std::max(depth, depths.back());
                depths.pop_back();
            }
            if (depth >= 16)
            {
                return format::fail(error, format::ErrorCode::OUT_OF_BOUNDS,
                            "scan expression depth exceeds 16");
            }
            parsed.depth = static_cast<std::uint8_t>(depth + 1);
            depths.push_back(parsed.depth);
        }
        plan.expression.push_back(parsed);
    }
    if (depths.size() != 1)
    {
        return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                    "scan expression must have exactly one root");
    }
    if (expectedLiteralOffset != literalBytes)
    {
        return format::fail(
                error, format::ErrorCode::INVALID_ARGUMENT,
                "scan literal pool is not canonical");
    }

    unique.clear();
    for (std::uint16_t index = 0; index < orderCount; ++index)
    {
        const std::uint8_t *key = data + position;
        position += ORDER_BYTES;
        ScanOrderKey parsed;
        parsed.column = read32(key);
        parsed.descending = key[4] != 0;
        parsed.nullsLast = key[5] != 0;
        if (key[4] > 1 || key[5] > 1
            || read16(key + 6) != 0
            || !unique.insert(parsed.column).second)
        {
            return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                        "scan order key is invalid or duplicated");
        }
        plan.order.push_back(parsed);
    }
    position += literalBytes;
    if (cursorBytes != 0)
    {
        plan.cursor.assign(
                reinterpret_cast<const char *>(data + position),
                cursorBytes);
        if (!validCursorText(plan.cursor))
        {
            return format::fail(error, format::ErrorCode::INVALID_ARGUMENT,
                        "scan cursor is not canonical base64url");
        }
    }
    return true;
}

} // namespace inspector
} // namespace pixels
