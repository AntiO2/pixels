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

#include "format/PlainLongDecoder.h"

#include "format/ByteReader.h"

#include <limits>

namespace pixels
{
namespace format
{

bool PlainLongDecoder::decode(
        const ByteSpan &bytes, bool littleEndian, std::uint64_t valueOffset,
        std::size_t valueCount, std::int64_t *destination,
        std::size_t destinationSize, FormatError &error)
{
    error.clear();
    if (!bytes.isValid())
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "plain LONG input span is invalid");
    }
    if (valueCount > 0 && destination == nullptr)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "plain LONG destination is null");
    }
    if (valueCount > destinationSize)
    {
        return fail(error, ErrorCode::BUFFER_TOO_SMALL,
                    "plain LONG destination is too small");
    }
    static_assert(
            sizeof(std::size_t) <= sizeof(std::uint64_t),
            "decoder requires size_t to fit in uint64_t");
    if (valueOffset > std::numeric_limits<std::uint64_t>::max()
                      / sizeof(std::int64_t))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain LONG element range overflows");
    }

    const std::uint64_t byteOffset =
            valueOffset * sizeof(std::int64_t);
    const std::uint64_t byteLength =
            static_cast<std::uint64_t>(valueCount)
            * sizeof(std::int64_t);
    std::uint64_t byteEnd = 0;
    if (!checkedAdd(byteOffset, byteLength, byteEnd)
        || byteEnd > bytes.size())
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "plain LONG element range exceeds the input");
    }

    for (std::size_t index = 0; index < valueCount; ++index)
    {
        const std::size_t sourceOffset =
                static_cast<std::size_t>(byteOffset)
                + index * sizeof(std::int64_t);
        if (!readSigned64(bytes, sourceOffset, littleEndian,
                          destination[index]))
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "plain LONG value is truncated");
        }
    }
    return true;
}

} // namespace format
} // namespace pixels
