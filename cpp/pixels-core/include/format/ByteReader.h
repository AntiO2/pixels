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

#ifndef PIXELS_FORMAT_BYTEREADER_H
#define PIXELS_FORMAT_BYTEREADER_H

#include "format/ByteSpan.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace pixels
{
namespace format
{

struct FileRange
{
    std::uint64_t offset = 0;
    std::uint64_t length = 0;

    [[nodiscard]] bool operator==(const FileRange &other) const noexcept
    {
        return offset == other.offset && length == other.length;
    }
};

inline bool checkedAdd(std::uint64_t left, std::uint64_t right,
                       std::uint64_t &result) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
    {
        return false;
    }
    result = left + right;
    return true;
}

inline bool isRangeWithinFile(const FileRange &range,
                              std::uint64_t fileSize) noexcept
{
    std::uint64_t end = 0;
    return checkedAdd(range.offset, range.length, end) && end <= fileSize;
}

inline bool readUnsigned64(const ByteSpan &bytes, std::size_t offset,
                           bool littleEndian, std::uint64_t &value) noexcept
{
    ByteSpan source;
    if (!bytes.subspan(offset, sizeof(std::uint64_t), source))
    {
        return false;
    }

    value = 0;
    if (littleEndian)
    {
        for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index)
        {
            value |= static_cast<std::uint64_t>(source.data()[index])
                     << (index * 8U);
        }
    }
    else
    {
        for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index)
        {
            value = (value << 8U) | source.data()[index];
        }
    }
    return true;
}

inline bool readSigned64(const ByteSpan &bytes, std::size_t offset,
                         bool littleEndian, std::int64_t &value) noexcept
{
    std::uint64_t unsignedValue = 0;
    if (!readUnsigned64(bytes, offset, littleEndian, unsignedValue))
    {
        return false;
    }
    static_assert(sizeof(value) == sizeof(unsignedValue),
                  "64-bit integer widths must match");
    std::memcpy(&value, &unsignedValue, sizeof(value));
    return true;
}

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_BYTEREADER_H
