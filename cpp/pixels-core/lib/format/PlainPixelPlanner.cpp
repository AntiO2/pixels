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

#include "format/PlainPixelPlanner.h"

#include "format/ByteReader.h"

namespace pixels
{
namespace format
{

namespace
{

bool isNullAt(
        const ByteSpan &bitmap, std::uint32_t row,
        bool littleEndian) noexcept
{
    const std::uint8_t packed = bitmap.data()[row / 8U];
    const std::uint32_t withinByte = row % 8U;
    const std::uint32_t shift =
            littleEndian ? withinByte : 7U - withinByte;
    return ((packed >> shift) & 1U) != 0;
}

} // namespace

bool PlainPixelPlanner::plan(
        std::uint32_t pixelRows, std::uint32_t rowOffset,
        std::uint32_t rowCount, bool hasNull, bool nullsPadding,
        bool littleEndian, const ByteSpan &nullBitmap,
        bool *validity, std::size_t validitySize,
        PlainPixelPlan &plan, FormatError &error)
{
    error.clear();
    plan = PlainPixelPlan{};
    if (pixelRows == 0 || rowCount == 0)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "pixel and page row counts must be positive");
    }
    std::uint64_t rowEnd = 0;
    if (!checkedAdd(rowOffset, rowCount, rowEnd)
        || rowEnd > pixelRows)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "page rows exceed the pixel");
    }
    if (validity == nullptr)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "pixel validity destination is null");
    }
    if (rowCount > validitySize)
    {
        return fail(error, ErrorCode::BUFFER_TOO_SMALL,
                    "pixel validity destination is too small");
    }
    if (hasNull)
    {
        const std::uint64_t requiredBytes =
                pixelRows / 8U + (pixelRows % 8U == 0 ? 0U : 1U);
        if (!nullBitmap.isValid()
            || nullBitmap.size() != requiredBytes)
        {
            return fail(error, ErrorCode::INVALID_ARGUMENT,
                        "pixel null bitmap has an invalid size");
        }
        bool observedNull = false;
        for (std::uint32_t row = 0; row < pixelRows; ++row)
        {
            observedNull = observedNull
                           || isNullAt(
                                   nullBitmap, row, littleEndian);
        }
        if (!observedNull)
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "pixel statistics declare nulls but bitmap has none");
        }
    }

    std::uint32_t nonNullBefore = 0;
    if (hasNull && !nullsPadding)
    {
        for (std::uint32_t row = 0; row < rowOffset; ++row)
        {
            if (!isNullAt(nullBitmap, row, littleEndian))
            {
                ++nonNullBefore;
            }
        }
    }
    plan.physicalOffset =
            hasNull && !nullsPadding ? nonNullBefore : rowOffset;
    plan.physicalCount =
            hasNull && !nullsPadding ? 0 : rowCount;

    for (std::uint32_t index = 0; index < rowCount; ++index)
    {
        const bool valid =
                !hasNull
                || !isNullAt(nullBitmap, rowOffset + index, littleEndian);
        validity[index] = valid;
        if (hasNull && !nullsPadding && valid)
        {
            ++plan.physicalCount;
        }
    }
    return true;
}

} // namespace format
} // namespace pixels
