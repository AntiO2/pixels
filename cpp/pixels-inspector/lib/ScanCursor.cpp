/*
 * Copyright 2026 PixelsDB.
 */

#include "ScanCursor.h"

#include <algorithm>
#include <array>
#include <vector>

namespace pixels
{
namespace inspector
{
namespace
{

std::uint64_t crc64(
        const std::uint8_t *bytes, std::size_t size,
        std::uint64_t crc = 0)
{
    constexpr std::uint64_t POLYNOMIAL = 0x42F0E1EBA9EA3693ULL;
    for (std::size_t index = 0; index < size; ++index)
    {
        crc ^= static_cast<std::uint64_t>(bytes[index]) << 56U;
        for (std::uint32_t bit = 0; bit < 8; ++bit)
        {
            crc = (crc & (1ULL << 63U)) != 0
                  ? (crc << 1U) ^ POLYNOMIAL : crc << 1U;
        }
    }
    return crc;
}

void append32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (std::uint32_t byte = 0; byte < 4; ++byte)
    {
        bytes.push_back(static_cast<std::uint8_t>(
                value >> (byte * 8U)));
    }
}

void append64(std::vector<std::uint8_t> &bytes, std::uint64_t value)
{
    for (std::uint32_t byte = 0; byte < 8; ++byte)
    {
        bytes.push_back(static_cast<std::uint8_t>(
                value >> (byte * 8U)));
    }
}

void write64(
        std::array<std::uint8_t, 40> &bytes, std::size_t offset,
        std::uint64_t value)
{
    for (std::uint32_t byte = 0; byte < 8; ++byte)
    {
        bytes[offset + byte] = static_cast<std::uint8_t>(
                value >> (byte * 8U));
    }
}

std::uint64_t read64(const std::uint8_t *bytes)
{
    std::uint64_t value = 0;
    for (std::uint32_t byte = 0; byte < 8; ++byte)
    {
        value |= static_cast<std::uint64_t>(bytes[byte])
                 << (byte * 8U);
    }
    return value;
}

std::string base64Url(const std::uint8_t *bytes, std::size_t size)
{
    static const char ALPHABET[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789-_";
    std::string output;
    output.reserve((size * 4U + 2U) / 3U);
    std::size_t index = 0;
    while (index + 3 <= size)
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes[index]) << 16U
                | static_cast<std::uint32_t>(bytes[index + 1]) << 8U
                | bytes[index + 2];
        output.push_back(ALPHABET[(value >> 18U) & 63U]);
        output.push_back(ALPHABET[(value >> 12U) & 63U]);
        output.push_back(ALPHABET[(value >> 6U) & 63U]);
        output.push_back(ALPHABET[value & 63U]);
        index += 3;
    }
    if (size - index == 1)
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes[index]) << 16U;
        output.push_back(ALPHABET[(value >> 18U) & 63U]);
        output.push_back(ALPHABET[(value >> 12U) & 63U]);
    }
    else if (size - index == 2)
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes[index]) << 16U
                | static_cast<std::uint32_t>(bytes[index + 1]) << 8U;
        output.push_back(ALPHABET[(value >> 18U) & 63U]);
        output.push_back(ALPHABET[(value >> 12U) & 63U]);
        output.push_back(ALPHABET[(value >> 6U) & 63U]);
    }
    return output;
}

int decodeDigit(char value)
{
    if (value >= 'A' && value <= 'Z')
    {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z')
    {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9')
    {
        return value - '0' + 52;
    }
    return value == '-' ? 62 : value == '_' ? 63 : -1;
}

bool decodeBase64Url(
        const std::string &encoded,
        std::array<std::uint8_t, 40> &bytes)
{
    if (encoded.size() != 54)
    {
        return false;
    }
    std::vector<std::uint8_t> output;
    output.reserve(40);
    std::uint32_t accumulator = 0;
    std::uint32_t bits = 0;
    for (char character : encoded)
    {
        const int digit = decodeDigit(character);
        if (digit < 0)
        {
            return false;
        }
        accumulator = (accumulator << 6U)
                      | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            output.push_back(static_cast<std::uint8_t>(
                    accumulator >> bits));
            accumulator &= (1U << bits) - 1U;
        }
    }
    if (output.size() != bytes.size() || accumulator != 0)
    {
        return false;
    }
    std::copy(output.begin(), output.end(), bytes.begin());
    return true;
}

} // namespace

std::uint64_t scanPlanFingerprint(const ScanPlan &plan)
{
    std::vector<std::uint8_t> bytes;
    append32(bytes, static_cast<std::uint32_t>(plan.projection.size()));
    for (std::uint32_t column : plan.projection)
    {
        append32(bytes, column);
    }
    append32(bytes, static_cast<std::uint32_t>(plan.expression.size()));
    for (const ScanExpressionNode &node : plan.expression)
    {
        bytes.push_back(static_cast<std::uint8_t>(node.kind));
        bytes.push_back(node.filterOperator);
        bytes.push_back(static_cast<std::uint8_t>(node.childCount));
        append32(bytes, node.column);
        append32(bytes, static_cast<std::uint32_t>(node.literal.size()));
        bytes.insert(bytes.end(), node.literal.begin(), node.literal.end());
    }
    append32(bytes, static_cast<std::uint32_t>(plan.order.size()));
    for (const ScanOrderKey &key : plan.order)
    {
        append32(bytes, key.column);
        bytes.push_back(key.descending ? 1 : 0);
        bytes.push_back(key.nullsLast ? 1 : 0);
    }
    return crc64(bytes.data(), bytes.size());
}

std::uint64_t scanSourceSignature(
        std::uint64_t fileSize, const std::string &sourceIdentity)
{
    std::vector<std::uint8_t> bytes;
    append64(bytes, fileSize);
    bytes.insert(
            bytes.end(), sourceIdentity.begin(), sourceIdentity.end());
    return crc64(bytes.data(), bytes.size());
}

std::string encodeScanCursor(const ScanCursor &cursor)
{
    std::array<std::uint8_t, 40> bytes{};
    bytes[0] = 'P';
    bytes[1] = 'X';
    bytes[2] = 'C';
    bytes[3] = '2';
    bytes[4] = 1;
    bytes[6] = cursor.ordered ? 1 : 0;
    write64(bytes, 8, cursor.planFingerprint);
    write64(bytes, 16, cursor.sourceSignature);
    write64(bytes, 24, cursor.anchorAbsoluteRow);
    return base64Url(bytes.data(), bytes.size());
}

bool decodeScanCursor(
        const std::string &encoded, ScanCursor &cursor,
        format::FormatError &error)
{
    std::array<std::uint8_t, 40> bytes{};
    if (!decodeBase64Url(encoded, bytes)
        || bytes[0] != 'P' || bytes[1] != 'X'
        || bytes[2] != 'C' || bytes[3] != '2'
        || bytes[4] != 1 || bytes[5] != 0
        || bytes[6] > 1 || bytes[7] != 0
        || read64(bytes.data() + 32) != 0)
    {
        return format::fail(
                error, format::ErrorCode::INVALID_ARGUMENT,
                "scan cursor is malformed");
    }
    cursor.ordered = bytes[6] == 1;
    cursor.planFingerprint = read64(bytes.data() + 8);
    cursor.sourceSignature = read64(bytes.data() + 16);
    cursor.anchorAbsoluteRow = read64(bytes.data() + 24);
    return true;
}

} // namespace inspector
} // namespace pixels
