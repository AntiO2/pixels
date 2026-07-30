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

#ifndef PIXELS_FORMAT_PLAINPIXELPLANNER_H
#define PIXELS_FORMAT_PLAINPIXELPLANNER_H

#include "format/ByteSpan.h"
#include "format/FormatError.h"

#include <cstddef>
#include <cstdint>

namespace pixels
{
namespace format
{

struct PlainPixelPlan
{
    std::uint32_t physicalOffset = 0;
    std::uint32_t physicalCount = 0;
};

class PlainPixelPlanner
{
public:
    [[nodiscard]] static bool plan(
            std::uint32_t pixelRows, std::uint32_t rowOffset,
            std::uint32_t rowCount, bool hasNull, bool nullsPadding,
            bool littleEndian, const ByteSpan &nullBitmap,
            bool *validity, std::size_t validitySize,
            PlainPixelPlan &plan, FormatError &error);
};

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_PLAINPIXELPLANNER_H
