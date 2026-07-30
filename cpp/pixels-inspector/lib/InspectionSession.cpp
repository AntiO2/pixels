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
#include "format/RunLengthByteDecoder.h"
#include "format/RunLengthIntDecoder.h"
#include "format/VariableLengthDecoder.h"

#include <algorithm>
#include <climits>
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
const std::uint32_t MAX_PIXEL_ROWS = 1048576;
const std::uint32_t MAX_VECTOR_DIMENSION = 4096;
const std::uint64_t MAX_PAGE_SCALARS = 1048576;
const std::uint32_t MAX_DICTIONARY_ENTRIES = 1048576;
const std::uint64_t MAX_DICTIONARY_BYTES = 67108864;

bool checkedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t &result) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return false;
    }
    result = left * right;
    return true;
}

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
            return false;
        case proto::Type_Kind_VECTOR:
            if (!type.has_dimension()
                || type.dimension() == 0
                || type.dimension() > MAX_VECTOR_DIMENSION)
            {
                return false;
            }
            width = static_cast<std::uint64_t>(type.dimension()) * 8U;
            return true;
    }
    return false;
}

bool isRunLengthInteger(const proto::Type &type)
{
    switch (type.kind())
    {
        case proto::Type_Kind_BYTE:
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
        case proto::Type_Kind_TIMESTAMP:
            return true;
        case proto::Type_Kind_BOOLEAN:
        case proto::Type_Kind_FLOAT:
        case proto::Type_Kind_DOUBLE:
        case proto::Type_Kind_STRING:
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_DECIMAL:
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

bool isPlainVariableString(const proto::Type &type)
{
    switch (type.kind())
    {
        case proto::Type_Kind_STRING:
            return true;
        case proto::Type_Kind_VARCHAR:
        case proto::Type_Kind_CHAR:
            return type.has_maximumlength()
                   && type.maximumlength() > 0;
        case proto::Type_Kind_BOOLEAN:
        case proto::Type_Kind_BYTE:
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_FLOAT:
        case proto::Type_Kind_DOUBLE:
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_TIMESTAMP:
        case proto::Type_Kind_ARRAY:
        case proto::Type_Kind_MAP:
        case proto::Type_Kind_STRUCT:
        case proto::Type_Kind_VARBINARY:
        case proto::Type_Kind_DECIMAL:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
        case proto::Type_Kind_VECTOR:
            return false;
    }
    return false;
}

bool isLengthPrefixedBinary(const proto::Type &type)
{
    return (type.kind() == proto::Type_Kind_BINARY
            || type.kind() == proto::Type_Kind_VARBINARY)
           && type.has_maximumlength()
           && type.maximumlength() > 0;
}

std::string encodeBase64(const format::ByteSpan &bytes)
{
    static const char ALPHABET[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2U) / 3U * 4U);
    std::size_t index = 0;
    while (index + 3U <= bytes.size())
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes.data()[index]) << 16U
                | static_cast<std::uint32_t>(
                        bytes.data()[index + 1U]) << 8U
                | bytes.data()[index + 2U];
        encoded.push_back(ALPHABET[(value >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 12U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 6U) & 0x3FU]);
        encoded.push_back(ALPHABET[value & 0x3FU]);
        index += 3U;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1U)
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes.data()[index]) << 16U;
        encoded.push_back(ALPHABET[(value >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 12U) & 0x3FU]);
        encoded.append("==");
    }
    else if (remaining == 2U)
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes.data()[index]) << 16U
                | static_cast<std::uint32_t>(
                        bytes.data()[index + 1U]) << 8U;
        encoded.push_back(ALPHABET[(value >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 12U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 6U) & 0x3FU]);
        encoded.push_back('=');
    }
    return encoded;
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

std::string formatDecimal128(
        format::Int128Words value, std::uint32_t scale)
{
    const bool negative = (value.high >> 63U) != 0;
    if (negative)
    {
        value.low = ~value.low + 1U;
        value.high = ~value.high
                     + (value.low == 0 ? 1U : 0U);
    }

    std::uint32_t limbs[] = {
            static_cast<std::uint32_t>(value.high >> 32U),
            static_cast<std::uint32_t>(value.high),
            static_cast<std::uint32_t>(value.low >> 32U),
            static_cast<std::uint32_t>(value.low)};
    std::string digits;
    bool any = false;
    do
    {
        std::uint64_t remainder = 0;
        any = false;
        for (std::uint32_t &limb : limbs)
        {
            const std::uint64_t dividend =
                    (remainder << 32U) | limb;
            limb = static_cast<std::uint32_t>(dividend / 10U);
            remainder = dividend % 10U;
            any = any || limb != 0;
        }
        digits.push_back(static_cast<char>('0' + remainder));
    }
    while (any);
    std::reverse(digits.begin(), digits.end());

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
    const bool zero =
            value.high == 0 && value.low == 0;
    if (negative && !zero)
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
    pageRequest_.pixel = 0;
    pageRequest_.pixelRowOffset = 0;
    pageRequest_.pixelRowCount = 0;
    pageRequest_.resultOffset = 0;
    pageRequest_.nullBitmapByteOffset = 0;
    pageRequest_.pixelPhysicalBase = 0;
    pageRequest_.variableContentBase = 0;
    variableLayout_ = format::PlainVariableLayout{};
    dictionaryLayout_ = format::DictionaryVariableLayout{};
    variableRanges_.clear();
    dictionaryRanges_.clear();
    dictionaryContent_.clear();
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
        case State::AWAITING_PREFIX_NULL_BITMAP:
            return consumePrefixNullBitmap(bytes);
        case State::AWAITING_VARIABLE_TRAILER:
            return consumeVariableTrailer(bytes);
        case State::AWAITING_VARIABLE_STARTS:
            return consumeVariableStarts(bytes);
        case State::AWAITING_VARIABLE_CONTENT:
            return consumeVariableContent(bytes);
        case State::AWAITING_DICTIONARY_TRAILER:
            return consumeDictionaryTrailer(bytes);
        case State::AWAITING_DICTIONARY_STARTS:
            return consumeDictionaryStarts(bytes);
        case State::AWAITING_DICTIONARY_CONTENT:
            return consumeDictionaryContent(bytes);
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
        case State::AWAITING_PREFIX_NULL_BITMAP:
        case State::AWAITING_VARIABLE_TRAILER:
        case State::AWAITING_VARIABLE_STARTS:
        case State::AWAITING_VARIABLE_CONTENT:
        case State::AWAITING_DICTIONARY_TRAILER:
        case State::AWAITING_DICTIONARY_STARTS:
        case State::AWAITING_DICTIONARY_CONTENT:
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
    pageValues_.assign(pageRequest_.rowCount, std::string());
    const std::uint32_t pixelStride =
            fileTail_.postscript().pixelstride();
    pageRequest_.pixel = static_cast<std::uint32_t>(
            pageRequest_.rowOffset / pixelStride);
    pageRequest_.pixelRowOffset = static_cast<std::uint32_t>(
            pageRequest_.rowOffset % pixelStride);
    pageRequest_.resultOffset = 0;
    return preparePageLayout();
}

bool InspectionSession::preparePageLayout()
{
    if (isDictionaryPage())
    {
        if (pageChunk_.chunklength() < 8)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "dictionary chunk has no layout trailer");
        }
        std::uint64_t trailerOffset = 0;
        if (!format::checkedAdd(
                    pageChunk_.chunkoffset(),
                    pageChunk_.chunklength() - 8U, trailerOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "dictionary trailer offset overflows");
        }
        setPendingRange(
                format::FileRange{trailerOffset, 8},
                State::AWAITING_DICTIONARY_TRAILER);
        return true;
    }
    if (!isPlainVariablePage())
    {
        return requestCurrentPixel();
    }
    if (pageChunk_.chunklength() < 4)
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable chunk has no layout trailer");
    }
    std::uint64_t trailerOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(),
                pageChunk_.chunklength() - 4U, trailerOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable trailer offset overflows");
    }
    setPendingRange(
            format::FileRange{trailerOffset, 4},
            State::AWAITING_VARIABLE_TRAILER);
    return true;
}

bool InspectionSession::consumeDictionaryTrailer(
        const format::ByteSpan &bytes)
{
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    if (!format::VariableLengthDecoder::parseDictionaryLayout(
                bytes, littleEndian, pageChunk_.chunklength(),
                dictionaryLayout_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (!pageChunk_.has_isnulloffset()
        || pageChunk_.isnulloffset() > dictionaryLayout_.idsLength
        || dictionaryLayout_.dictionaryContentLength
           > MAX_DICTIONARY_BYTES)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "dictionary fields exceed their bounded layout");
    }
    std::uint64_t fileOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(),
                dictionaryLayout_.dictionaryStartsOffset,
                fileOffset))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "dictionary starts file offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    fileOffset,
                    dictionaryLayout_.dictionaryStartsLength},
            State::AWAITING_DICTIONARY_STARTS);
    return true;
}

bool InspectionSession::consumeDictionaryStarts(
        const format::ByteSpan &bytes)
{
    const proto::ColumnEncoding &encoding =
            rowGroupFooter_.rowgroupencoding()
                    .columnchunkencodings(
                            static_cast<int>(pageRequest_.column));
    const std::size_t dictionarySize = encoding.dictionarysize();
    std::vector<std::int64_t> starts(dictionarySize + 1U);
    bool decoded = false;
    if (usesCascadeRunLengthEncoding())
    {
        std::size_t consumed = 0;
        decoded = format::RunLengthIntDecoder::decode(
                bytes, false, starts.size(), starts.data(),
                starts.size(), consumed, error_);
    }
    else
    {
        if (starts.size()
            > std::numeric_limits<std::size_t>::max() / 4U
            || bytes.size() != starts.size() * 4U)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "dictionary starts field has an invalid length");
        }
        std::vector<std::int32_t> decodedStarts(starts.size());
        decoded = format::PlainScalarDecoder::decodeInt32(
                bytes,
                pageChunk_.has_littleendian()
                && pageChunk_.littleendian(),
                0, decodedStarts.size(), decodedStarts.data(),
                decodedStarts.size(), error_);
        if (decoded)
        {
            for (std::size_t index = 0;
                 index < starts.size(); ++index)
            {
                starts[index] = decodedStarts[index];
            }
        }
    }
    if (!decoded)
    {
        state_ = State::FAILED;
        return false;
    }
    dictionaryRanges_.clear();
    dictionaryRanges_.reserve(dictionarySize);
    for (std::size_t index = 0; index < starts.size(); ++index)
    {
        if (starts[index] < 0
            || static_cast<std::uint64_t>(starts[index])
               > dictionaryLayout_.dictionaryContentLength
            || (index != 0 && starts[index] < starts[index - 1U]))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "dictionary starts are unordered or out of bounds");
        }
    }
    if (starts.front() != 0
        || static_cast<std::uint64_t>(starts.back())
           != dictionaryLayout_.dictionaryContentLength)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "dictionary starts do not cover its content");
    }
    for (std::size_t index = 0; index < dictionarySize; ++index)
    {
        dictionaryRanges_.push_back(
                format::VariableValueRange{
                        static_cast<std::uint64_t>(starts[index]),
                        static_cast<std::uint64_t>(
                                starts[index + 1U] - starts[index])});
    }
    if (dictionaryLayout_.dictionaryContentLength == 0)
    {
        dictionaryContent_.clear();
        return requestCurrentPixel();
    }
    std::uint64_t fileOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(),
                dictionaryLayout_.dictionaryContentOffset,
                fileOffset))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "dictionary content file offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    fileOffset,
                    dictionaryLayout_.dictionaryContentLength},
            State::AWAITING_DICTIONARY_CONTENT);
    return true;
}

bool InspectionSession::consumeDictionaryContent(
        const format::ByteSpan &bytes)
{
    dictionaryContent_.assign(
            bytes.data(), bytes.data() + bytes.size());
    return requestCurrentPixel();
}

bool InspectionSession::consumeVariableTrailer(
        const format::ByteSpan &bytes)
{
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    if (!format::VariableLengthDecoder::parsePlainLayout(
                bytes, littleEndian, pageChunk_.chunklength(),
                variableLayout_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (!pageChunk_.has_isnulloffset()
        || pageChunk_.isnulloffset() > variableLayout_.startsOffset)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain variable null and starts offsets are inconsistent");
    }

    const std::uint64_t pixelStart =
            static_cast<std::uint64_t>(pageRequest_.pixel)
            * fileTail_.postscript().pixelstride();
    pageRequest_.pixelPhysicalBase = pixelStart;
    if (usesNullPadding() || pageRequest_.nullBitmapByteOffset == 0)
    {
        return requestCurrentPixel();
    }
    std::uint64_t prefixOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(), pageChunk_.isnulloffset(),
                prefixOffset))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable prefix bitmap offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    prefixOffset,
                    pageRequest_.nullBitmapByteOffset},
            State::AWAITING_PREFIX_NULL_BITMAP);
    return true;
}

bool InspectionSession::consumePrefixNullBitmap(
        const format::ByteSpan &bytes)
{
    if (!computeVariablePhysicalBase(bytes))
    {
        state_ = State::FAILED;
        return false;
    }
    return requestCurrentPixel();
}

bool InspectionSession::requestCurrentPixel()
{
    std::uint32_t pixelRows = 0;
    if (!currentPixelRows(pixelRows))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel is outside the row group");
    }
    const std::uint32_t rowsRemaining =
            pageRequest_.rowCount - pageRequest_.resultOffset;
    pageRequest_.pixelRowCount = std::min(
            rowsRemaining, pixelRows - pageRequest_.pixelRowOffset);

    if (pixelHasNull(pageChunk_, pageRequest_.pixel))
    {
        format::FileRange nullRange;
        if (!currentNullBitmapRange(nullRange))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "pixel null bitmap range is out of bounds");
        }
        setPendingRange(nullRange, State::AWAITING_NULL_BITMAP);
        return true;
    }

    if (!format::PlainPixelPlanner::plan(
                pixelRows,
                pageRequest_.pixelRowOffset,
                pageRequest_.pixelRowCount, false,
                usesNullPadding(),
                pageChunk_.has_littleendian()
                && pageChunk_.littleendian(),
                format::ByteSpan(),
                pageValidity_.get() + pageRequest_.resultOffset,
                pageRequest_.rowCount - pageRequest_.resultOffset,
                pagePlan_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    return requestPixelValues();
}

bool InspectionSession::consumeNullBitmap(const format::ByteSpan &bytes)
{
    std::uint32_t pixelRows = 0;
    if (!currentPixelRows(pixelRows))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel is outside the row group");
    }
    if (!format::PlainPixelPlanner::plan(
                pixelRows,
                pageRequest_.pixelRowOffset,
                pageRequest_.pixelRowCount, true,
                usesNullPadding(),
                pageChunk_.has_littleendian()
                && pageChunk_.littleendian(),
                bytes,
                pageValidity_.get() + pageRequest_.resultOffset,
                pageRequest_.rowCount - pageRequest_.resultOffset,
                pagePlan_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    return requestPixelValues();
}

bool InspectionSession::requestPixelValues()
{
    if (pagePlan_.physicalCount == 0)
    {
        return finishCurrentPixel(std::vector<std::string>());
    }

    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    if (isPlainVariablePage())
    {
        return requestVariableValues();
    }
    std::uint64_t pixelDataOffset = 0;
    std::uint64_t pixelDataLength = 0;
    if (!currentPixelDataRange(pixelDataOffset, pixelDataLength))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "pixel data range is invalid");
    }
    if (usesRunLengthEncoding())
    {
        if (pixelDataLength == 0)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "non-empty RLE pixel has no encoded content");
        }
        std::uint64_t pageFileOffset = 0;
        if (!format::checkedAdd(pageChunk_.chunkoffset(), pixelDataOffset,
                                pageFileOffset))
        {
            state_ = State::FAILED;
            return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                                "RLE pixel file offset overflows");
        }
        setPendingRange(
                format::FileRange{pageFileOffset, pixelDataLength},
                State::AWAITING_COLUMN_CHUNK);
        return true;
    }
    if (isDictionaryPage())
    {
        if (pixelDataLength == 0)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "non-empty dictionary pixel has no encoded IDs");
        }
        std::uint64_t pageFileOffset = 0;
        if (!format::checkedAdd(
                    pageChunk_.chunkoffset(), pixelDataOffset,
                    pageFileOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "dictionary ID file offset overflows");
        }
        setPendingRange(
                format::FileRange{pageFileOffset, pixelDataLength},
                State::AWAITING_COLUMN_CHUNK);
        return true;
    }
    if (isLengthPrefixedBinary(type))
    {
        if (pixelDataLength == 0)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "non-empty binary pixel has no encoded content");
        }
        std::uint64_t pageFileOffset = 0;
        if (!format::checkedAdd(
                    pageChunk_.chunkoffset(), pixelDataOffset,
                    pageFileOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "binary pixel file offset overflows");
        }
        setPendingRange(
                format::FileRange{pageFileOffset, pixelDataLength},
                State::AWAITING_COLUMN_CHUNK);
        return true;
    }

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
        if (!checkedMultiply(pagePlan_.physicalOffset, valueWidth,
                             pageByteOffset)
            || !checkedMultiply(pagePlan_.physicalCount, valueWidth,
                                pageByteLength))
        {
            state_ = State::FAILED;
            return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                                "plain scalar page byte count overflows");
        }
    }
    std::uint64_t pageByteEnd = 0;
    if (!format::checkedAdd(
                pageByteOffset, pageByteLength, pageByteEnd)
        || pageByteEnd > pixelDataLength)
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain scalar page exceeds its pixel data");
    }
    std::uint64_t chunkByteOffset = 0;
    if (!format::checkedAdd(pixelDataOffset, pageByteOffset,
                            chunkByteOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain scalar chunk offset overflows");
    }
    std::uint64_t pageFileOffset = 0;
    if (!format::checkedAdd(pageChunk_.chunkoffset(), chunkByteOffset,
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

bool InspectionSession::requestVariableValues()
{
    std::uint64_t firstValue = 0;
    std::uint64_t valueEnd = 0;
    if (!format::checkedAdd(
                pageRequest_.pixelPhysicalBase,
                pagePlan_.physicalOffset, firstValue)
        || !format::checkedAdd(
                firstValue, pagePlan_.physicalCount, valueEnd)
        || valueEnd > variableLayout_.physicalValueCount)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable starts window exceeds its value count");
    }
    std::uint64_t startsByteOffset = 0;
    std::uint64_t startsByteLength = 0;
    if (!checkedMultiply(firstValue, 4U, startsByteOffset)
        || !format::checkedAdd(
                variableLayout_.startsOffset, startsByteOffset,
                startsByteOffset)
        || !checkedMultiply(
                static_cast<std::uint64_t>(pagePlan_.physicalCount) + 1U,
                4U, startsByteLength))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable starts range overflows");
    }
    std::uint64_t startsEnd = 0;
    std::uint64_t layoutEnd = 0;
    if (!format::checkedAdd(
                startsByteOffset, startsByteLength, startsEnd)
        || !format::checkedAdd(
                variableLayout_.startsOffset,
                variableLayout_.startsLength, layoutEnd)
        || startsEnd > layoutEnd)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable starts range exceeds its field");
    }
    std::uint64_t fileOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(), startsByteOffset, fileOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable starts file offset overflows");
    }
    setPendingRange(
            format::FileRange{fileOffset, startsByteLength},
            State::AWAITING_VARIABLE_STARTS);
    return true;
}

bool InspectionSession::consumeVariableStarts(
        const format::ByteSpan &bytes)
{
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    if (!format::VariableLengthDecoder::decodeStartsWindow(
                bytes, littleEndian, pagePlan_.physicalCount,
                pageChunk_.isnulloffset(), variableRanges_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (variableRanges_.empty())
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain variable starts window is empty");
    }
    const std::uint64_t contentStart = variableRanges_.front().offset;
    const format::VariableValueRange &last = variableRanges_.back();
    std::uint64_t contentEnd = 0;
    std::uint64_t pixelOffset = 0;
    std::uint64_t pixelLength = 0;
    if (!format::checkedAdd(last.offset, last.length, contentEnd)
        || !currentPixelDataRange(pixelOffset, pixelLength))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable content range overflows");
    }
    std::uint64_t pixelEnd = 0;
    if (!format::checkedAdd(pixelOffset, pixelLength, pixelEnd)
        || contentStart < pixelOffset || contentEnd > pixelEnd)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain variable starts escape their pixel content");
    }
    pageRequest_.variableContentBase = contentStart;
    const std::uint64_t contentLength = contentEnd - contentStart;
    if (contentLength == 0)
    {
        return finishVariableValues(format::ByteSpan());
    }
    std::uint64_t fileOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(), contentStart, fileOffset))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable content file offset overflows");
    }
    setPendingRange(
            format::FileRange{fileOffset, contentLength},
            State::AWAITING_VARIABLE_CONTENT);
    return true;
}

bool InspectionSession::consumeVariableContent(
        const format::ByteSpan &bytes)
{
    return finishVariableValues(bytes);
}

bool InspectionSession::finishVariableValues(
        const format::ByteSpan &bytes)
{
    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    std::vector<std::string> physicalValues(variableRanges_.size());
    for (std::size_t index = 0;
         index < variableRanges_.size(); ++index)
    {
        const format::VariableValueRange &range =
                variableRanges_[index];
        if (range.offset < pageRequest_.variableContentBase)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "plain variable value precedes requested content");
        }
        const std::uint64_t relative =
                range.offset - pageRequest_.variableContentBase;
        if ((type.kind() == proto::Type_Kind_VARCHAR
             || type.kind() == proto::Type_Kind_CHAR)
            && range.length > type.maximumlength())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "plain variable value exceeds its schema length");
        }
        format::ByteSpan value;
        if (range.length == 0)
        {
            value = format::ByteSpan();
        }
        else if (relative > std::numeric_limits<std::size_t>::max()
                 || range.length
                    > std::numeric_limits<std::size_t>::max()
                 || !bytes.subspan(
                        static_cast<std::size_t>(relative),
                        static_cast<std::size_t>(range.length), value))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "plain variable value exceeds requested content");
        }
        if (!format::VariableLengthDecoder::isValidUtf8(value))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "string value is not valid UTF-8");
        }
        std::string text;
        if (!value.empty())
        {
            text.assign(
                    reinterpret_cast<const char *>(value.data()),
                    value.size());
        }
        physicalValues[index] = "\"" + escapeJson(text) + "\"";
    }
    return finishCurrentPixel(physicalValues);
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
    const bool variableString = isPlainVariableString(type);
    const bool binary = isLengthPrefixedBinary(type);
    std::uint64_t valueWidth = 0;
    if (!variableString && !binary
        && !plainValueWidth(type, valueWidth))
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_TYPE,
                "plain scalar page does not support this column type");
    }
    if (type.kind() == proto::Type_Kind_VECTOR
        && (type.dimension() > MAX_PAGE_SCALARS
                               / pageRequest_.rowCount))
    {
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "VECTOR page exceeds the bounded scalar limit");
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
    if (!encoding.has_kind())
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "column encoding kind is missing");
    }
    if (encoding.kind() == proto::ColumnEncoding_Kind_RUNLENGTH)
    {
        if (!isRunLengthInteger(type))
        {
            return format::fail(
                    error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                    "RLEv2 is not supported for this column type");
        }
    }
    else if (encoding.kind()
             == proto::ColumnEncoding_Kind_DICTIONARY)
    {
        if (!variableString
            || !encoding.has_dictionarysize()
            || encoding.dictionarysize() > MAX_DICTIONARY_ENTRIES
            || (encoding.has_cascadeencoding()
                && (!encoding.cascadeencoding().has_kind()
                    || encoding.cascadeencoding().kind()
                       != proto::ColumnEncoding_Kind_RUNLENGTH
                    || encoding.cascadeencoding()
                               .has_cascadeencoding())))
        {
            return format::fail(
                    error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                    "dictionary encoding metadata is unsupported");
        }
    }
    else if (encoding.kind() != proto::ColumnEncoding_Kind_NONE)
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                "column encoding is not supported");
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
    if (pixelStride == 0 || pixelStride > MAX_PIXEL_ROWS)
    {
        return format::fail(error_, format::ErrorCode::MALFORMED_PROTOBUF,
                            "pixel stride is outside the bounded range");
    }

    pageChunk_ = index.columnchunkindexentries(
            static_cast<int>(pageRequest_.column));
    const std::uint64_t rowGroupRows = rowGroup.numberofrows();
    const std::uint64_t pixelCount =
            rowGroupRows == 0
            ? 0
            : (rowGroupRows - 1U) / pixelStride + 1U;
    if (pixelCount == 0 || pixelCount > INT_MAX
        || pageChunk_.pixelstatistics_size()
           != static_cast<int>(pixelCount)
        || pageChunk_.pixelpositions_size()
           != static_cast<int>(pixelCount))
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "pixel positions and statistics do not match row-group rows");
    }

    const std::uint64_t dataLength =
            pageChunk_.has_isnulloffset()
            ? pageChunk_.isnulloffset()
            : pageChunk_.chunklength();
    std::uint64_t nullBitmapBytes = 0;
    std::uint32_t previousPosition = 0;
    for (std::uint64_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        const int pixelIndex = static_cast<int>(pixel);
        const proto::PixelStatistic &statistics =
                pageChunk_.pixelstatistics(pixelIndex);
        if (!statistics.has_statistic()
            || !statistics.statistic().has_hasnull())
        {
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "pixel is missing explicit null statistics");
        }
        const std::uint32_t position =
                pageChunk_.pixelpositions(pixelIndex);
        if ((pixel == 0 && position != 0)
            || (pixel != 0 && position < previousPosition)
            || position > dataLength)
        {
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "pixel positions are unordered or outside column data");
        }
        previousPosition = position;
        if (statistics.statistic().hasnull())
        {
            const std::uint64_t pixelStart = pixel * pixelStride;
            const std::uint64_t rows = std::min<std::uint64_t>(
                    pixelStride, rowGroupRows - pixelStart);
            const std::uint64_t bitmapBytes =
                    rows / 8U + (rows % 8U == 0 ? 0U : 1U);
            if (!format::checkedAdd(
                        nullBitmapBytes, bitmapBytes, nullBitmapBytes))
            {
                return format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "pixel null bitmap byte count overflows");
            }
        }
    }
    if (nullBitmapBytes != 0 && !pageChunk_.has_isnulloffset())
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "null-containing chunk has no null bitmap offset");
    }
    std::uint64_t nullBitmapEnd = 0;
    if (!format::checkedAdd(dataLength, nullBitmapBytes, nullBitmapEnd)
        || nullBitmapEnd > pageChunk_.chunklength())
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "pixel null bitmaps exceed the column chunk");
    }

    const std::uint64_t firstPixel =
            pageRequest_.rowOffset / pixelStride;
    if (firstPixel > std::numeric_limits<std::uint32_t>::max())
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel index exceeds the supported range");
    }
    pageRequest_.nullBitmapByteOffset = 0;
    for (std::uint64_t pixel = 0; pixel < firstPixel; ++pixel)
    {
        if (!pixelHasNull(pageChunk_, static_cast<std::uint32_t>(pixel)))
        {
            continue;
        }
        const std::uint64_t pixelStart = pixel * pixelStride;
        const std::uint64_t rows = std::min<std::uint64_t>(
                pixelStride, rowGroupRows - pixelStart);
        const std::uint64_t bitmapBytes =
                rows / 8U + (rows % 8U == 0 ? 0U : 1U);
        pageRequest_.nullBitmapByteOffset += bitmapBytes;
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
    if (isDictionaryPage())
    {
        std::vector<std::int64_t> pixelIds(
                pagePlan_.pixelPhysicalCount);
        bool decodedIds = false;
        if (usesCascadeRunLengthEncoding())
        {
            std::size_t consumed = 0;
            decodedIds = format::RunLengthIntDecoder::decode(
                    bytes, false, pixelIds.size(), pixelIds.data(),
                    pixelIds.size(), consumed, error_);
        }
        else
        {
            if (pixelIds.size()
                > std::numeric_limits<std::size_t>::max() / 4U
                || bytes.size() != pixelIds.size() * 4U)
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "dictionary ID field has an invalid pixel length");
            }
            std::vector<std::int32_t> decoded(pixelIds.size());
            decodedIds = format::PlainScalarDecoder::decodeInt32(
                    bytes, littleEndian, 0, decoded.size(),
                    decoded.data(), decoded.size(), error_);
            if (decodedIds)
            {
                for (std::size_t index = 0;
                     index < decoded.size(); ++index)
                {
                    pixelIds[index] = decoded[index];
                }
            }
        }
        if (!decodedIds)
        {
            state_ = State::FAILED;
            return false;
        }
        std::uint64_t physicalEnd = 0;
        if (!format::checkedAdd(
                    pagePlan_.physicalOffset, pagePlan_.physicalCount,
                    physicalEnd)
            || physicalEnd > pixelIds.size())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "dictionary page IDs exceed the decoded pixel");
        }
        format::ByteSpan dictionary(
                dictionaryContent_.data(),
                dictionaryContent_.size());
        for (std::size_t index = 0;
             index < physicalValues.size(); ++index)
        {
            const std::int64_t id =
                    pixelIds[pagePlan_.physicalOffset + index];
            if (id < 0
                || static_cast<std::uint64_t>(id)
                   >= dictionaryRanges_.size())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "dictionary ID is outside the dictionary");
            }
            const format::VariableValueRange &range =
                    dictionaryRanges_[static_cast<std::size_t>(id)];
            if ((type.kind() == proto::Type_Kind_VARCHAR
                 || type.kind() == proto::Type_Kind_CHAR)
                && range.length > type.maximumlength())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "dictionary string exceeds its schema length");
            }
            format::ByteSpan value;
            if (range.length == 0)
            {
                value = format::ByteSpan();
            }
            else if (range.offset
                > std::numeric_limits<std::size_t>::max()
                || range.length
                   > std::numeric_limits<std::size_t>::max()
                || !dictionary.subspan(
                        static_cast<std::size_t>(range.offset),
                        static_cast<std::size_t>(range.length), value)
                || !format::VariableLengthDecoder::isValidUtf8(value))
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "dictionary string is invalid");
            }
            std::string text;
            if (!value.empty())
            {
                text.assign(
                        reinterpret_cast<const char *>(value.data()),
                        value.size());
            }
            physicalValues[index] =
                    "\"" + escapeJson(text) + "\"";
        }
        return finishCurrentPixel(physicalValues);
    }
    if (isLengthPrefixedBinary(type))
    {
        std::vector<format::VariableValueRange> pixelRanges;
        if (!format::VariableLengthDecoder::decodeLengthPrefixed(
                    bytes, pagePlan_.pixelPhysicalCount,
                    type.maximumlength(), pixelRanges, error_))
        {
            state_ = State::FAILED;
            return false;
        }
        std::uint64_t physicalEnd = 0;
        if (!format::checkedAdd(
                    pagePlan_.physicalOffset, pagePlan_.physicalCount,
                    physicalEnd)
            || physicalEnd > pixelRanges.size())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "binary page values exceed the decoded pixel");
        }
        for (std::size_t index = 0;
             index < physicalValues.size(); ++index)
        {
            const format::VariableValueRange &range =
                    pixelRanges[pagePlan_.physicalOffset + index];
            format::ByteSpan value;
            if (range.offset
                > std::numeric_limits<std::size_t>::max()
                || range.length
                   > std::numeric_limits<std::size_t>::max()
                || !bytes.subspan(
                        static_cast<std::size_t>(range.offset),
                        static_cast<std::size_t>(range.length), value))
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "binary value exceeds the decoded pixel");
            }
            physicalValues[index] =
                    "\"" + encodeBase64(value) + "\"";
        }
        return finishCurrentPixel(physicalValues);
    }
    if (usesRunLengthEncoding())
    {
        if (type.kind() == proto::Type_Kind_BYTE)
        {
            std::vector<std::int8_t> pixelValues(
                    pagePlan_.pixelPhysicalCount);
            std::size_t consumedBytes = 0;
            if (!format::RunLengthByteDecoder::decode(
                        bytes, pixelValues.size(),
                        pixelValues.data(), pixelValues.size(),
                        consumedBytes, error_))
            {
                state_ = State::FAILED;
                return false;
            }
            std::uint64_t physicalEnd = 0;
            if (!format::checkedAdd(
                        pagePlan_.physicalOffset,
                        pagePlan_.physicalCount, physicalEnd)
                || physicalEnd > pixelValues.size())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "RLE byte page exceeds the decoded pixel");
            }
            for (std::size_t index = 0;
                 index < physicalValues.size(); ++index)
            {
                physicalValues[index] = quoteInteger(
                        pixelValues[
                                pagePlan_.physicalOffset + index]);
            }
            return finishCurrentPixel(physicalValues);
        }
        std::vector<std::int64_t> pixelValues(
                pagePlan_.pixelPhysicalCount);
        std::size_t consumedBytes = 0;
        if (!format::RunLengthIntDecoder::decode(
                    bytes, true, pixelValues.size(),
                    pixelValues.data(), pixelValues.size(),
                    consumedBytes, error_))
        {
            state_ = State::FAILED;
            return false;
        }
        std::uint64_t physicalEnd = 0;
        if (!format::checkedAdd(
                    pagePlan_.physicalOffset, pagePlan_.physicalCount,
                    physicalEnd)
            || physicalEnd > pixelValues.size())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "RLE page values exceed the decoded pixel");
        }
        for (std::size_t index = 0;
             index < physicalValues.size(); ++index)
        {
            physicalValues[index] = quoteInteger(
                    pixelValues[pagePlan_.physicalOffset + index]);
        }
        return finishCurrentPixel(physicalValues);
    }

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
            if (type.precision() <= 18)
            {
                std::vector<std::int64_t> decodedValues(
                        pagePlan_.physicalCount);
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
            }
            else
            {
                std::vector<format::Int128Words> decodedValues(
                        pagePlan_.physicalCount);
                decoded = format::PlainScalarDecoder::decodeInt128(
                        bytes, littleEndian, 0, decodedValues.size(),
                        decodedValues.data(), decodedValues.size(), error_);
                if (decoded)
                {
                    for (std::size_t index = 0;
                         index < physicalValues.size(); ++index)
                    {
                        physicalValues[index] = formatDecimal128(
                                decodedValues[index], type.scale());
                    }
                }
            }
            break;
        }
        case proto::Type_Kind_VECTOR:
        {
            const std::size_t dimension = type.dimension();
            if (pagePlan_.physicalCount
                > std::numeric_limits<std::size_t>::max() / dimension)
            {
                decoded = format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "VECTOR scalar count overflows");
                break;
            }
            const std::size_t scalarCount =
                    pagePlan_.physicalCount * dimension;
            std::vector<double> decodedValues(scalarCount);
            decoded = format::PlainScalarDecoder::decodeDouble(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t value = 0;
                     value < physicalValues.size(); ++value)
                {
                    std::string formatted = "[";
                    for (std::size_t component = 0;
                         component < dimension; ++component)
                    {
                        if (component != 0)
                        {
                            formatted += ",";
                        }
                        formatted += formatFloating(
                                decodedValues[
                                        value * dimension + component]);
                    }
                    formatted += "]";
                    physicalValues[value] = formatted;
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
    return finishCurrentPixel(physicalValues);
}

bool InspectionSession::finishCurrentPixel(
        const std::vector<std::string> &physicalValues)
{
    const bool nullsPadding = usesNullPadding();
    std::size_t physicalIndex = 0;
    for (std::uint32_t index = 0;
         index < pageRequest_.pixelRowCount; ++index)
    {
        const std::size_t resultIndex =
                static_cast<std::size_t>(pageRequest_.resultOffset) + index;
        if (pageValidity_[resultIndex])
        {
            if (physicalIndex >= physicalValues.size())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "plain scalar physical values end before the pixel");
            }
            pageValues_[resultIndex] = physicalValues[physicalIndex];
        }
        else
        {
            pageValues_[resultIndex] = "null";
        }
        if (nullsPadding || pageValidity_[resultIndex])
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
    return advancePixel();
}

bool InspectionSession::usesRunLengthEncoding() const
{
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    return pageRequest_.column
           < static_cast<std::uint32_t>(
                   encodings.columnchunkencodings_size())
           && encodings.columnchunkencodings(
                      static_cast<int>(pageRequest_.column)).has_kind()
           && encodings.columnchunkencodings(
                      static_cast<int>(pageRequest_.column)).kind()
              == proto::ColumnEncoding_Kind_RUNLENGTH;
}

bool InspectionSession::usesCascadeRunLengthEncoding() const
{
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                encodings.columnchunkencodings_size()))
    {
        return false;
    }
    const proto::ColumnEncoding &encoding =
            encodings.columnchunkencodings(
                    static_cast<int>(pageRequest_.column));
    return encoding.has_kind()
           && encoding.kind()
              == proto::ColumnEncoding_Kind_DICTIONARY
           && encoding.has_cascadeencoding()
           && encoding.cascadeencoding().has_kind()
           && encoding.cascadeencoding().kind()
              == proto::ColumnEncoding_Kind_RUNLENGTH;
}

bool InspectionSession::usesNullPadding() const
{
    if (pageRequest_.column
        < static_cast<std::uint32_t>(
                fileTail_.footer().types_size()))
    {
        const proto::Type &type =
                fileTail_.footer().types(
                        static_cast<int>(pageRequest_.column));
        if (isLengthPrefixedBinary(type)
            || type.kind() == proto::Type_Kind_VECTOR)
        {
            return false;
        }
    }
    return !usesRunLengthEncoding()
           && !usesCascadeRunLengthEncoding()
           && pageChunk_.has_nullspadding()
           && pageChunk_.nullspadding();
}

bool InspectionSession::isPlainVariablePage() const
{
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                fileTail_.footer().types_size()))
    {
        return false;
    }
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                encodings.columnchunkencodings_size()))
    {
        return false;
    }
    const proto::ColumnEncoding &encoding =
            encodings.columnchunkencodings(
                    static_cast<int>(pageRequest_.column));
    return isPlainVariableString(
                   fileTail_.footer().types(
                           static_cast<int>(pageRequest_.column)))
           && encoding.has_kind()
           && encoding.kind() == proto::ColumnEncoding_Kind_NONE;
}

bool InspectionSession::isDictionaryPage() const
{
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                fileTail_.footer().types_size()))
    {
        return false;
    }
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                encodings.columnchunkencodings_size()))
    {
        return false;
    }
    const proto::ColumnEncoding &encoding =
            encodings.columnchunkencodings(
                    static_cast<int>(pageRequest_.column));
    return isPlainVariableString(
                   fileTail_.footer().types(
                           static_cast<int>(pageRequest_.column)))
           && encoding.has_kind()
           && encoding.kind()
              == proto::ColumnEncoding_Kind_DICTIONARY;
}

bool InspectionSession::computeVariablePhysicalBase(
        const format::ByteSpan &prefixNullBitmaps)
{
    const std::uint64_t pixelStride =
            fileTail_.postscript().pixelstride();
    const std::uint64_t rowGroupRows =
            fileTail_.footer().rowgroupinfos(
                    static_cast<int>(pageRequest_.rowGroup)).numberofrows();
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    std::size_t bitmapOffset = 0;
    std::uint64_t physicalBase = 0;
    for (std::uint32_t pixel = 0;
         pixel < pageRequest_.pixel; ++pixel)
    {
        const std::uint64_t pixelStart =
                static_cast<std::uint64_t>(pixel) * pixelStride;
        const std::uint32_t rows = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                        pixelStride, rowGroupRows - pixelStart));
        if (!pixelHasNull(pageChunk_, pixel))
        {
            physicalBase += rows;
            continue;
        }
        const std::size_t bitmapBytes =
                rows / 8U + (rows % 8U == 0 ? 0U : 1U);
        format::ByteSpan bitmap;
        if (!prefixNullBitmaps.subspan(
                    bitmapOffset, bitmapBytes, bitmap))
        {
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "plain variable prefix null bitmap is truncated");
        }
        std::unique_ptr<bool[]> validity(new bool[rows]);
        format::PlainPixelPlan plan;
        if (!format::PlainPixelPlanner::plan(
                    rows, 0, rows, true, false, littleEndian,
                    bitmap, validity.get(), rows, plan, error_))
        {
            return false;
        }
        physicalBase += plan.pixelPhysicalCount;
        bitmapOffset += bitmapBytes;
    }
    if (bitmapOffset != prefixNullBitmaps.size())
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain variable prefix null bitmap has trailing bytes");
    }
    pageRequest_.pixelPhysicalBase = physicalBase;
    return true;
}

bool InspectionSession::advancePixel()
{
    std::uint32_t pixelRows = 0;
    if (!currentPixelRows(pixelRows))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel is outside the row group");
    }
    if (pixelHasNull(pageChunk_, pageRequest_.pixel))
    {
        const std::uint64_t bitmapBytes =
                pixelRows / 8U + (pixelRows % 8U == 0 ? 0U : 1U);
        if (!format::checkedAdd(
                    pageRequest_.nullBitmapByteOffset, bitmapBytes,
                    pageRequest_.nullBitmapByteOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "pixel null bitmap offset overflows");
        }
    }
    if (isPlainVariablePage()
        && !format::checkedAdd(
                pageRequest_.pixelPhysicalBase,
                pagePlan_.pixelPhysicalCount,
                pageRequest_.pixelPhysicalBase))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable physical index overflows");
    }

    pageRequest_.resultOffset += pageRequest_.pixelRowCount;
    if (pageRequest_.resultOffset == pageRequest_.rowCount)
    {
        buildPageResult(pageValues_);
        state_ = State::PAGE_READY;
        return true;
    }
    if (pageRequest_.pixel
        == std::numeric_limits<std::uint32_t>::max())
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel index overflows");
    }
    ++pageRequest_.pixel;
    pageRequest_.pixelRowOffset = 0;
    return requestCurrentPixel();
}

bool InspectionSession::currentPixelRows(std::uint32_t &rows) const
{
    const std::uint64_t pixelStride =
            fileTail_.postscript().pixelstride();
    const std::uint64_t rowGroupRows =
            fileTail_.footer().rowgroupinfos(
                    static_cast<int>(pageRequest_.rowGroup)).numberofrows();
    std::uint64_t pixelStart = 0;
    if (pixelStride == 0
        || !checkedMultiply(pageRequest_.pixel, pixelStride, pixelStart)
        || pixelStart >= rowGroupRows)
    {
        rows = 0;
        return false;
    }
    rows = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                    pixelStride, rowGroupRows - pixelStart));
    return true;
}

bool InspectionSession::currentPixelDataRange(
        std::uint64_t &offset, std::uint64_t &length) const
{
    if (pageRequest_.pixel
        >= static_cast<std::uint32_t>(
                pageChunk_.pixelpositions_size()))
    {
        return false;
    }
    offset = pageChunk_.pixelpositions(
            static_cast<int>(pageRequest_.pixel));
    const std::uint64_t end =
            pageRequest_.pixel + 1U
                    < static_cast<std::uint32_t>(
                              pageChunk_.pixelpositions_size())
            ? pageChunk_.pixelpositions(
                    static_cast<int>(pageRequest_.pixel + 1U))
            : (pageChunk_.has_isnulloffset()
               ? pageChunk_.isnulloffset()
               : pageChunk_.chunklength());
    if (end < offset)
    {
        return false;
    }
    length = end - offset;
    return true;
}

bool InspectionSession::currentNullBitmapRange(
        format::FileRange &range) const
{
    if (!pageChunk_.has_isnulloffset())
    {
        return false;
    }
    std::uint32_t pixelRows = 0;
    if (!currentPixelRows(pixelRows))
    {
        return false;
    }
    const std::uint64_t bitmapBytes =
            pixelRows / 8U + (pixelRows % 8U == 0 ? 0U : 1U);
    std::uint64_t chunkOffset = 0;
    std::uint64_t chunkEnd = 0;
    if (!format::checkedAdd(
                pageChunk_.isnulloffset(),
                pageRequest_.nullBitmapByteOffset, chunkOffset)
        || !format::checkedAdd(chunkOffset, bitmapBytes, chunkEnd)
        || chunkEnd > pageChunk_.chunklength()
        || !format::checkedAdd(
                pageChunk_.chunkoffset(), chunkOffset, range.offset))
    {
        return false;
    }
    range.length = bitmapBytes;
    return format::isRangeWithinFile(range, fileSize_);
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
