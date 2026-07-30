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

#include "format/VariableLengthDecoder.h"

#include "format/ByteReader.h"

#include <limits>
#include <string>

namespace pixels
{
namespace format
{

namespace
{

bool readUnsigned32(
        const ByteSpan &bytes, std::size_t offset,
        bool littleEndian, std::uint32_t &value) noexcept
{
    ByteSpan word;
    if (!bytes.subspan(offset, 4, word))
    {
        return false;
    }
    value = 0;
    if (littleEndian)
    {
        for (std::size_t index = 0; index < 4; ++index)
        {
            value |= static_cast<std::uint32_t>(word.data()[index])
                     << (index * 8U);
        }
    }
    else
    {
        for (std::size_t index = 0; index < 4; ++index)
        {
            value = (value << 8U) | word.data()[index];
        }
    }
    return true;
}

bool isContinuation(std::uint8_t value) noexcept
{
    return (value & 0xC0U) == 0x80U;
}

} // namespace

bool VariableLengthDecoder::parsePlainLayout(
        const ByteSpan &trailer, bool littleEndian,
        std::uint64_t encodedDataLength,
        PlainVariableLayout &layout, FormatError &error)
{
    error.clear();
    layout = PlainVariableLayout{};
    if (!trailer.isValid() || trailer.size() != 4)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "plain variable trailer must contain four bytes");
    }
    if (encodedDataLength < 8)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain variable data is shorter than its layout");
    }
    std::uint32_t startsOffset = 0;
    if (!readUnsigned32(trailer, 0, littleEndian, startsOffset))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain variable starts offset is truncated");
    }
    const std::uint64_t startsEnd = encodedDataLength - 4U;
    if (startsOffset > startsEnd)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain variable starts offset exceeds column data");
    }
    const std::uint64_t startsLength = startsEnd - startsOffset;
    if (startsLength < 4 || startsLength % 4U != 0)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "plain variable starts field has an invalid length");
    }
    layout.contentLength = startsOffset;
    layout.startsOffset = startsOffset;
    layout.startsLength = startsLength;
    layout.physicalValueCount = startsLength / 4U - 1U;
    return true;
}

bool VariableLengthDecoder::decodeStartsWindow(
        const ByteSpan &bytes, bool littleEndian,
        std::size_t valueCount, std::uint64_t contentLimit,
        std::vector<VariableValueRange> &ranges,
        FormatError &error)
{
    error.clear();
    ranges.clear();
    if (!bytes.isValid())
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "plain variable starts span is invalid");
    }
    if (valueCount
        > (std::numeric_limits<std::size_t>::max() / 4U) - 1U)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain variable starts count overflows");
    }
    const std::size_t expectedBytes = (valueCount + 1U) * 4U;
    if (bytes.size() != expectedBytes)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "plain variable starts window has an invalid size");
    }
    std::vector<std::uint32_t> starts(valueCount + 1U);
    for (std::size_t index = 0; index < starts.size(); ++index)
    {
        if (!readUnsigned32(
                    bytes, index * 4U, littleEndian, starts[index]))
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "plain variable starts window is truncated");
        }
        if (starts[index] > contentLimit
            || (index != 0 && starts[index] < starts[index - 1U]))
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "plain variable starts are unordered or out of bounds");
        }
    }
    ranges.resize(valueCount);
    for (std::size_t index = 0; index < valueCount; ++index)
    {
        ranges[index].offset = starts[index];
        ranges[index].length =
                static_cast<std::uint64_t>(starts[index + 1U])
                - starts[index];
    }
    return true;
}

bool VariableLengthDecoder::parseDictionaryLayout(
        const ByteSpan &trailer, bool littleEndian,
        std::uint64_t encodedDataLength,
        DictionaryVariableLayout &layout, FormatError &error)
{
    error.clear();
    layout = DictionaryVariableLayout{};
    if (!trailer.isValid() || trailer.size() != 8)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "dictionary trailer must contain eight bytes");
    }
    if (encodedDataLength < 8)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "dictionary data is shorter than its trailer");
    }
    std::uint32_t contentOffset = 0;
    std::uint32_t startsOffset = 0;
    if (!readUnsigned32(
                trailer, 0, littleEndian, contentOffset)
        || !readUnsigned32(
                trailer, 4, littleEndian, startsOffset))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "dictionary offsets are truncated");
    }
    const std::uint64_t startsEnd = encodedDataLength - 8U;
    if (contentOffset > startsOffset || startsOffset > startsEnd)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "dictionary offsets are unordered or out of bounds");
    }
    layout.idsLength = contentOffset;
    layout.dictionaryContentOffset = contentOffset;
    layout.dictionaryContentLength =
            static_cast<std::uint64_t>(startsOffset) - contentOffset;
    layout.dictionaryStartsOffset = startsOffset;
    layout.dictionaryStartsLength = startsEnd - startsOffset;
    return true;
}

bool VariableLengthDecoder::decodeLengthPrefixed(
        const ByteSpan &bytes, std::size_t valueCount,
        std::uint32_t maximumLength,
        std::vector<VariableValueRange> &ranges,
        FormatError &error)
{
    error.clear();
    ranges.clear();
    if (!bytes.isValid())
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "length-prefixed binary span is invalid");
    }
    if (maximumLength == 0)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "binary maximum length must be positive");
    }
    ranges.reserve(valueCount);
    std::size_t offset = 0;
    for (std::size_t index = 0; index < valueCount; ++index)
    {
        if (offset >= bytes.size())
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "binary length prefix is truncated");
        }
        const std::uint32_t length = bytes.data()[offset++];
        if (length > maximumLength || length > bytes.size() - offset)
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "binary value exceeds its schema or pixel range");
        }
        ranges.push_back(VariableValueRange{
                static_cast<std::uint64_t>(offset), length});
        offset += length;
    }
    if (offset != bytes.size())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "binary pixel has trailing bytes");
    }
    return true;
}

bool VariableLengthDecoder::isValidUtf8(
        const ByteSpan &bytes) noexcept
{
    if (!bytes.isValid())
    {
        return false;
    }
    std::size_t index = 0;
    while (index < bytes.size())
    {
        const std::uint8_t first = bytes.data()[index++];
        if (first <= 0x7FU)
        {
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU)
        {
            if (index >= bytes.size()
                || !isContinuation(bytes.data()[index++]))
            {
                return false;
            }
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU)
        {
            if (index + 1U >= bytes.size())
            {
                return false;
            }
            const std::uint8_t second = bytes.data()[index++];
            const std::uint8_t third = bytes.data()[index++];
            if (!isContinuation(second) || !isContinuation(third)
                || (first == 0xE0U && second < 0xA0U)
                || (first == 0xEDU && second >= 0xA0U))
            {
                return false;
            }
            continue;
        }
        if (first >= 0xF0U && first <= 0xF4U)
        {
            if (index + 2U >= bytes.size())
            {
                return false;
            }
            const std::uint8_t second = bytes.data()[index++];
            const std::uint8_t third = bytes.data()[index++];
            const std::uint8_t fourth = bytes.data()[index++];
            if (!isContinuation(second)
                || !isContinuation(third)
                || !isContinuation(fourth)
                || (first == 0xF0U && second < 0x90U)
                || (first == 0xF4U && second >= 0x90U))
            {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

} // namespace format
} // namespace pixels
