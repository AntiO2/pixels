/*
 * Copyright 2026 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#include "format/PlainScalarDecoder.h"

#include "format/ByteReader.h"

#include <cstring>
#include <limits>
#include <string>

namespace pixels
{
namespace format
{

namespace
{

bool validateDestination(
        const ByteSpan &bytes, std::size_t valueCount,
        const void *destination, std::size_t destinationSize,
        const std::string &kind, FormatError &error)
{
    error.clear();
    if (!bytes.isValid())
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "plain " + kind + " input span is invalid");
    }
    if (valueCount > 0 && destination == nullptr)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "plain " + kind + " destination is null");
    }
    if (valueCount > destinationSize)
    {
        return fail(error, ErrorCode::BUFFER_TOO_SMALL,
                    "plain " + kind + " destination is too small");
    }
    return true;
}

bool validateByteRange(
        const ByteSpan &bytes, std::uint64_t valueOffset,
        std::size_t valueCount, std::uint64_t width,
        const std::string &kind, std::uint64_t &byteOffset,
        FormatError &error)
{
    if (valueOffset > std::numeric_limits<std::uint64_t>::max() / width)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain " + kind + " element range overflows");
    }
    byteOffset = valueOffset * width;
    if (valueCount
        > std::numeric_limits<std::uint64_t>::max() / width)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain " + kind + " element range overflows");
    }
    const std::uint64_t byteLength =
            static_cast<std::uint64_t>(valueCount) * width;
    std::uint64_t byteEnd = 0;
    if (!checkedAdd(byteOffset, byteLength, byteEnd)
        || byteEnd > bytes.size())
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain " + kind + " element range exceeds the input");
    }
    return true;
}

bool readUnsigned(
        const ByteSpan &bytes, std::size_t offset, std::size_t width,
        bool littleEndian, std::uint64_t &value) noexcept
{
    ByteSpan source;
    if (width == 0 || width > sizeof(value)
        || !bytes.subspan(offset, width, source))
    {
        return false;
    }
    value = 0;
    if (littleEndian)
    {
        for (std::size_t index = 0; index < width; ++index)
        {
            value |= static_cast<std::uint64_t>(source.data()[index])
                     << (index * 8U);
        }
    }
    else
    {
        for (std::size_t index = 0; index < width; ++index)
        {
            value = (value << 8U) | source.data()[index];
        }
    }
    return true;
}

template<typename Destination, typename Converter>
bool decodeFixed(
        const ByteSpan &bytes, bool littleEndian,
        std::uint64_t valueOffset, std::size_t valueCount,
        Destination *destination, std::size_t destinationSize,
        std::size_t width, const std::string &kind,
        Converter converter, FormatError &error)
{
    if (!validateDestination(
                bytes, valueCount, destination, destinationSize,
                kind, error))
    {
        return false;
    }
    std::uint64_t byteOffset = 0;
    if (!validateByteRange(
                bytes, valueOffset, valueCount, width, kind,
                byteOffset, error))
    {
        return false;
    }
    for (std::size_t index = 0; index < valueCount; ++index)
    {
        std::uint64_t bits = 0;
        if (!readUnsigned(
                    bytes,
                    static_cast<std::size_t>(byteOffset)
                    + index * width,
                    width, littleEndian, bits))
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "plain " + kind + " value is truncated");
        }
        converter(bits, destination[index]);
    }
    return true;
}

} // namespace

bool PlainScalarDecoder::decodeBoolean(
        const ByteSpan &bytes, bool littleEndian,
        std::uint64_t valueOffset, std::size_t valueCount,
        bool *destination, std::size_t destinationSize,
        FormatError &error)
{
    const std::string kind = "BOOLEAN";
    if (!validateDestination(
                bytes, valueCount, destination, destinationSize,
                kind, error))
    {
        return false;
    }
    std::uint64_t bitEnd = 0;
    if (!checkedAdd(
                valueOffset, static_cast<std::uint64_t>(valueCount),
                bitEnd)
        || bitEnd > static_cast<std::uint64_t>(bytes.size()) * 8U)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain BOOLEAN element range exceeds the input");
    }
    for (std::size_t index = 0; index < valueCount; ++index)
    {
        const std::uint64_t bit = valueOffset + index;
        const std::uint8_t packed =
                bytes.data()[static_cast<std::size_t>(bit / 8U)];
        const std::uint32_t withinByte =
                static_cast<std::uint32_t>(bit % 8U);
        const std::uint32_t shift =
                littleEndian ? withinByte : 7U - withinByte;
        destination[index] = ((packed >> shift) & 1U) != 0;
    }
    return true;
}

bool PlainScalarDecoder::decodeByte(
        const ByteSpan &bytes, std::uint64_t valueOffset,
        std::size_t valueCount, std::int8_t *destination,
        std::size_t destinationSize, FormatError &error)
{
    return decodeFixed(
            bytes, true, valueOffset, valueCount, destination,
            destinationSize, 1, "BYTE",
            [](std::uint64_t bits, std::int8_t &value)
            {
                const std::uint8_t byte = static_cast<std::uint8_t>(bits);
                std::memcpy(&value, &byte, sizeof(value));
            },
            error);
}

bool PlainScalarDecoder::decodeInt32(
        const ByteSpan &bytes, bool littleEndian,
        std::uint64_t valueOffset, std::size_t valueCount,
        std::int32_t *destination, std::size_t destinationSize,
        FormatError &error)
{
    return decodeFixed(
            bytes, littleEndian, valueOffset, valueCount, destination,
            destinationSize, 4, "INT32",
            [](std::uint64_t bits, std::int32_t &value)
            {
                const std::uint32_t word =
                        static_cast<std::uint32_t>(bits);
                std::memcpy(&value, &word, sizeof(value));
            },
            error);
}

bool PlainScalarDecoder::decodeInt64(
        const ByteSpan &bytes, bool littleEndian,
        std::uint64_t valueOffset, std::size_t valueCount,
        std::int64_t *destination, std::size_t destinationSize,
        FormatError &error)
{
    return decodeFixed(
            bytes, littleEndian, valueOffset, valueCount, destination,
            destinationSize, 8, "INT64",
            [](std::uint64_t bits, std::int64_t &value)
            {
                std::memcpy(&value, &bits, sizeof(value));
            },
            error);
}

bool PlainScalarDecoder::decodeFloat(
        const ByteSpan &bytes, bool littleEndian,
        std::uint64_t valueOffset, std::size_t valueCount,
        float *destination, std::size_t destinationSize,
        FormatError &error)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t),
                  "Pixels FLOAT requires IEEE-754 binary32");
    return decodeFixed(
            bytes, littleEndian, valueOffset, valueCount, destination,
            destinationSize, 4, "FLOAT",
            [](std::uint64_t bits, float &value)
            {
                const std::uint32_t word =
                        static_cast<std::uint32_t>(bits);
                std::memcpy(&value, &word, sizeof(value));
            },
            error);
}

bool PlainScalarDecoder::decodeDouble(
        const ByteSpan &bytes, bool littleEndian,
        std::uint64_t valueOffset, std::size_t valueCount,
        double *destination, std::size_t destinationSize,
        FormatError &error)
{
    static_assert(sizeof(double) == sizeof(std::uint64_t),
                  "Pixels DOUBLE requires IEEE-754 binary64");
    return decodeFixed(
            bytes, littleEndian, valueOffset, valueCount, destination,
            destinationSize, 8, "DOUBLE",
            [](std::uint64_t bits, double &value)
            {
                std::memcpy(&value, &bits, sizeof(value));
            },
            error);
}

bool PlainScalarDecoder::decodeInt128(
        const ByteSpan &bytes, bool littleEndian,
        std::uint64_t valueOffset, std::size_t valueCount,
        Int128Words *destination, std::size_t destinationSize,
        FormatError &error)
{
    const std::string kind = "INT128";
    if (!validateDestination(
                bytes, valueCount, destination, destinationSize,
                kind, error))
    {
        return false;
    }
    std::uint64_t byteOffset = 0;
    if (!validateByteRange(
                bytes, valueOffset, valueCount, 16, kind,
                byteOffset, error))
    {
        return false;
    }
    for (std::size_t index = 0; index < valueCount; ++index)
    {
        const std::size_t source =
                static_cast<std::size_t>(byteOffset) + index * 16U;
        if (!readUnsigned(
                    bytes, source, 8, littleEndian,
                    destination[index].high)
            || !readUnsigned(
                    bytes, source + 8U, 8, littleEndian,
                    destination[index].low))
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "plain INT128 value is truncated");
        }
    }
    return true;
}

} // namespace format
} // namespace pixels
