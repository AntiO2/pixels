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

#include "format/PlainScalarDecoder.h"

namespace pixels
{
namespace format
{

bool PlainLongDecoder::decode(
        const ByteSpan &bytes, bool littleEndian, std::uint64_t valueOffset,
        std::size_t valueCount, std::int64_t *destination,
        std::size_t destinationSize, FormatError &error)
{
    return PlainScalarDecoder::decodeInt64(
            bytes, littleEndian, valueOffset, valueCount,
            destination, destinationSize, error);
}

} // namespace format
} // namespace pixels
