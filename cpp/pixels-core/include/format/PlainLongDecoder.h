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

#ifndef PIXELS_FORMAT_PLAINLONGDECODER_H
#define PIXELS_FORMAT_PLAINLONGDECODER_H

#include "format/ByteSpan.h"
#include "format/FormatError.h"

#include <cstddef>
#include <cstdint>

namespace pixels
{
namespace format
{

class PlainLongDecoder
{
public:
    [[nodiscard]] static bool decode(
            const ByteSpan &bytes, bool littleEndian, std::uint64_t valueOffset,
            std::size_t valueCount, std::int64_t *destination,
            std::size_t destinationSize, FormatError &error);
};

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_PLAINLONGDECODER_H
