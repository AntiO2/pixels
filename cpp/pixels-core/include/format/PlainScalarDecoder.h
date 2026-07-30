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

#ifndef PIXELS_FORMAT_PLAINSCALARDECODER_H
#define PIXELS_FORMAT_PLAINSCALARDECODER_H

#include "format/ByteSpan.h"
#include "format/FormatError.h"

#include <cstddef>
#include <cstdint>

namespace pixels
{
namespace format
{

struct Int128Words
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;
};

class PlainScalarDecoder
{
public:
    [[nodiscard]] static bool decodeBoolean(
            const ByteSpan &bytes, bool littleEndian,
            std::uint64_t valueOffset, std::size_t valueCount,
            bool *destination, std::size_t destinationSize,
            FormatError &error);

    [[nodiscard]] static bool decodeByte(
            const ByteSpan &bytes, std::uint64_t valueOffset,
            std::size_t valueCount, std::int8_t *destination,
            std::size_t destinationSize, FormatError &error);

    [[nodiscard]] static bool decodeInt32(
            const ByteSpan &bytes, bool littleEndian,
            std::uint64_t valueOffset, std::size_t valueCount,
            std::int32_t *destination, std::size_t destinationSize,
            FormatError &error);

    [[nodiscard]] static bool decodeInt64(
            const ByteSpan &bytes, bool littleEndian,
            std::uint64_t valueOffset, std::size_t valueCount,
            std::int64_t *destination, std::size_t destinationSize,
            FormatError &error);

    [[nodiscard]] static bool decodeFloat(
            const ByteSpan &bytes, bool littleEndian,
            std::uint64_t valueOffset, std::size_t valueCount,
            float *destination, std::size_t destinationSize,
            FormatError &error);

    [[nodiscard]] static bool decodeDouble(
            const ByteSpan &bytes, bool littleEndian,
            std::uint64_t valueOffset, std::size_t valueCount,
            double *destination, std::size_t destinationSize,
            FormatError &error);

    [[nodiscard]] static bool decodeInt128(
            const ByteSpan &bytes, bool littleEndian,
            std::uint64_t valueOffset, std::size_t valueCount,
            Int128Words *destination, std::size_t destinationSize,
            FormatError &error);
};

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_PLAINSCALARDECODER_H
