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

#include "InspectionSession.h"

#include "format/PixelsFormatReader.h"
#include "format/PlainLongDecoder.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace pixels
{
namespace inspector
{

namespace
{

const std::uint32_t MAX_PAGE_ROWS = 65536;

std::string escapeJson(const std::string &value)
{
    std::ostringstream escaped;
    for (std::string::const_iterator iterator = value.begin();
         iterator != value.end(); ++iterator)
    {
        const unsigned char character =
                static_cast<unsigned char>(*iterator);
        switch (character)
        {
            case '"':
                escaped << "\\\"";
                break;
            case '\\':
                escaped << "\\\\";
                break;
            case '\b':
                escaped << "\\b";
                break;
            case '\f':
                escaped << "\\f";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if (character < 0x20U)
                {
                    static const char HEX[] = "0123456789abcdef";
                    escaped << "\\u00" << HEX[(character >> 4U) & 0x0FU]
                            << HEX[character & 0x0FU];
                }
                else
                {
                    escaped << static_cast<char>(character);
                }
        }
    }
    return escaped.str();
}

bool pixelHasNull(const proto::ColumnChunkIndex &chunk,
                  std::uint32_t pixel)
{
    if (pixel >= static_cast<std::uint32_t>(chunk.pixelstatistics_size()))
    {
        return false;
    }
    const proto::PixelStatistic &pixelStatistics =
            chunk.pixelstatistics(static_cast<int>(pixel));
    return pixelStatistics.has_statistic()
           && pixelStatistics.statistic().has_hasnull()
           && pixelStatistics.statistic().hasnull();
}

} // namespace

InspectionSession::InspectionSession(std::uint64_t fileSize)
        : fileSize_(fileSize)
{
}

bool InspectionSession::beginMetadata()
{
    if (state_ != State::IDLE)
    {
        return transitionFailure(format::ErrorCode::INVALID_STATE,
                                 "metadata can only begin from the idle state");
    }
    if (fileSize_ < format::PixelsFormatReader::TAIL_POINTER_SIZE)
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "file is shorter than the eight-byte tail pointer");
    }

    result_.clear();
    error_.clear();
    setPendingRange(
            format::FileRange{
                    fileSize_
                    - format::PixelsFormatReader::TAIL_POINTER_SIZE,
                    format::PixelsFormatReader::TAIL_POINTER_SIZE},
            State::AWAITING_TAIL_POINTER);
    return true;
}

bool InspectionSession::beginPlainLongPage(
        std::uint32_t rowGroup, std::uint32_t column,
        std::uint64_t rowOffset, std::uint32_t rowCount)
{
    if (state_ != State::METADATA_READY)
    {
        return transitionFailure(format::ErrorCode::INVALID_STATE,
                                 "page decoding requires ready metadata");
    }
    if (rowCount == 0)
    {
        return transitionFailure(format::ErrorCode::INVALID_ARGUMENT,
                                 "page row count must be positive");
    }
    if (rowCount > MAX_PAGE_ROWS)
    {
        return transitionFailure(format::ErrorCode::OUT_OF_BOUNDS,
                                 "page row count exceeds the bounded limit");
    }
    if (rowGroup >= static_cast<std::uint32_t>(
                            fileTail_.footer().rowgroupinfos_size()))
    {
        return transitionFailure(format::ErrorCode::OUT_OF_BOUNDS,
                                 "row-group index is out of bounds");
    }
    if (column >= static_cast<std::uint32_t>(
                          fileTail_.footer().types_size()))
    {
        return transitionFailure(format::ErrorCode::OUT_OF_BOUNDS,
                                 "column index is out of bounds");
    }

    pageRequest_.rowGroup = rowGroup;
    pageRequest_.column = column;
    pageRequest_.rowOffset = rowOffset;
    pageRequest_.rowCount = rowCount;
    result_.clear();
    error_.clear();

    const proto::RowGroupInformation &rowGroupInformation =
            fileTail_.footer().rowgroupinfos(static_cast<int>(rowGroup));
    setPendingRange(
            format::FileRange{rowGroupInformation.footeroffset(),
                              rowGroupInformation.footerlength()},
            State::AWAITING_ROW_GROUP_FOOTER);
    return true;
}

bool InspectionSession::nextRange(format::FileRange &range) const
{
    if (!hasPendingRange_)
    {
        return false;
    }
    range = pendingRange_;
    return true;
}

bool InspectionSession::supplyRange(
        const format::FileRange &range, const format::ByteSpan &bytes)
{
    if (!hasPendingRange_)
    {
        return transitionFailure(format::ErrorCode::INVALID_STATE,
                                 "no byte range is currently pending");
    }
    if (!(range == pendingRange_))
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "supplied byte range does not match the pending request");
    }
    if (!bytes.isValid() || bytes.size() != range.length)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "supplied byte count does not match the pending request");
    }

    hasPendingRange_ = false;
    switch (state_)
    {
        case State::AWAITING_TAIL_POINTER:
            return consumeTailPointer(bytes);
        case State::AWAITING_FILE_TAIL:
            return consumeFileTail(bytes);
        case State::AWAITING_ROW_GROUP_FOOTER:
            return consumeRowGroupFooter(bytes);
        case State::AWAITING_COLUMN_CHUNK:
            return consumeColumnChunk(bytes);
        case State::IDLE:
        case State::METADATA_READY:
        case State::PAGE_READY:
        case State::CANCELLED:
        case State::FAILED:
            return transitionFailure(format::ErrorCode::INVALID_STATE,
                                     "current state cannot consume a range");
    }
    return transitionFailure(format::ErrorCode::INVALID_STATE,
                             "unknown inspection state");
}

bool InspectionSession::cancel()
{
    switch (state_)
    {
        case State::AWAITING_TAIL_POINTER:
        case State::AWAITING_FILE_TAIL:
        case State::METADATA_READY:
        case State::AWAITING_ROW_GROUP_FOOTER:
        case State::AWAITING_COLUMN_CHUNK:
            state_ = State::CANCELLED;
            hasPendingRange_ = false;
            result_.clear();
            error_.code = format::ErrorCode::CANCELLED;
            error_.message = "inspection was cancelled";
            return true;
        case State::IDLE:
        case State::PAGE_READY:
        case State::CANCELLED:
        case State::FAILED:
            return transitionFailure(format::ErrorCode::INVALID_STATE,
                                     "current state cannot be cancelled");
    }
    return transitionFailure(format::ErrorCode::INVALID_STATE,
                             "unknown inspection state");
}

bool InspectionSession::consumeTailPointer(const format::ByteSpan &bytes)
{
    format::FileRange fileTailRange;
    if (!format::PixelsFormatReader::parseTailPointer(
                fileSize_, bytes, fileTailRange, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    setPendingRange(fileTailRange, State::AWAITING_FILE_TAIL);
    return true;
}

bool InspectionSession::consumeFileTail(const format::ByteSpan &bytes)
{
    if (!format::PixelsFormatReader::parseFileTail(
                fileSize_, pendingRange_, bytes, fileTail_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    buildMetadataResult();
    state_ = State::METADATA_READY;
    return true;
}

bool InspectionSession::consumeRowGroupFooter(const format::ByteSpan &bytes)
{
    if (!format::PixelsFormatReader::parseRowGroupFooter(
                fileSize_, pendingRange_, bytes, rowGroupFooter_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (!validatePageRequest())
    {
        state_ = State::FAILED;
        return false;
    }

    const std::uint64_t pageByteOffset =
            pageRequest_.rowOffset * sizeof(std::int64_t);
    std::uint64_t pageFileOffset = 0;
    if (!format::checkedAdd(pageChunk_.chunkoffset(), pageByteOffset,
                            pageFileOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain LONG page file offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    pageFileOffset,
                    static_cast<std::uint64_t>(pageRequest_.rowCount)
                    * sizeof(std::int64_t)},
            State::AWAITING_COLUMN_CHUNK);
    return true;
}

bool InspectionSession::validatePageRequest()
{
    const proto::Footer &footer = fileTail_.footer();
    const proto::Type &type =
            footer.types(static_cast<int>(pageRequest_.column));
    if (!type.has_kind()
        || type.kind() != proto::Type_Kind_LONG)
    {
        return format::fail(error_, format::ErrorCode::UNSUPPORTED_TYPE,
                            "validation page requires a LONG column");
    }

    const proto::RowGroupIndex &index =
            rowGroupFooter_.rowgroupindexentry();
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                index.columnchunkindexentries_size())
        || pageRequest_.column
           >= static_cast<std::uint32_t>(
                   encodings.columnchunkencodings_size()))
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "column metadata is missing from the row group");
    }

    const proto::ColumnEncoding &encoding =
            encodings.columnchunkencodings(
                    static_cast<int>(pageRequest_.column));
    if (!encoding.has_kind()
        || encoding.kind() != proto::ColumnEncoding_Kind_NONE)
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                "validation page requires NONE encoding");
    }

    const proto::RowGroupInformation &rowGroup =
            footer.rowgroupinfos(static_cast<int>(pageRequest_.rowGroup));
    std::uint64_t rowEnd = 0;
    if (!format::checkedAdd(pageRequest_.rowOffset,
                            pageRequest_.rowCount, rowEnd)
        || !rowGroup.has_numberofrows()
        || rowEnd > rowGroup.numberofrows())
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page rows exceed the row group");
    }

    const std::uint32_t pixelStride =
            fileTail_.postscript().pixelstride();
    if (pageRequest_.rowOffset >= pixelStride || rowEnd > pixelStride)
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                "validation page must stay within the first pixel");
    }

    pageChunk_ = index.columnchunkindexentries(
            static_cast<int>(pageRequest_.column));
    if (pageChunk_.pixelstatistics_size() == 0
        || !pageChunk_.pixelstatistics(0).has_statistic()
        || !pageChunk_.pixelstatistics(0).statistic().has_hasnull())
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "first pixel is missing explicit null statistics");
    }
    if (pixelHasNull(pageChunk_, 0))
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                "validation page does not yet support null values");
    }

    const std::uint64_t dataLength = pageChunk_.has_isnulloffset()
                                     ? pageChunk_.isnulloffset()
                                     : pageChunk_.chunklength();
    if (pageRequest_.rowOffset
        > std::numeric_limits<std::uint64_t>::max()
          / sizeof(std::int64_t)
        || rowEnd > dataLength / sizeof(std::int64_t))
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain LONG page exceeds column data");
    }
    return true;
}

bool InspectionSession::consumeColumnChunk(const format::ByteSpan &bytes)
{
    std::vector<std::int64_t> values(pageRequest_.rowCount);
    if (!format::PlainLongDecoder::decode(
                bytes, pageChunk_.has_littleendian()
                       && pageChunk_.littleendian(),
                0, values.size(), values.data(),
                values.size(), error_))
    {
        state_ = State::FAILED;
        return false;
    }
    buildPageResult(values);
    state_ = State::PAGE_READY;
    return true;
}

bool InspectionSession::transitionFailure(
        format::ErrorCode code, const std::string &message)
{
    state_ = State::FAILED;
    hasPendingRange_ = false;
    result_.clear();
    return format::fail(error_, code, message);
}

void InspectionSession::setPendingRange(
        const format::FileRange &range, State state)
{
    pendingRange_ = range;
    hasPendingRange_ = true;
    state_ = state;
}

void InspectionSession::buildMetadataResult()
{
    const proto::PostScript &postScript = fileTail_.postscript();
    const proto::Footer &footer = fileTail_.footer();
    const proto::Type &firstType = footer.types(0);

    std::ostringstream output;
    output << "{\"abi\":1"
           << ",\"version\":" << postScript.version()
           << ",\"magic\":\"" << escapeJson(postScript.magic()) << "\""
           << ",\"rows\":" << postScript.numberofrows()
           << ",\"pixelStride\":" << postScript.pixelstride()
           << ",\"schemaCount\":" << footer.types_size()
           << ",\"rowGroupCount\":" << footer.rowgroupinfos_size()
           << ",\"firstColumn\":{\"name\":\""
           << escapeJson(firstType.name()) << "\",\"kind\":"
           << static_cast<int>(firstType.kind()) << "}}";
    result_ = output.str();
}

void InspectionSession::buildPageResult(
        const std::vector<std::int64_t> &values)
{
    std::ostringstream output;
    output << "{\"rowGroup\":" << pageRequest_.rowGroup
           << ",\"column\":" << pageRequest_.column
           << ",\"offset\":" << pageRequest_.rowOffset
           << ",\"count\":" << pageRequest_.rowCount
           << ",\"values\":[";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            output << ',';
        }
        output << '"' << values[index] << '"';
    }
    output << "]}";
    result_ = output.str();
}

} // namespace inspector
} // namespace pixels
