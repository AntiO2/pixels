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

#ifndef PIXELS_FORMAT_VARIABLELENGTHDECODER_H
#define PIXELS_FORMAT_VARIABLELENGTHDECODER_H

#include "format/ByteSpan.h"
#include "format/FormatError.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pixels
{
namespace format
{

struct PlainVariableLayout
{
    std::uint64_t contentLength = 0;
    std::uint64_t startsOffset = 0;
    std::uint64_t startsLength = 0;
    std::uint64_t physicalValueCount = 0;
};

struct DictionaryVariableLayout
{
    std::uint64_t idsLength = 0;
    std::uint64_t dictionaryContentOffset = 0;
    std::uint64_t dictionaryContentLength = 0;
    std::uint64_t dictionaryStartsOffset = 0;
    std::uint64_t dictionaryStartsLength = 0;
};

struct VariableValueRange
{
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

class VariableLengthDecoder
{
public:
    [[nodiscard]] static bool parsePlainLayout(
            const ByteSpan &trailer, bool littleEndian,
            std::uint64_t encodedDataLength,
            PlainVariableLayout &layout, FormatError &error);

    [[nodiscard]] static bool decodeStartsWindow(
            const ByteSpan &bytes, bool littleEndian,
            std::size_t valueCount, std::uint64_t contentLimit,
            std::vector<VariableValueRange> &ranges,
            FormatError &error);

    [[nodiscard]] static bool parseDictionaryLayout(
            const ByteSpan &trailer, bool littleEndian,
            std::uint64_t encodedDataLength,
            DictionaryVariableLayout &layout, FormatError &error);

    [[nodiscard]] static bool decodeLengthPrefixed(
            const ByteSpan &bytes, std::size_t valueCount,
            std::uint32_t maximumLength,
            std::vector<VariableValueRange> &ranges,
            FormatError &error);

    [[nodiscard]] static bool isValidUtf8(
            const ByteSpan &bytes) noexcept;
};

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_VARIABLELENGTHDECODER_H
