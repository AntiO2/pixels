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

#include "format/PixelsFormatReader.h"

#include <climits>
#include <sstream>

namespace pixels
{
namespace format
{

namespace
{

const char *const PIXELS_MAGIC = "PIXELS";

bool validateProtoLength(std::uint64_t length, FormatError &error)
{
    if (length > static_cast<std::uint64_t>(INT_MAX))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "protobuf input exceeds the supported size");
    }
    return true;
}

std::string indexedMessage(const char *prefix, int index)
{
    std::ostringstream message;
    message << prefix << index;
    return message.str();
}

} // namespace

bool PixelsFormatReader::parseTailPointer(
        std::uint64_t fileSize, const ByteSpan &tailPointerBytes,
        FileRange &fileTailRange, FormatError &error)
{
    error.clear();
    fileTailRange = FileRange{};

    if (fileSize < TAIL_POINTER_SIZE)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "file is shorter than the eight-byte tail pointer");
    }
    if (!tailPointerBytes.isValid()
        || tailPointerBytes.size() != TAIL_POINTER_SIZE)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "tail pointer input must contain exactly eight bytes");
    }

    std::uint64_t fileTailOffset = 0;
    if (!readUnsigned64(tailPointerBytes, 0, false, fileTailOffset))
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "unable to read the tail pointer");
    }

    const std::uint64_t tailPointerOffset = fileSize - TAIL_POINTER_SIZE;
    if (fileTailOffset > tailPointerOffset)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "file tail starts beyond the tail pointer");
    }

    fileTailRange.offset = fileTailOffset;
    fileTailRange.length = tailPointerOffset - fileTailOffset;
    if (fileTailRange.length == 0)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "file tail is empty");
    }
    return validateProtoLength(fileTailRange.length, error);
}

bool PixelsFormatReader::parseFileTail(
        std::uint64_t fileSize, const FileRange &fileTailRange,
        const ByteSpan &fileTailBytes, proto::FileTail &fileTail,
        FormatError &error)
{
    error.clear();
    fileTail.Clear();

    if (fileSize < TAIL_POINTER_SIZE)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "file is shorter than the eight-byte tail pointer");
    }
    if (!isRangeWithinFile(fileTailRange, fileSize)
        || fileTailRange.offset + fileTailRange.length
           != fileSize - TAIL_POINTER_SIZE)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "file tail range does not end at the tail pointer");
    }
    if (!fileTailBytes.isValid()
        || fileTailBytes.size() != fileTailRange.length)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "supplied file tail bytes do not match the requested range");
    }
    if (!validateProtoLength(fileTailRange.length, error))
    {
        return false;
    }
    if (!fileTail.ParseFromArray(fileTailBytes.data(),
                                 static_cast<int>(fileTailBytes.size()))
        || !fileTail.IsInitialized())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "unable to parse a complete Pixels FileTail");
    }
    if (!validateFileTail(fileSize, fileTail, error))
    {
        return false;
    }
    return validateRowGroupRanges(
            fileSize, fileTailRange.offset, fileTail.footer(), error);
}

bool PixelsFormatReader::validateFileTail(
        std::uint64_t fileSize, const proto::FileTail &fileTail,
        FormatError &error)
{
    error.clear();
    if (fileSize < TAIL_POINTER_SIZE)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "file is shorter than the eight-byte tail pointer");
    }
    if (!fileTail.has_postscript())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "FileTail does not contain a PostScript");
    }
    if (!fileTail.has_footer())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "FileTail does not contain a Footer");
    }

    const proto::PostScript &postScript = fileTail.postscript();
    if (!postScript.has_version()
        || postScript.version() != SUPPORTED_VERSION)
    {
        return fail(error, ErrorCode::UNSUPPORTED_VERSION,
                    "Pixels file version is not supported");
    }
    if (!postScript.has_magic() || postScript.magic() != PIXELS_MAGIC)
    {
        return fail(error, ErrorCode::INVALID_MAGIC,
                    "Pixels file magic is invalid");
    }
    if (!postScript.has_pixelstride() || postScript.pixelstride() == 0)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "PostScript pixel stride must be positive");
    }
    if (!postScript.has_numberofrows())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "PostScript does not declare a row count");
    }
    if (postScript.has_contentlength()
        && postScript.contentlength() > fileSize - TAIL_POINTER_SIZE)
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "PostScript content length exceeds the file");
    }
    if (fileTail.footer().types_size() == 0)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "Footer schema is empty");
    }
    for (int index = 0; index < fileTail.footer().types_size(); ++index)
    {
        const proto::Type &type = fileTail.footer().types(index);
        if (!type.has_kind() || !type.has_name())
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        indexedMessage(
                                "schema type is missing kind or name: ",
                                index));
        }
    }
    return validateRowGroupRanges(
            fileSize, fileSize - TAIL_POINTER_SIZE,
            fileTail.footer(), error);
}

bool PixelsFormatReader::validateRowGroupRanges(
        std::uint64_t fileSize, std::uint64_t rowGroupEnd,
        const proto::Footer &footer,
        FormatError &error)
{
    for (int index = 0; index < footer.rowgroupinfos_size(); ++index)
    {
        const proto::RowGroupInformation &rowGroup =
                footer.rowgroupinfos(index);
        if (!rowGroup.has_footeroffset() || !rowGroup.has_footerlength())
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        indexedMessage(
                                "row group is missing its footer range: ",
                                index));
        }
        const FileRange range{rowGroup.footeroffset(),
                              rowGroup.footerlength()};
        if (range.length == 0 || !isRangeWithinFile(range, fileSize)
            || !isRangeWithinFile(range, rowGroupEnd))
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        indexedMessage(
                                "row-group footer is outside file content: ",
                                       index));
        }
        if (!validateProtoLength(range.length, error))
        {
            return false;
        }
    }
    return true;
}

bool PixelsFormatReader::parseRowGroupFooter(
        std::uint64_t fileSize, const FileRange &footerRange,
        const ByteSpan &footerBytes, proto::RowGroupFooter &footer,
        FormatError &error)
{
    error.clear();
    footer.Clear();

    if (footerRange.length == 0
        || !isRangeWithinFile(footerRange, fileSize))
    {
        return fail(error, ErrorCode::OUT_OF_BOUNDS,
                    "row-group footer range is out of file bounds");
    }
    if (!footerBytes.isValid() || footerBytes.size() != footerRange.length)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "supplied row-group footer does not match its range");
    }
    if (!validateProtoLength(footerRange.length, error))
    {
        return false;
    }
    if (!footer.ParseFromArray(footerBytes.data(),
                               static_cast<int>(footerBytes.size()))
        || !footer.IsInitialized())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "unable to parse a complete RowGroupFooter");
    }
    if (!footer.has_rowgroupindexentry()
        || !footer.has_rowgroupencoding())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RowGroupFooter is missing index or encoding metadata");
    }

    const proto::RowGroupIndex &index = footer.rowgroupindexentry();
    const proto::RowGroupEncoding &encoding = footer.rowgroupencoding();
    if (index.columnchunkindexentries_size()
        != encoding.columnchunkencodings_size())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "row-group index and encoding counts differ");
    }

    for (int column = 0;
         column < index.columnchunkindexentries_size(); ++column)
    {
        const proto::ColumnChunkIndex &chunk =
                index.columnchunkindexentries(column);
        if (!chunk.has_chunkoffset() || !chunk.has_chunklength())
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        indexedMessage(
                                "column chunk is missing its range: ", column));
        }
        const FileRange chunkRange{chunk.chunkoffset(), chunk.chunklength()};
        std::uint64_t chunkEnd = 0;
        if (chunkRange.length == 0
            || !isRangeWithinFile(chunkRange, fileSize)
            || !checkedAdd(chunkRange.offset, chunkRange.length, chunkEnd)
            || chunkEnd > footerRange.offset)
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        indexedMessage(
                                "column chunk is empty, out of bounds, or "
                                "overlaps its row-group footer: ",
                                column));
        }
        if (chunk.has_isnulloffset()
            && chunk.isnulloffset() > chunk.chunklength())
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        indexedMessage("null bitmap offset is out of bounds: ",
                                       column));
        }
    }
    return true;
}

} // namespace format
} // namespace pixels
