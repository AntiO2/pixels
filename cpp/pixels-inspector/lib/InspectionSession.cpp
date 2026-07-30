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
#include "format/PlainScalarDecoder.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
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

bool plainValueWidth(const proto::Type &type, std::uint64_t &width)
{
    switch (type.kind())
    {
        case proto::Type_Kind_BYTE:
            width = 1;
            return true;
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_FLOAT:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
            width = 4;
            return true;
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_DOUBLE:
        case proto::Type_Kind_TIMESTAMP:
            width = 8;
            return true;
        case proto::Type_Kind_DECIMAL:
            if (!type.has_precision() || !type.has_scale()
                || type.precision() == 0 || type.precision() > 38
                || type.scale() > type.precision())
            {
                return false;
            }
            width = type.precision() <= 18 ? 8 : 16;
            return true;
        case proto::Type_Kind_BOOLEAN:
            width = 0;
            return true;
        case proto::Type_Kind_STRING:
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_ARRAY:
        case proto::Type_Kind_MAP:
        case proto::Type_Kind_STRUCT:
        case proto::Type_Kind_VARBINARY:
        case proto::Type_Kind_VARCHAR:
        case proto::Type_Kind_CHAR:
        case proto::Type_Kind_VECTOR:
            return false;
    }
    return false;
}

std::string quoteInteger(std::int64_t value)
{
    return "\"" + std::to_string(value) + "\"";
}

std::string formatDecimal(std::int64_t value, std::uint32_t scale)
{
    const bool negative = value < 0;
    const std::uint64_t magnitude =
            negative
            ? static_cast<std::uint64_t>(-(value + 1)) + 1U
            : static_cast<std::uint64_t>(value);
    std::string digits = std::to_string(magnitude);
    if (scale > 0)
    {
        if (digits.size() <= scale)
        {
            digits.insert(
                    0, static_cast<std::size_t>(scale + 1U)
                       - digits.size(), '0');
        }
        digits.insert(digits.size() - scale, 1, '.');
    }
    if (negative)
    {
        digits.insert(0, 1, '-');
    }
    return "\"" + digits + "\"";
}

template<typename Floating>
std::string formatFloating(Floating value)
{
    if (std::isnan(value))
    {
        return "\"NaN\"";
    }
    if (std::isinf(value))
    {
        return value < 0 ? "\"-Infinity\"" : "\"Infinity\"";
    }
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<Floating>::max_digits10)
           << value;
    return output.str();
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
    return beginPageRequest(
            rowGroup, column, rowOffset, rowCount, true);
}

bool InspectionSession::beginPage(
        std::uint32_t rowGroup, std::uint32_t column,
        std::uint64_t rowOffset, std::uint32_t rowCount)
{
    return beginPageRequest(
            rowGroup, column, rowOffset, rowCount, false);
}

bool InspectionSession::beginPageRequest(
        std::uint32_t rowGroup, std::uint32_t column,
        std::uint64_t rowOffset, std::uint32_t rowCount,
        bool legacyLongResult)
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
    pageRequest_.legacyLongResult = legacyLongResult;
    pageRequest_.bitOffset = 0;
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
        case State::AWAITING_NULL_BITMAP:
            return consumeNullBitmap(bytes);
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
        case State::AWAITING_NULL_BITMAP:
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

    pageValidity_.reset(new bool[pageRequest_.rowCount]);
    const proto::RowGroupInformation &rowGroup =
            fileTail_.footer().rowgroupinfos(
                    static_cast<int>(pageRequest_.rowGroup));
    const std::uint32_t pixelRows = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                    fileTail_.postscript().pixelstride(),
                    rowGroup.numberofrows()));
    if (pixelHasNull(pageChunk_, 0))
    {
        if (!pageChunk_.has_isnulloffset())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "null-containing pixel has no null bitmap offset");
        }
        std::uint64_t nullFileOffset = 0;
        if (!format::checkedAdd(
                    pageChunk_.chunkoffset(), pageChunk_.isnulloffset(),
                    nullFileOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "pixel null bitmap offset overflows");
        }
        const std::uint64_t nullBytes =
                pixelRows / 8U + (pixelRows % 8U == 0 ? 0U : 1U);
        setPendingRange(
                format::FileRange{nullFileOffset, nullBytes},
                State::AWAITING_NULL_BITMAP);
        return true;
    }

    if (!format::PlainPixelPlanner::plan(
                pixelRows,
                static_cast<std::uint32_t>(pageRequest_.rowOffset),
                pageRequest_.rowCount, false,
                pageChunk_.has_nullspadding()
                && pageChunk_.nullspadding(),
                pageChunk_.has_littleendian()
                && pageChunk_.littleendian(),
                format::ByteSpan(), pageValidity_.get(),
                pageRequest_.rowCount, pagePlan_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    return requestPlainValues();
}

bool InspectionSession::consumeNullBitmap(const format::ByteSpan &bytes)
{
    const proto::RowGroupInformation &rowGroup =
            fileTail_.footer().rowgroupinfos(
                    static_cast<int>(pageRequest_.rowGroup));
    const std::uint32_t pixelRows = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                    fileTail_.postscript().pixelstride(),
                    rowGroup.numberofrows()));
    if (!format::PlainPixelPlanner::plan(
                pixelRows,
                static_cast<std::uint32_t>(pageRequest_.rowOffset),
                pageRequest_.rowCount, true,
                pageChunk_.has_nullspadding()
                && pageChunk_.nullspadding(),
                pageChunk_.has_littleendian()
                && pageChunk_.littleendian(),
                bytes, pageValidity_.get(), pageRequest_.rowCount,
                pagePlan_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    return requestPlainValues();
}

bool InspectionSession::requestPlainValues()
{
    if (pagePlan_.physicalCount == 0)
    {
        buildPageResult(
                std::vector<std::string>(
                        pageRequest_.rowCount, "null"));
        state_ = State::PAGE_READY;
        return true;
    }

    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    std::uint64_t valueWidth = 0;
    if (!plainValueWidth(type, valueWidth))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::UNSUPPORTED_TYPE,
                            "column does not have a supported plain scalar layout");
    }
    std::uint64_t pageByteOffset = 0;
    std::uint64_t pageByteLength = 0;
    if (type.kind() == proto::Type_Kind_BOOLEAN)
    {
        pageRequest_.bitOffset = pagePlan_.physicalOffset % 8U;
        pageByteOffset = pagePlan_.physicalOffset / 8U;
        pageByteLength =
                (static_cast<std::uint64_t>(pageRequest_.bitOffset)
                 + pagePlan_.physicalCount + 7U) / 8U;
    }
    else
    {
        pageByteOffset =
                static_cast<std::uint64_t>(pagePlan_.physicalOffset)
                * valueWidth;
        pageByteLength =
                static_cast<std::uint64_t>(pagePlan_.physicalCount)
                * valueWidth;
    }
    const std::uint64_t dataLength = pageChunk_.has_isnulloffset()
                                     ? pageChunk_.isnulloffset()
                                     : pageChunk_.chunklength();
    std::uint64_t pageByteEnd = 0;
    if (!format::checkedAdd(
                pageByteOffset, pageByteLength, pageByteEnd)
        || pageByteEnd > dataLength)
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain scalar page exceeds column data");
    }
    std::uint64_t pageFileOffset = 0;
    if (!format::checkedAdd(pageChunk_.chunkoffset(), pageByteOffset,
                            pageFileOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain scalar page file offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    pageFileOffset,
                    pageByteLength},
            State::AWAITING_COLUMN_CHUNK);
    return true;
}

bool InspectionSession::validatePageRequest()
{
    const proto::Footer &footer = fileTail_.footer();
    const proto::Type &type =
            footer.types(static_cast<int>(pageRequest_.column));
    if (!type.has_kind())
    {
        return format::fail(error_, format::ErrorCode::MALFORMED_PROTOBUF,
                            "column type kind is missing");
    }
    if (pageRequest_.legacyLongResult
        && type.kind() != proto::Type_Kind_LONG)
    {
        return format::fail(error_, format::ErrorCode::UNSUPPORTED_TYPE,
                            "validation page requires a LONG column");
    }
    std::uint64_t valueWidth = 0;
    if (!plainValueWidth(type, valueWidth))
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_TYPE,
                "plain scalar page does not support this column type");
    }
    if (type.kind() == proto::Type_Kind_DECIMAL
        && type.precision() > 18)
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_TYPE,
                "long DECIMAL page formatting is not implemented");
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
                "plain scalar page requires NONE encoding");
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
                "plain scalar page must stay within the first pixel");
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
    if (pageChunk_.pixelpositions_size() == 0
        || pageChunk_.pixelpositions(0) != 0)
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "first pixel position is missing or nonzero");
    }
    return true;
}

bool InspectionSession::consumeColumnChunk(const format::ByteSpan &bytes)
{
    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    std::vector<std::string> physicalValues(pagePlan_.physicalCount);
    bool decoded = false;
    switch (type.kind())
    {
        case proto::Type_Kind_BOOLEAN:
        {
            std::unique_ptr<bool[]> decodedValues(
                    new bool[pagePlan_.physicalCount]);
            decoded = format::PlainScalarDecoder::decodeBoolean(
                    bytes, littleEndian, pageRequest_.bitOffset,
                    pagePlan_.physicalCount, decodedValues.get(),
                    pagePlan_.physicalCount, error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            decodedValues[index] ? "true" : "false";
                }
            }
            break;
        }
        case proto::Type_Kind_BYTE:
        {
            std::vector<std::int8_t> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeByte(
                    bytes, 0, decodedValues.size(), decodedValues.data(),
                    decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            quoteInteger(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
        {
            std::vector<std::int32_t> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeInt32(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            quoteInteger(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_TIMESTAMP:
        {
            std::vector<std::int64_t> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeInt64(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            quoteInteger(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_FLOAT:
        {
            std::vector<float> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeFloat(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            formatFloating(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_DOUBLE:
        {
            std::vector<double> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeDouble(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            formatFloating(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_DECIMAL:
        {
            std::vector<std::int64_t> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeInt64(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] = formatDecimal(
                            decodedValues[index], type.scale());
                }
            }
            break;
        }
        case proto::Type_Kind_STRING:
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_ARRAY:
        case proto::Type_Kind_MAP:
        case proto::Type_Kind_STRUCT:
        case proto::Type_Kind_VARBINARY:
        case proto::Type_Kind_VARCHAR:
        case proto::Type_Kind_CHAR:
        case proto::Type_Kind_VECTOR:
            decoded = format::fail(
                    error_, format::ErrorCode::UNSUPPORTED_TYPE,
                    "column type is not a plain scalar");
            break;
    }
    if (!decoded)
    {
        state_ = State::FAILED;
        return false;
    }
    const bool nullsPadding =
            pageChunk_.has_nullspadding() && pageChunk_.nullspadding();
    std::vector<std::string> values(pageRequest_.rowCount);
    std::size_t physicalIndex = 0;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (pageValidity_[index])
        {
            values[index] = physicalValues[physicalIndex];
        }
        else
        {
            values[index] = "null";
        }
        if (nullsPadding || pageValidity_[index])
        {
            ++physicalIndex;
        }
    }
    if (physicalIndex != physicalValues.size())
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain scalar physical value count is inconsistent");
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
        const std::vector<std::string> &values)
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
        output << values[index];
    }
    output << "]}";
    result_ = output.str();
}

} // namespace inspector
} // namespace pixels
