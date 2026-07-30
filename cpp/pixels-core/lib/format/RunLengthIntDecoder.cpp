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

#include "format/RunLengthIntDecoder.h"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace pixels
{
namespace format
{

namespace
{

const std::size_t MAX_RUN_LENGTH = 512;

class Cursor
{
public:
    explicit Cursor(const ByteSpan &bytes) : bytes_(bytes)
    {
    }

    bool readByte(std::uint8_t &value) noexcept
    {
        if (position_ >= bytes_.size())
        {
            return false;
        }
        value = bytes_.data()[position_++];
        return true;
    }

    bool readBigEndian(
            std::size_t width, std::uint64_t &value) noexcept
    {
        if (width == 0 || width > sizeof(value)
            || width > bytes_.size() - position_)
        {
            return false;
        }
        value = 0;
        for (std::size_t index = 0; index < width; ++index)
        {
            value = (value << 8U) | bytes_.data()[position_++];
        }
        return true;
    }

    bool readVarint(std::uint64_t &value) noexcept
    {
        value = 0;
        for (std::uint32_t index = 0; index < 10; ++index)
        {
            std::uint8_t byte = 0;
            if (!readByte(byte))
            {
                return false;
            }
            if (index == 9 && (byte & 0xFEU) != 0)
            {
                return false;
            }
            value |= static_cast<std::uint64_t>(byte & 0x7FU)
                     << (index * 7U);
            if ((byte & 0x80U) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool readPacked(
            std::size_t count, std::uint32_t width,
            std::vector<std::uint64_t> &values) noexcept
    {
        if (width == 0 || width > 64
            || count > std::numeric_limits<std::size_t>::max() / width)
        {
            return false;
        }
        const std::size_t bitCount = count * width;
        const std::size_t byteCount =
                bitCount / 8U + (bitCount % 8U == 0 ? 0U : 1U);
        if (byteCount > bytes_.size() - position_)
        {
            return false;
        }
        values.assign(count, 0);
        const std::size_t start = position_;
        std::size_t bitPosition = 0;
        for (std::size_t index = 0; index < count; ++index)
        {
            std::uint64_t value = 0;
            for (std::uint32_t bit = 0; bit < width; ++bit)
            {
                const std::size_t absoluteBit = bitPosition++;
                const std::uint8_t packed =
                        bytes_.data()[start + absoluteBit / 8U];
                value = (value << 1U)
                        | ((packed >> (7U - absoluteBit % 8U)) & 1U);
            }
            values[index] = value;
        }
        position_ += byteCount;
        return true;
    }

    [[nodiscard]] std::size_t position() const noexcept
    {
        return position_;
    }

private:
    ByteSpan bytes_;
    std::size_t position_ = 0;
};

std::uint32_t decodeBitWidth(std::uint8_t encoded) noexcept
{
    if (encoded <= 23U)
    {
        return encoded + 1U;
    }
    static const std::uint32_t LARGE_WIDTHS[] = {
            26, 28, 30, 32, 40, 48, 56, 64};
    return LARGE_WIDTHS[encoded - 24U];
}

std::uint32_t closestFixedWidth(std::uint32_t width) noexcept
{
    if (width <= 24U)
    {
        return width;
    }
    static const std::uint32_t LARGE_WIDTHS[] = {
            26, 28, 30, 32, 40, 48, 56, 64};
    for (const std::uint32_t candidate : LARGE_WIDTHS)
    {
        if (width <= candidate)
        {
            return candidate;
        }
    }
    return 0;
}

std::int64_t bitsToSigned(std::uint64_t bits) noexcept
{
    std::int64_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::int64_t zigzagDecode(std::uint64_t value) noexcept
{
    const std::uint64_t bits =
            (value >> 1U)
            ^ (static_cast<std::uint64_t>(0)
               - (value & 1U));
    return bitsToSigned(bits);
}

bool checkedAddSigned(
        std::int64_t left, std::int64_t right,
        std::int64_t &result) noexcept
{
    if ((right > 0
         && left > std::numeric_limits<std::int64_t>::max() - right)
        || (right < 0
            && left < std::numeric_limits<std::int64_t>::min() - right))
    {
        return false;
    }
    result = left + right;
    return true;
}

bool checkedSubtractMagnitude(
        std::int64_t value, std::uint64_t magnitude,
        std::int64_t &result) noexcept
{
    const std::uint64_t capacity =
            value >= 0
            ? static_cast<std::uint64_t>(value)
              + (static_cast<std::uint64_t>(1) << 63U)
            : static_cast<std::uint64_t>(
                    value - std::numeric_limits<std::int64_t>::min());
    if (magnitude > capacity)
    {
        return false;
    }
    if (magnitude == (static_cast<std::uint64_t>(1) << 63U))
    {
        result = value >= 0
                 ? value + std::numeric_limits<std::int64_t>::min()
                 : 0;
        return value >= 0;
    }
    result = value - static_cast<std::int64_t>(magnitude);
    return true;
}

bool appendRun(
        const std::vector<std::int64_t> &run, std::size_t expectedCount,
        std::int64_t *destination, std::size_t &written,
        FormatError &error)
{
    if (run.empty() || run.size() > expectedCount - written)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE integer run exceeds the expected value count");
    }
    for (std::size_t index = 0; index < run.size(); ++index)
    {
        destination[written++] = run[index];
    }
    return true;
}

bool decodeShortRepeat(
        Cursor &cursor, std::uint8_t first, bool signedValues,
        std::vector<std::int64_t> &run, FormatError &error)
{
    const std::size_t width = ((first >> 3U) & 0x07U) + 1U;
    const std::size_t length = (first & 0x07U) + 3U;
    std::uint64_t encoded = 0;
    if (!cursor.readBigEndian(width, encoded))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE short-repeat value is truncated");
    }
    if (!signedValues
        && encoded
           > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max()))
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE unsigned short-repeat value exceeds int64");
    }
    const std::int64_t value =
            signedValues ? zigzagDecode(encoded)
                         : static_cast<std::int64_t>(encoded);
    run.assign(length, value);
    return true;
}

bool readRunLength(
        Cursor &cursor, std::uint8_t first, bool oneBased,
        std::size_t &length, FormatError &error)
{
    std::uint8_t second = 0;
    if (!cursor.readByte(second))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE run-length header is truncated");
    }
    length = (static_cast<std::size_t>(first & 1U) << 8U) | second;
    if (oneBased)
    {
        ++length;
    }
    if (length == 0 || length > MAX_RUN_LENGTH)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE run length is invalid");
    }
    return true;
}

bool decodeDirect(
        Cursor &cursor, std::uint8_t first, bool signedValues,
        std::vector<std::int64_t> &run, FormatError &error)
{
    std::size_t length = 0;
    if (!readRunLength(cursor, first, true, length, error))
    {
        return false;
    }
    const std::uint32_t width =
            decodeBitWidth((first >> 1U) & 0x1FU);
    std::vector<std::uint64_t> packed;
    if (!cursor.readPacked(length, width, packed))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE direct values are truncated");
    }
    run.resize(length);
    for (std::size_t index = 0; index < length; ++index)
    {
        if (!signedValues
            && packed[index]
               > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()))
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "RLE unsigned direct value exceeds int64");
        }
        run[index] =
                signedValues
                ? zigzagDecode(packed[index])
                : static_cast<std::int64_t>(packed[index]);
    }
    return true;
}

bool decodePatchedBase(
        Cursor &cursor, std::uint8_t first,
        std::vector<std::int64_t> &run, FormatError &error)
{
    std::size_t length = 0;
    if (!readRunLength(cursor, first, true, length, error))
    {
        return false;
    }
    const std::uint32_t dataWidth =
            decodeBitWidth((first >> 1U) & 0x1FU);
    std::uint8_t third = 0;
    std::uint8_t fourth = 0;
    if (!cursor.readByte(third) || !cursor.readByte(fourth))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE patched-base header is truncated");
    }
    const std::size_t baseWidth = ((third >> 5U) & 0x07U) + 1U;
    const std::uint32_t patchWidth =
            decodeBitWidth(third & 0x1FU);
    const std::uint32_t gapWidth = ((fourth >> 5U) & 0x07U) + 1U;
    const std::size_t patchLength = fourth & 0x1FU;
    if (patchLength == 0 || patchWidth + gapWidth > 64U
        || dataWidth >= 64U)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE patched-base widths or patch count are invalid");
    }

    std::uint64_t baseBits = 0;
    if (!cursor.readBigEndian(baseWidth, baseBits))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE patched-base base is truncated");
    }
    const std::uint64_t signBit =
            static_cast<std::uint64_t>(1)
            << (baseWidth * 8U - 1U);
    std::int64_t base = 0;
    if ((baseBits & signBit) != 0)
    {
        const std::uint64_t magnitude = baseBits & ~signBit;
        if (magnitude
            > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "RLE patched-base negative base overflows");
        }
        base = -static_cast<std::int64_t>(magnitude);
    }
    else
    {
        base = bitsToSigned(baseBits);
    }

    std::vector<std::uint64_t> unpacked;
    if (!cursor.readPacked(length, dataWidth, unpacked))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE patched-base data is truncated");
    }
    std::vector<std::uint64_t> patches;
    const std::uint32_t combinedWidth =
            closestFixedWidth(patchWidth + gapWidth);
    if (combinedWidth == 0)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE patched-base combined width is invalid");
    }
    if (!cursor.readPacked(patchLength, combinedWidth, patches))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE patched-base patch list is truncated");
    }

    const std::uint64_t patchMask =
            (static_cast<std::uint64_t>(1) << patchWidth) - 1U;
    std::size_t patchIndex = 0;
    std::size_t patchPosition = 0;
    bool havePatch = false;
    auto loadPatch = [&]() -> bool
    {
        std::size_t gap = 0;
        while (patchIndex < patches.size())
        {
            const std::uint64_t entry = patches[patchIndex++];
            const std::uint64_t entryGap = entry >> patchWidth;
            const std::uint64_t patch = entry & patchMask;
            if (entryGap == 255U && patch == 0)
            {
                gap += 255U;
                continue;
            }
            if (entryGap > length || gap > length - entryGap)
            {
                return false;
            }
            gap += static_cast<std::size_t>(entryGap);
            if (patchPosition > length - gap)
            {
                return false;
            }
            patchPosition += gap;
            patches[patchIndex - 1U] = patch;
            havePatch = true;
            return true;
        }
        havePatch = false;
        return true;
    };
    if (!loadPatch())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE patched-base gap exceeds the run");
    }

    run.resize(length);
    for (std::size_t index = 0; index < length; ++index)
    {
        std::uint64_t reduced = unpacked[index];
        if (havePatch && index == patchPosition)
        {
            const std::uint64_t patch = patches[patchIndex - 1U];
            if (patch
                > (std::numeric_limits<std::uint64_t>::max()
                   >> dataWidth))
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "RLE patched-base patch overflows");
            }
            reduced |= patch << dataWidth;
            if (!loadPatch())
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "RLE patched-base gap exceeds the run");
            }
        }
        if (reduced
            > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "RLE patched-base value overflows");
        }
        if (!checkedAddSigned(
                    base, static_cast<std::int64_t>(reduced),
                    run[index]))
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "RLE patched-base result overflows");
        }
    }
    if (havePatch || patchIndex != patches.size())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE patched-base contains unused patches");
    }
    return true;
}

bool decodeDelta(
        Cursor &cursor, std::uint8_t first, bool signedValues,
        std::vector<std::int64_t> &run, FormatError &error)
{
    std::size_t remaining = 0;
    if (!readRunLength(cursor, first, false, remaining, error))
    {
        return false;
    }
    const std::uint8_t encodedWidth = (first >> 1U) & 0x1FU;
    const std::uint32_t width =
            encodedWidth == 0 ? 0 : decodeBitWidth(encodedWidth);
    std::uint64_t firstEncoded = 0;
    if (!cursor.readVarint(firstEncoded))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE delta first value is truncated or invalid");
    }
    const std::int64_t firstValue =
            signedValues ? zigzagDecode(firstEncoded)
                         : (firstEncoded
                            <= static_cast<std::uint64_t>(
                                    std::numeric_limits<std::int64_t>::max())
                            ? static_cast<std::int64_t>(firstEncoded)
                            : std::numeric_limits<std::int64_t>::min());
    if (!signedValues
        && firstEncoded
           > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max()))
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE unsigned delta value exceeds int64");
    }
    run.push_back(firstValue);

    std::uint64_t deltaEncoded = 0;
    if (!cursor.readVarint(deltaEncoded))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE delta base is truncated or invalid");
    }
    const std::int64_t deltaBase = zigzagDecode(deltaEncoded);
    std::int64_t previous = firstValue;
    if (width == 0)
    {
        for (std::size_t index = 0; index < remaining; ++index)
        {
            std::int64_t value = 0;
            if (!checkedAddSigned(previous, deltaBase, value))
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "RLE fixed-delta result overflows");
            }
            run.push_back(value);
            previous = value;
        }
        return true;
    }

    if (remaining == 0)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE variable-delta run is too short");
    }
    std::int64_t second = 0;
    if (!checkedAddSigned(firstValue, deltaBase, second))
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE delta base result overflows");
    }
    run.push_back(second);
    previous = second;
    --remaining;
    std::vector<std::uint64_t> magnitudes;
    if (!cursor.readPacked(remaining, width, magnitudes))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "RLE delta magnitudes are truncated");
    }
    for (const std::uint64_t magnitude : magnitudes)
    {
        std::int64_t value = 0;
        const bool valid =
                deltaBase < 0
                ? checkedSubtractMagnitude(previous, magnitude, value)
                : (magnitude
                   <= static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())
                   && checkedAddSigned(
                           previous,
                           static_cast<std::int64_t>(magnitude),
                           value));
        if (!valid)
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "RLE variable-delta result overflows");
        }
        run.push_back(value);
        previous = value;
    }
    return true;
}

} // namespace

bool RunLengthIntDecoder::decode(
        const ByteSpan &bytes, bool signedValues,
        std::size_t expectedCount, std::int64_t *destination,
        std::size_t destinationSize, std::size_t &consumedBytes,
        FormatError &error)
{
    error.clear();
    consumedBytes = 0;
    if (!bytes.isValid())
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "RLE integer input span is invalid");
    }
    if (expectedCount == 0)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "RLE integer expected count must be positive");
    }
    if (destination == nullptr)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "RLE integer destination is null");
    }
    if (expectedCount > destinationSize)
    {
        return fail(error, ErrorCode::BUFFER_TOO_SMALL,
                    "RLE integer destination is too small");
    }

    Cursor cursor(bytes);
    std::size_t written = 0;
    while (written < expectedCount)
    {
        std::uint8_t first = 0;
        if (!cursor.readByte(first))
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "RLE integer stream ends before expected values");
        }
        std::vector<std::int64_t> run;
        bool decoded = false;
        switch (first >> 6U)
        {
            case 0:
                decoded = decodeShortRepeat(
                        cursor, first, signedValues, run, error);
                break;
            case 1:
                decoded = decodeDirect(
                        cursor, first, signedValues, run, error);
                break;
            case 2:
                decoded = decodePatchedBase(cursor, first, run, error);
                break;
            case 3:
                decoded = decodeDelta(
                        cursor, first, signedValues, run, error);
                break;
        }
        if (!decoded || !appendRun(
                    run, expectedCount, destination, written, error))
        {
            return false;
        }
    }
    consumedBytes = cursor.position();
    if (consumedBytes != bytes.size())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE integer stream has trailing bytes");
    }
    return true;
}

} // namespace format
} // namespace pixels
