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

#ifndef PIXELS_FORMAT_PIXELSFORMATREADER_H
#define PIXELS_FORMAT_PIXELSFORMATREADER_H

#include "format/ByteReader.h"
#include "format/FormatError.h"
#include "pixels.pb.h"

#include <cstdint>

namespace pixels
{
namespace format
{

class PixelsFormatReader
{
public:
    static constexpr std::uint32_t SUPPORTED_VERSION = 1;
    static constexpr std::uint64_t TAIL_POINTER_SIZE = 8;

    [[nodiscard]] static bool parseTailPointer(
            std::uint64_t fileSize, const ByteSpan &tailPointerBytes,
            FileRange &fileTailRange, FormatError &error);

    [[nodiscard]] static bool parseFileTail(
            std::uint64_t fileSize, const FileRange &fileTailRange,
            const ByteSpan &fileTailBytes, proto::FileTail &fileTail,
            FormatError &error);

    [[nodiscard]] static bool validateFileTail(
            std::uint64_t fileSize, const proto::FileTail &fileTail,
            FormatError &error);

    [[nodiscard]] static bool parseRowGroupFooter(
            std::uint64_t fileSize, const FileRange &footerRange,
            const ByteSpan &footerBytes, proto::RowGroupFooter &footer,
            FormatError &error);

private:
    [[nodiscard]] static bool validateRowGroupRanges(
            std::uint64_t fileSize, std::uint64_t rowGroupEnd,
            const proto::Footer &footer,
            FormatError &error);
};

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_PIXELSFORMATREADER_H
