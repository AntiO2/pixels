/*
 * Copyright 2026 PixelsDB.
 *
 * Native conformance tests for the host-driven Pixels inspector boundary.
 */

#include "pixels_inspector.h"

#include "format/ByteReader.h"
#include "format/PlainLongDecoder.h"
#include "format/PlainPixelPlanner.h"
#include "format/PlainScalarDecoder.h"
#include "pixels.pb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

const std::string EXPECTED_METADATA =
        "{\"abi\":1,\"version\":1,\"magic\":\"PIXELS\",\"rows\":10,"
        "\"pixelStride\":10000,\"schemaCount\":4,\"rowGroupCount\":1,"
        "\"firstColumn\":{\"name\":\"id\",\"kind\":4}}";

const std::string EXPECTED_PAGE =
        "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":10,"
        "\"values\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\","
        "\"7\",\"8\",\"9\"]}";

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> readFixture()
{
    std::ifstream input(PIXELS_TEST_FIXTURE, std::ios::binary);
    require(input.good(), "unable to open the canonical fixture");
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    require(length >= 0, "unable to determine fixture length");
    input.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char *>(bytes.data()), length);
    }
    require(input.good(), "unable to read the canonical fixture");
    return bytes;
}

class Session
{
public:
    explicit Session(std::uint64_t fileSize)
    {
        require(pixels_inspector_create(fileSize, &handle_)
                == PIXELS_INSPECTOR_OK,
                "unable to create an inspection session");
    }

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    ~Session()
    {
        if (handle_ != 0)
        {
            pixels_inspector_destroy(handle_);
        }
    }

    pixels_inspector_handle handle() const
    {
        return handle_;
    }

    void destroy()
    {
        require(pixels_inspector_destroy(handle_) == PIXELS_INSPECTOR_OK,
                "unable to destroy inspection session");
        handle_ = 0;
    }

private:
    pixels_inspector_handle handle_ = 0;
};

pixels::format::FileRange nextRange(const Session &session)
{
    pixels::format::FileRange range;
    require(pixels_inspector_next_range(
                    session.handle(), &range.offset, &range.length)
            == PIXELS_INSPECTOR_RANGE_READY,
            "expected a pending range");
    return range;
}

pixels_inspector_status supply(
        const Session &session, const pixels::format::FileRange &range,
        const std::vector<std::uint8_t> &file)
{
    require(range.offset <= file.size()
            && range.length <= file.size() - range.offset,
            "test attempted to supply an invalid fixture range");
    return pixels_inspector_supply_range(
            session.handle(), range.offset, range.length,
            file.data() + static_cast<std::size_t>(range.offset));
}

void replaceSerializedRange(
        std::vector<std::uint8_t> &file,
        const pixels::format::FileRange &range,
        const google::protobuf::MessageLite &message)
{
    std::string serialized;
    require(message.SerializeToString(&serialized),
            "unable to serialize mutated protobuf");
    require(serialized.size() == range.length,
            "mutated protobuf changed the fixture range length");
    std::copy(serialized.begin(), serialized.end(),
              file.begin() + static_cast<std::size_t>(range.offset));
}

void mutateFileTail(
        std::vector<std::uint8_t> &file,
        const std::function<void(pixels::proto::FileTail &)> &mutation)
{
    const pixels::format::FileRange range{506, 276};
    pixels::proto::FileTail tail;
    require(tail.ParseFromArray(
                    file.data() + range.offset,
                    static_cast<int>(range.length)),
            "unable to parse fixture FileTail for mutation");
    mutation(tail);
    replaceSerializedRange(file, range, tail);
}

void mutateRowGroupFooter(
        std::vector<std::uint8_t> &file,
        const std::function<void(pixels::proto::RowGroupFooter &)> &mutation)
{
    const pixels::format::FileRange range{352, 154};
    pixels::proto::RowGroupFooter footer;
    require(footer.ParseFromArray(
                    file.data() + range.offset,
                    static_cast<int>(range.length)),
            "unable to parse fixture RowGroupFooter for mutation");
    mutation(footer);
    replaceSerializedRange(file, range, footer);
}

void appendLittleInt64(
        std::vector<std::uint8_t> &bytes, std::int64_t value)
{
    const std::uint64_t bits = static_cast<std::uint64_t>(value);
    for (std::uint32_t byte = 0; byte < 8; ++byte)
    {
        bytes.push_back(static_cast<std::uint8_t>(
                bits >> (byte * 8U)));
    }
}

void appendLittleUint32(
        std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (std::uint32_t byte = 0; byte < 4; ++byte)
    {
        bytes.push_back(static_cast<std::uint8_t>(
                value >> (byte * 8U)));
    }
}

void appendBigDouble(
        std::vector<std::uint8_t> &bytes, double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value),
                  "VECTOR fixture requires binary64");
    std::memcpy(&bits, &value, sizeof(bits));
    for (int byte = 7; byte >= 0; --byte)
    {
        bytes.push_back(static_cast<std::uint8_t>(
                bits >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
}

void appendDirectRleValues(
        std::vector<std::uint8_t> &bytes,
        const std::vector<std::uint64_t> &encoded)
{
    require(!encoded.empty() && encoded.size() <= 512,
            "RLE test run length is invalid");
    std::uint64_t maximum = 0;
    for (const std::uint64_t value : encoded)
    {
        maximum = std::max(maximum, value);
    }
    std::uint32_t width = 1;
    while (width < 24U && (maximum >> width) != 0)
    {
        ++width;
    }
    require(width <= 24U, "RLE test value exceeds the compact helper");
    const std::uint32_t encodedWidth = width - 1U;
    const std::uint32_t encodedLength =
            static_cast<std::uint32_t>(encoded.size() - 1U);
    bytes.push_back(static_cast<std::uint8_t>(
            0x40U | (encodedWidth << 1U)
            | ((encodedLength >> 8U) & 1U)));
    bytes.push_back(static_cast<std::uint8_t>(encodedLength));

    std::uint8_t packed = 0;
    std::uint32_t packedBits = 0;
    for (const std::uint64_t value : encoded)
    {
        for (std::uint32_t bit = 0; bit < width; ++bit)
        {
            const std::uint32_t source = width - bit - 1U;
            packed = static_cast<std::uint8_t>(
                    (packed << 1U) | ((value >> source) & 1U));
            ++packedBits;
            if (packedBits == 8U)
            {
                bytes.push_back(packed);
                packed = 0;
                packedBits = 0;
            }
        }
    }
    if (packedBits != 0)
    {
        bytes.push_back(static_cast<std::uint8_t>(
                packed << (8U - packedBits)));
    }
}

void appendDirectRleInt64(
        std::vector<std::uint8_t> &bytes,
        const std::vector<std::int64_t> &values)
{
    std::vector<std::uint64_t> encoded(values.size());
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const std::uint64_t bits =
                static_cast<std::uint64_t>(values[index]);
        encoded[index] =
                (bits << 1U)
                ^ (static_cast<std::uint64_t>(0)
                   - (bits >> 63U));
    }
    appendDirectRleValues(bytes, encoded);
}

void appendDirectRleUint32(
        std::vector<std::uint8_t> &bytes,
        const std::vector<std::uint32_t> &values)
{
    std::vector<std::uint64_t> encoded(
            values.begin(), values.end());
    appendDirectRleValues(bytes, encoded);
}

std::vector<std::uint8_t> makeMultiPixelLongFixture(
        bool nullsPadding,
        const std::vector<std::uint8_t> &nullMasks,
        const std::function<void(pixels::proto::RowGroupFooter &)> &mutation =
                std::function<void(pixels::proto::RowGroupFooter &)>(),
        bool runLengthEncoding = false)
{
    require(nullMasks.size() == 3,
            "multi-pixel fixture requires three null masks");
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::RowGroupFooter rowGroupFooter;
    require(rowGroupFooter.ParseFromArray(
                    canonical.data() + 352, 154),
            "unable to parse canonical RowGroupFooter");
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse canonical FileTail");

    pixels::proto::ColumnChunkIndex *chunk =
            rowGroupFooter.mutable_rowgroupindexentry()
                    ->mutable_columnchunkindexentries(0);
    chunk->clear_pixelpositions();
    chunk->clear_pixelstatistics();
    chunk->set_littleendian(true);
    chunk->set_nullspadding(nullsPadding);

    std::vector<std::uint8_t> columnData;
    for (std::uint32_t pixel = 0; pixel < 3; ++pixel)
    {
        chunk->add_pixelpositions(
                static_cast<std::uint32_t>(columnData.size()));
        pixels::proto::PixelStatistic *statistics =
                chunk->add_pixelstatistics();
        statistics->mutable_statistic()->set_hasnull(
                nullMasks[pixel] != 0);
        const std::uint32_t rowStart = pixel * 4U;
        const std::uint32_t rows = pixel == 2 ? 2U : 4U;
        std::vector<std::int64_t> pixelValues;
        for (std::uint32_t row = 0; row < rows; ++row)
        {
            const bool isNull =
                    ((nullMasks[pixel] >> row) & 1U) != 0;
            if ((nullsPadding && !runLengthEncoding) || !isNull)
            {
                pixelValues.push_back(
                        static_cast<std::int64_t>(rowStart + row));
            }
        }
        if (runLengthEncoding)
        {
            if (!pixelValues.empty())
            {
                appendDirectRleInt64(columnData, pixelValues);
            }
        }
        else
        {
            for (const std::int64_t value : pixelValues)
            {
                appendLittleInt64(columnData, value);
            }
        }
    }
    if (runLengthEncoding)
    {
        rowGroupFooter.mutable_rowgroupencoding()
                ->mutable_columnchunkencodings(0)
                ->set_kind(pixels::proto::ColumnEncoding_Kind_RUNLENGTH);
    }
    chunk->set_isnulloffset(
            static_cast<std::uint32_t>(columnData.size()));
    for (std::size_t pixel = 0; pixel < nullMasks.size(); ++pixel)
    {
        if (nullMasks[pixel] != 0)
        {
            columnData.push_back(nullMasks[pixel]);
        }
    }
    chunk->set_chunklength(
            static_cast<std::uint32_t>(columnData.size()));
    if (mutation)
    {
        mutation(rowGroupFooter);
    }

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize multi-pixel RowGroupFooter");
    pixels::proto::RowGroupInformation *rowGroup =
            fileTail.mutable_footer()->mutable_rowgroupinfos(0);
    rowGroup->set_footeroffset(352);
    rowGroup->set_footerlength(serializedFooter.size());
    rowGroup->set_numberofrows(10);
    fileTail.mutable_postscript()->set_pixelstride(4);
    fileTail.mutable_postscript()->set_numberofrows(10);

    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize multi-pixel FileTail");
    std::vector<std::uint8_t> file(canonical.begin(),
                                   canonical.begin() + 352);
    std::copy(columnData.begin(), columnData.end(), file.begin());
    file.insert(file.end(), serializedFooter.begin(),
                serializedFooter.end());
    const std::uint64_t tailOffset = file.size();
    file.insert(file.end(), serializedTail.begin(), serializedTail.end());
    for (int byte = 7; byte >= 0; --byte)
    {
        file.push_back(static_cast<std::uint8_t>(
                tailOffset >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
    return file;
}

std::vector<std::uint8_t> makeMultiPixelVarcharFixture()
{
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::RowGroupFooter rowGroupFooter;
    require(rowGroupFooter.ParseFromArray(
                    canonical.data() + 352, 154),
            "unable to parse VARCHAR fixture RowGroupFooter");
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse VARCHAR fixture FileTail");

    std::vector<std::uint8_t> columnData = {
            'a', 'c', 'c', 'c', 'd', 'e', 'e', 'f', 'g', 'g',
            0x02, 0x04, 0x01};
    const std::uint32_t starts[] = {0, 1, 1, 4, 5, 7, 8, 10};
    for (const std::uint32_t start : starts)
    {
        appendLittleUint32(columnData, start);
    }
    appendLittleUint32(columnData, 13);

    pixels::proto::ColumnChunkIndex *chunk =
            rowGroupFooter.mutable_rowgroupindexentry()
                    ->mutable_columnchunkindexentries(0);
    chunk->set_chunkoffset(0);
    chunk->set_chunklength(
            static_cast<std::uint32_t>(columnData.size()));
    chunk->set_isnulloffset(10);
    chunk->clear_pixelpositions();
    chunk->add_pixelpositions(0);
    chunk->add_pixelpositions(4);
    chunk->add_pixelpositions(8);
    chunk->clear_pixelstatistics();
    for (std::uint32_t pixel = 0; pixel < 3; ++pixel)
    {
        pixels::proto::ColumnStatistic *statistic =
                chunk->add_pixelstatistics()->mutable_statistic();
        statistic->set_numberofvalues(pixel == 2 ? 2 : 4);
        statistic->set_hasnull(true);
    }
    chunk->set_littleendian(true);
    chunk->set_nullspadding(false);
    rowGroupFooter.mutable_rowgroupencoding()
            ->mutable_columnchunkencodings(0)
            ->set_kind(pixels::proto::ColumnEncoding_Kind_NONE);

    pixels::proto::Type *type =
            fileTail.mutable_footer()->mutable_types(0);
    type->set_kind(pixels::proto::Type_Kind_VARCHAR);
    type->set_name("name");
    type->set_maximumlength(16);
    pixels::proto::RowGroupInformation *rowGroup =
            fileTail.mutable_footer()->mutable_rowgroupinfos(0);
    rowGroup->set_footeroffset(352);
    rowGroup->set_numberofrows(10);
    fileTail.mutable_postscript()->set_pixelstride(4);
    fileTail.mutable_postscript()->set_numberofrows(10);

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize VARCHAR fixture RowGroupFooter");
    rowGroup->set_footerlength(serializedFooter.size());
    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize VARCHAR fixture FileTail");

    std::vector<std::uint8_t> file(canonical.begin(),
                                   canonical.begin() + 352);
    std::copy(columnData.begin(), columnData.end(), file.begin());
    file.insert(file.end(), serializedFooter.begin(),
                serializedFooter.end());
    const std::uint64_t tailOffset = file.size();
    file.insert(file.end(), serializedTail.begin(), serializedTail.end());
    for (int byte = 7; byte >= 0; --byte)
    {
        file.push_back(static_cast<std::uint8_t>(
                tailOffset >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
    return file;
}

std::vector<std::uint8_t> makeBinaryFixture()
{
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::RowGroupFooter rowGroupFooter;
    require(rowGroupFooter.ParseFromArray(
                    canonical.data() + 352, 154),
            "unable to parse binary fixture RowGroupFooter");
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse binary fixture FileTail");

    std::vector<std::uint8_t> columnData = {
            2, 0x00, 0xFF,
            0,
            1, 1,
            1, 2,
            1, 3,
            1, 4,
            1, 5,
            1, 6,
            1, 7,
            0x02, 0x00};
    pixels::proto::ColumnChunkIndex *chunk =
            rowGroupFooter.mutable_rowgroupindexentry()
                    ->mutable_columnchunkindexentries(0);
    chunk->set_chunkoffset(0);
    chunk->set_chunklength(
            static_cast<std::uint32_t>(columnData.size()));
    chunk->set_isnulloffset(18);
    chunk->clear_pixelpositions();
    chunk->add_pixelpositions(0);
    chunk->clear_pixelstatistics();
    pixels::proto::ColumnStatistic *statistic =
            chunk->add_pixelstatistics()->mutable_statistic();
    statistic->set_numberofvalues(10);
    statistic->set_hasnull(true);
    chunk->set_littleendian(true);
    chunk->set_nullspadding(true);
    rowGroupFooter.mutable_rowgroupencoding()
            ->mutable_columnchunkencodings(0)
            ->set_kind(pixels::proto::ColumnEncoding_Kind_NONE);

    pixels::proto::Type *type =
            fileTail.mutable_footer()->mutable_types(0);
    type->set_kind(pixels::proto::Type_Kind_VARBINARY);
    type->set_name("payload");
    type->set_maximumlength(7);
    pixels::proto::RowGroupInformation *rowGroup =
            fileTail.mutable_footer()->mutable_rowgroupinfos(0);
    rowGroup->set_footeroffset(352);
    rowGroup->set_numberofrows(10);
    fileTail.mutable_postscript()->set_pixelstride(10);
    fileTail.mutable_postscript()->set_numberofrows(10);

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize binary fixture RowGroupFooter");
    rowGroup->set_footerlength(serializedFooter.size());
    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize binary fixture FileTail");

    std::vector<std::uint8_t> file(canonical.begin(),
                                   canonical.begin() + 352);
    std::copy(columnData.begin(), columnData.end(), file.begin());
    file.insert(file.end(), serializedFooter.begin(),
                serializedFooter.end());
    const std::uint64_t tailOffset = file.size();
    file.insert(file.end(), serializedTail.begin(), serializedTail.end());
    for (int byte = 7; byte >= 0; --byte)
    {
        file.push_back(static_cast<std::uint8_t>(
                tailOffset >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
    return file;
}

std::vector<std::uint8_t> makeVectorFixture()
{
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::RowGroupFooter rowGroupFooter;
    require(rowGroupFooter.ParseFromArray(
                    canonical.data() + 352, 154),
            "unable to parse VECTOR fixture RowGroupFooter");
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse VECTOR fixture FileTail");

    std::vector<std::uint8_t> columnData;
    for (std::uint32_t row = 0; row < 10; ++row)
    {
        if (row == 2)
        {
            appendBigDouble(
                    columnData,
                    std::numeric_limits<double>::quiet_NaN());
            appendBigDouble(
                    columnData,
                    std::numeric_limits<double>::infinity());
        }
        else
        {
            appendBigDouble(columnData, row * 2.0 + 1.0);
            appendBigDouble(columnData, row * 2.0 + 2.0);
        }
    }
    pixels::proto::ColumnChunkIndex *chunk =
            rowGroupFooter.mutable_rowgroupindexentry()
                    ->mutable_columnchunkindexentries(0);
    chunk->set_chunkoffset(0);
    chunk->set_chunklength(
            static_cast<std::uint32_t>(columnData.size()));
    chunk->set_isnulloffset(
            static_cast<std::uint32_t>(columnData.size()));
    chunk->clear_pixelpositions();
    chunk->add_pixelpositions(0);
    chunk->clear_pixelstatistics();
    pixels::proto::ColumnStatistic *statistic =
            chunk->add_pixelstatistics()->mutable_statistic();
    statistic->set_numberofvalues(10);
    statistic->set_hasnull(false);
    chunk->set_littleendian(false);
    chunk->set_nullspadding(false);
    rowGroupFooter.mutable_rowgroupencoding()
            ->mutable_columnchunkencodings(0)
            ->set_kind(pixels::proto::ColumnEncoding_Kind_NONE);

    pixels::proto::Type *type =
            fileTail.mutable_footer()->mutable_types(0);
    type->set_kind(pixels::proto::Type_Kind_VECTOR);
    type->set_name("embedding");
    type->set_dimension(2);
    pixels::proto::RowGroupInformation *rowGroup =
            fileTail.mutable_footer()->mutable_rowgroupinfos(0);
    rowGroup->set_footeroffset(352);
    rowGroup->set_numberofrows(10);
    fileTail.mutable_postscript()->set_pixelstride(10);
    fileTail.mutable_postscript()->set_numberofrows(10);

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize VECTOR fixture RowGroupFooter");
    rowGroup->set_footerlength(serializedFooter.size());
    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize VECTOR fixture FileTail");

    std::vector<std::uint8_t> file(canonical.begin(),
                                   canonical.begin() + 352);
    std::copy(columnData.begin(), columnData.end(), file.begin());
    file.insert(file.end(), serializedFooter.begin(),
                serializedFooter.end());
    const std::uint64_t tailOffset = file.size();
    file.insert(file.end(), serializedTail.begin(), serializedTail.end());
    for (int byte = 7; byte >= 0; --byte)
    {
        file.push_back(static_cast<std::uint8_t>(
                tailOffset >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
    return file;
}

std::vector<std::uint8_t> makeDictionaryFixture(bool cascadeRle)
{
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::RowGroupFooter rowGroupFooter;
    require(rowGroupFooter.ParseFromArray(
                    canonical.data() + 352, 154),
            "unable to parse dictionary fixture RowGroupFooter");
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse dictionary fixture FileTail");

    std::vector<std::uint8_t> columnData;
    if (cascadeRle)
    {
        appendDirectRleUint32(
                columnData, {1, 2, 1, 2, 1, 2, 1, 2, 1});
    }
    else
    {
        const std::uint32_t ids[] = {
                1, 0, 2, 1, 2, 1, 2, 1, 2, 1};
        for (const std::uint32_t id : ids)
        {
            appendLittleUint32(columnData, id);
        }
    }
    const std::uint32_t nullOffset =
            static_cast<std::uint32_t>(columnData.size());
    columnData.push_back(0x02);
    columnData.push_back(0x00);
    const std::uint32_t dictionaryContentOffset =
            static_cast<std::uint32_t>(columnData.size());
    columnData.insert(
            columnData.end(),
            {'c', 'a', 't', 'd', 'o', 'g'});
    const std::uint32_t dictionaryStartsOffset =
            static_cast<std::uint32_t>(columnData.size());
    if (cascadeRle)
    {
        appendDirectRleUint32(columnData, {0, 0, 3, 6});
    }
    else
    {
        for (const std::uint32_t start :
             std::vector<std::uint32_t>{0, 0, 3, 6})
        {
            appendLittleUint32(columnData, start);
        }
    }
    appendLittleUint32(columnData, dictionaryContentOffset);
    appendLittleUint32(columnData, dictionaryStartsOffset);

    pixels::proto::ColumnChunkIndex *chunk =
            rowGroupFooter.mutable_rowgroupindexentry()
                    ->mutable_columnchunkindexentries(0);
    chunk->set_chunkoffset(0);
    chunk->set_chunklength(
            static_cast<std::uint32_t>(columnData.size()));
    chunk->set_isnulloffset(nullOffset);
    chunk->clear_pixelpositions();
    chunk->add_pixelpositions(0);
    chunk->clear_pixelstatistics();
    pixels::proto::ColumnStatistic *statistic =
            chunk->add_pixelstatistics()->mutable_statistic();
    statistic->set_numberofvalues(10);
    statistic->set_hasnull(true);
    chunk->set_littleendian(true);
    chunk->set_nullspadding(true);

    pixels::proto::ColumnEncoding *encoding =
            rowGroupFooter.mutable_rowgroupencoding()
                    ->mutable_columnchunkencodings(0);
    encoding->set_kind(
            pixels::proto::ColumnEncoding_Kind_DICTIONARY);
    encoding->set_dictionarysize(3);
    encoding->clear_cascadeencoding();
    if (cascadeRle)
    {
        encoding->mutable_cascadeencoding()->set_kind(
                pixels::proto::ColumnEncoding_Kind_RUNLENGTH);
    }

    pixels::proto::Type *type =
            fileTail.mutable_footer()->mutable_types(0);
    type->set_kind(pixels::proto::Type_Kind_VARCHAR);
    type->set_name("animal");
    type->set_maximumlength(8);
    pixels::proto::RowGroupInformation *rowGroup =
            fileTail.mutable_footer()->mutable_rowgroupinfos(0);
    rowGroup->set_footeroffset(352);
    rowGroup->set_numberofrows(10);
    fileTail.mutable_postscript()->set_pixelstride(10);
    fileTail.mutable_postscript()->set_numberofrows(10);

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize dictionary fixture RowGroupFooter");
    rowGroup->set_footerlength(serializedFooter.size());
    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize dictionary fixture FileTail");

    std::vector<std::uint8_t> file(canonical.begin(),
                                   canonical.begin() + 352);
    std::copy(columnData.begin(), columnData.end(), file.begin());
    file.insert(file.end(), serializedFooter.begin(),
                serializedFooter.end());
    const std::uint64_t tailOffset = file.size();
    file.insert(file.end(), serializedTail.begin(), serializedTail.end());
    for (int byte = 7; byte >= 0; --byte)
    {
        file.push_back(static_cast<std::uint8_t>(
                tailOffset >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
    return file;
}

std::vector<std::uint8_t> makeLongDecimalFixture()
{
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::RowGroupFooter rowGroupFooter;
    require(rowGroupFooter.ParseFromArray(
                    canonical.data() + 352, 154),
            "unable to parse long DECIMAL fixture RowGroupFooter");
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse long DECIMAL fixture FileTail");

    std::vector<std::uint8_t> columnData;
    appendLittleInt64(columnData, 0);
    appendLittleInt64(columnData, 123456);
    appendLittleInt64(columnData, -1);
    appendLittleInt64(columnData, -1);
    for (std::uint32_t row = 2; row < 10; ++row)
    {
        appendLittleInt64(columnData, 0);
        appendLittleInt64(columnData, 0);
    }
    pixels::proto::ColumnChunkIndex *chunk =
            rowGroupFooter.mutable_rowgroupindexentry()
                    ->mutable_columnchunkindexentries(0);
    chunk->set_chunkoffset(0);
    chunk->set_chunklength(
            static_cast<std::uint32_t>(columnData.size()));
    chunk->set_isnulloffset(
            static_cast<std::uint32_t>(columnData.size()));
    chunk->clear_pixelpositions();
    chunk->add_pixelpositions(0);
    chunk->clear_pixelstatistics();
    pixels::proto::ColumnStatistic *statistic =
            chunk->add_pixelstatistics()->mutable_statistic();
    statistic->set_numberofvalues(10);
    statistic->set_hasnull(false);
    chunk->set_littleendian(true);
    chunk->set_nullspadding(false);
    rowGroupFooter.mutable_rowgroupencoding()
            ->mutable_columnchunkencodings(0)
            ->set_kind(pixels::proto::ColumnEncoding_Kind_NONE);

    pixels::proto::Type *type =
            fileTail.mutable_footer()->mutable_types(0);
    type->set_kind(pixels::proto::Type_Kind_DECIMAL);
    type->set_name("amount");
    type->set_precision(38);
    type->set_scale(4);
    pixels::proto::RowGroupInformation *rowGroup =
            fileTail.mutable_footer()->mutable_rowgroupinfos(0);
    rowGroup->set_footeroffset(352);
    rowGroup->set_numberofrows(10);
    fileTail.mutable_postscript()->set_pixelstride(10);
    fileTail.mutable_postscript()->set_numberofrows(10);

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize long DECIMAL fixture RowGroupFooter");
    rowGroup->set_footerlength(serializedFooter.size());
    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize long DECIMAL fixture FileTail");

    std::vector<std::uint8_t> file(canonical.begin(),
                                   canonical.begin() + 352);
    std::copy(columnData.begin(), columnData.end(), file.begin());
    file.insert(file.end(), serializedFooter.begin(),
                serializedFooter.end());
    const std::uint64_t tailOffset = file.size();
    file.insert(file.end(), serializedTail.begin(), serializedTail.end());
    for (int byte = 7; byte >= 0; --byte)
    {
        file.push_back(static_cast<std::uint8_t>(
                tailOffset >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
    return file;
}

std::vector<std::uint8_t> makeByteRleFixture()
{
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::RowGroupFooter rowGroupFooter;
    require(rowGroupFooter.ParseFromArray(
                    canonical.data() + 352, 154),
            "unable to parse BYTE RLE fixture RowGroupFooter");
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse BYTE RLE fixture FileTail");

    std::vector<std::uint8_t> columnData = {
            0x00, 0x01,
            0xFA, 0xFE, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x02, 0x00};
    pixels::proto::ColumnChunkIndex *chunk =
            rowGroupFooter.mutable_rowgroupindexentry()
                    ->mutable_columnchunkindexentries(0);
    chunk->set_chunkoffset(0);
    chunk->set_chunklength(
            static_cast<std::uint32_t>(columnData.size()));
    chunk->set_isnulloffset(9);
    chunk->clear_pixelpositions();
    chunk->add_pixelpositions(0);
    chunk->clear_pixelstatistics();
    pixels::proto::ColumnStatistic *statistic =
            chunk->add_pixelstatistics()->mutable_statistic();
    statistic->set_numberofvalues(10);
    statistic->set_hasnull(true);
    chunk->set_littleendian(true);
    chunk->set_nullspadding(true);
    rowGroupFooter.mutable_rowgroupencoding()
            ->mutable_columnchunkencodings(0)
            ->set_kind(pixels::proto::ColumnEncoding_Kind_RUNLENGTH);

    pixels::proto::Type *type =
            fileTail.mutable_footer()->mutable_types(0);
    type->set_kind(pixels::proto::Type_Kind_BYTE);
    type->set_name("tiny");
    pixels::proto::RowGroupInformation *rowGroup =
            fileTail.mutable_footer()->mutable_rowgroupinfos(0);
    rowGroup->set_footeroffset(352);
    rowGroup->set_numberofrows(10);
    fileTail.mutable_postscript()->set_pixelstride(10);
    fileTail.mutable_postscript()->set_numberofrows(10);

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize BYTE RLE fixture RowGroupFooter");
    rowGroup->set_footerlength(serializedFooter.size());
    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize BYTE RLE fixture FileTail");

    std::vector<std::uint8_t> file(canonical.begin(),
                                   canonical.begin() + 352);
    std::copy(columnData.begin(), columnData.end(), file.begin());
    file.insert(file.end(), serializedFooter.begin(),
                serializedFooter.end());
    const std::uint64_t tailOffset = file.size();
    file.insert(file.end(), serializedTail.begin(), serializedTail.end());
    for (int byte = 7; byte >= 0; --byte)
    {
        file.push_back(static_cast<std::uint8_t>(
                tailOffset >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
    return file;
}

std::string readResult(const Session &session)
{
    std::uint64_t size = 0;
    require(pixels_inspector_result_size(session.handle(), &size)
            == PIXELS_INSPECTOR_OK,
            "result size is unavailable");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    require(pixels_inspector_copy_result(
                    session.handle(), bytes.data(), bytes.size())
            == PIXELS_INSPECTOR_OK,
            "unable to copy inspection result");
    return std::string(bytes.begin(), bytes.end());
}

std::string readError(const Session &session)
{
    std::uint64_t size = 0;
    require(pixels_inspector_error_size(session.handle(), &size)
            == PIXELS_INSPECTOR_OK,
            "error size is unavailable");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    require(pixels_inspector_copy_error(
                    session.handle(), bytes.data(), bytes.size())
            == PIXELS_INSPECTOR_OK,
            "unable to copy inspection error");
    return std::string(bytes.begin(), bytes.end());
}

void driveMetadata(Session &session,
                   const std::vector<std::uint8_t> &file)
{
    require(pixels_inspector_begin_metadata(session.handle())
            == PIXELS_INSPECTOR_RANGE_READY,
            "metadata did not request the tail pointer");

    const pixels::format::FileRange pointer = nextRange(session);
    require(pointer.offset == 782 && pointer.length == 8,
            "unexpected tail-pointer range");
    require(supply(session, pointer, file) == PIXELS_INSPECTOR_RANGE_READY,
            "tail pointer did not produce a FileTail request");

    const pixels::format::FileRange tail = nextRange(session);
    require(tail.offset == 506 && tail.length == 276,
            "unexpected FileTail range");
    require(supply(session, tail, file) == PIXELS_INSPECTOR_RESULT_READY,
            "FileTail did not produce metadata");
}

void driveMetadataFlexible(
        Session &session, const std::vector<std::uint8_t> &file)
{
    require(pixels_inspector_begin_metadata(session.handle())
            == PIXELS_INSPECTOR_RANGE_READY,
            "metadata did not request the tail pointer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "tail pointer did not produce a FileTail request");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "FileTail did not produce metadata");
}

void testCanonicalFixture()
{
    const std::vector<std::uint8_t> file = readFixture();
    require(file.size() == 790, "canonical fixture length changed");

    Session session(file.size());
    driveMetadata(session, file);
    require(readResult(session) == EXPECTED_METADATA,
            "canonical metadata differs from the golden");

    require(pixels_inspector_begin_plain_long_page(
                    session.handle(), 0, 0, 0, 10)
            == PIXELS_INSPECTOR_RANGE_READY,
            "page did not request its row-group footer");
    const pixels::format::FileRange footer = nextRange(session);
    require(footer.offset == 352 && footer.length == 154,
            "unexpected row-group footer range");
    require(supply(session, footer, file) == PIXELS_INSPECTOR_RANGE_READY,
            "row-group footer did not produce a chunk request");

    const pixels::format::FileRange chunk = nextRange(session);
    require(chunk.offset == 0 && chunk.length == 80,
            "unexpected column chunk range");
    require(supply(session, chunk, file) == PIXELS_INSPECTOR_RESULT_READY,
            "column chunk did not produce a page");
    require(readResult(session) == EXPECTED_PAGE,
            "canonical page differs from the golden");
}

void testBoundedPageRange()
{
    const std::vector<std::uint8_t> file = readFixture();
    Session session(file.size());
    driveMetadata(session, file);

    require(pixels_inspector_begin_plain_long_page(
                    session.handle(), 0, 0, 2, 3)
            == PIXELS_INSPECTOR_RANGE_READY,
            "bounded page did not request its row-group footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "row-group footer did not produce a bounded page request");
    const pixels::format::FileRange page = nextRange(session);
    require(page.offset == 16 && page.length == 24,
            "page request was not reduced to the requested values");
    require(supply(session, page, file) == PIXELS_INSPECTOR_RESULT_READY,
            "bounded page bytes did not produce a result");
    require(readResult(session)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":2,\"count\":3,"
               "\"values\":[\"2\",\"3\",\"4\"]}",
            "bounded page result differs from the golden");
}

void testGenericPlainScalarPages()
{
    const std::vector<std::uint8_t> file = readFixture();
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 2, 1, 3)
                == PIXELS_INSPECTOR_RANGE_READY,
                "DATE page did not request its row-group footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "DATE footer did not produce a value range");
        const pixels::format::FileRange range = nextRange(session);
        require(range.offset == 196 && range.length == 12,
                "DATE page range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "DATE bytes did not produce a page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":2,\"offset\":1,\"count\":3,"
                   "\"values\":[\"10227\",\"10207\",\"11208\"]}",
                "DATE page differs from the golden");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 3, 0, 4)
                == PIXELS_INSPECTOR_RANGE_READY,
                "DECIMAL page did not request its row-group footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "DECIMAL footer did not produce a value range");
        const pixels::format::FileRange range = nextRange(session);
        require(range.offset == 256 && range.length == 32,
                "DECIMAL page range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "DECIMAL bytes did not produce a page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":3,\"offset\":0,\"count\":4,"
                   "\"values\":[\"90.60\",\"10.10\",\"20.40\",\"54.60\"]}",
                "DECIMAL page differs from the golden");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 1, 8, 2)
                == PIXELS_INSPECTOR_RANGE_READY,
                "VARCHAR page did not request its row-group footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "VARCHAR footer did not request its layout trailer");
        pixels::format::FileRange range = nextRange(session);
        require(range.offset == 187 && range.length == 4,
                "VARCHAR trailer range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "VARCHAR trailer did not request starts");
        range = nextRange(session);
        require(range.offset == 175 && range.length == 12,
                "VARCHAR starts range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "VARCHAR starts did not request content");
        range = nextRange(session);
        require(range.offset == 130 && range.length == 13,
                "VARCHAR content range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "VARCHAR content did not produce a page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":1,\"offset\":8,\"count\":2,"
                   "\"values\":[\"Liangyong\",\"Eric\"]}",
                "VARCHAR page differs from the golden");
    }
    {
        std::vector<std::uint8_t> nullFile = file;
        nullFile[80] = 0x04;
        nullFile[81] = 0x00;
        mutateRowGroupFooter(
                nullFile, [](pixels::proto::RowGroupFooter &footer) {
                    pixels::proto::ColumnChunkIndex *chunk =
                            footer.mutable_rowgroupindexentry()
                                    ->mutable_columnchunkindexentries(0);
                    chunk->set_chunklength(82);
                    chunk->mutable_pixelstatistics(0)
                            ->mutable_statistic()
                            ->set_hasnull(true);
                });
        Session session(nullFile.size());
        driveMetadata(session, nullFile);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "null page did not request its row-group footer");
        require(supply(session, nextRange(session), nullFile)
                == PIXELS_INSPECTOR_RANGE_READY,
                "null page did not request its bitmap");
        const pixels::format::FileRange nullRange = nextRange(session);
        require(nullRange.offset == 80 && nullRange.length == 2,
                "null bitmap range is not exact");
        require(supply(session, nullRange, nullFile)
                == PIXELS_INSPECTOR_RANGE_READY,
                "null bitmap did not produce a value range");
        const pixels::format::FileRange valueRange = nextRange(session);
        require(valueRange.offset == 0 && valueRange.length == 72,
                "unpadded value range did not exclude the null");
        require(supply(session, valueRange, nullFile)
                == PIXELS_INSPECTOR_RESULT_READY,
                "unpadded values did not produce a page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":10,"
                   "\"values\":[\"0\",\"1\",null,\"2\",\"3\",\"4\",\"5\","
                   "\"6\",\"7\",\"8\"]}",
                "unpadded null page differs from the golden");
    }
}

void testMultiPixelPlainPages()
{
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(false, {0, 0, 0});
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 2, 6)
                == PIXELS_INSPECTOR_RANGE_READY,
                "legacy multi-pixel LONG page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "multi-pixel footer did not produce the first value range");
        pixels::format::FileRange range = nextRange(session);
        require(range.offset == 16 && range.length == 16,
                "first bounded pixel range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "first pixel did not continue to the second pixel");
        range = nextRange(session);
        require(range.offset == 32 && range.length == 32,
                "second bounded pixel range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "second pixel did not complete the page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":2,\"count\":6,"
                   "\"values\":[\"2\",\"3\",\"4\",\"5\",\"6\",\"7\"]}",
                "null-free multi-pixel page differs from the golden");
    }
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(false, {0x02, 0, 0x01});
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "unpadded multi-pixel page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "unpadded footer did not request the first bitmap");
        pixels::format::FileRange range = nextRange(session);
        require(range.offset == 64 && range.length == 1,
                "first unpadded null bitmap range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "first bitmap did not request its values");
        range = nextRange(session);
        require(range.offset == 0 && range.length == 24,
                "first unpadded value range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "first values did not continue to the null-free pixel");
        range = nextRange(session);
        require(range.offset == 24 && range.length == 32,
                "middle unpadded value range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "middle values did not request the final bitmap");
        range = nextRange(session);
        require(range.offset == 65 && range.length == 1,
                "final unpadded null bitmap range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "final bitmap did not request its values");
        range = nextRange(session);
        require(range.offset == 56 && range.length == 8,
                "final unpadded value range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "final unpadded values did not complete the page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":10,"
                   "\"values\":[\"0\",null,\"2\",\"3\",\"4\",\"5\",\"6\","
                   "\"7\",null,\"9\"]}",
                "unpadded multi-pixel page differs from the golden");
    }
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(true, {0x02, 0, 0x01});
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "padded multi-pixel page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "padded footer did not request the first bitmap");
        pixels::format::FileRange range = nextRange(session);
        require(range.offset == 80 && range.length == 1,
                "first padded null bitmap range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "first padded bitmap did not request values");
        range = nextRange(session);
        require(range.offset == 0 && range.length == 32,
                "first padded value range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "first padded values did not continue");
        range = nextRange(session);
        require(range.offset == 32 && range.length == 32,
                "middle padded value range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "middle padded values did not request the final bitmap");
        range = nextRange(session);
        require(range.offset == 81 && range.length == 1,
                "final padded null bitmap range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "final padded bitmap did not request values");
        range = nextRange(session);
        require(range.offset == 64 && range.length == 16,
                "final padded value range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "final padded values did not complete the page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":10,"
                   "\"values\":[\"0\",null,\"2\",\"3\",\"4\",\"5\",\"6\","
                   "\"7\",null,\"9\"]}",
                "padded multi-pixel page differs from the golden");
    }
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(false, {0x02, 0, 0x01});
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 8, 2)
                == PIXELS_INSPECTOR_RANGE_READY,
                "late-pixel page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "late-pixel footer did not request its bitmap");
        pixels::format::FileRange range = nextRange(session);
        require(range.offset == 65 && range.length == 1,
                "late-pixel bitmap did not skip the earlier bitmap");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "late-pixel bitmap did not request values");
        range = nextRange(session);
        require(range.offset == 56 && range.length == 8,
                "late-pixel value range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "late-pixel values did not complete the page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":8,\"count\":2,"
                   "\"values\":[null,\"9\"]}",
                "late-pixel page differs from the golden");
    }
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(false, {0x0F, 0, 0});
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 6)
                == PIXELS_INSPECTOR_RANGE_READY,
                "all-null pixel page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "all-null pixel footer did not request its bitmap");
        pixels::format::FileRange range = nextRange(session);
        require(range.offset == 48 && range.length == 1,
                "all-null pixel bitmap range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "all-null pixel did not advance without a value range");
        range = nextRange(session);
        require(range.offset == 0 && range.length == 16,
                "value range after the all-null pixel is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "values after the all-null pixel did not complete the page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":6,"
                   "\"values\":[null,null,null,null,\"4\",\"5\"]}",
                "all-null pixel page differs from the golden");
    }
}

void testMultiPixelRunLengthPages()
{
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(
                        true, {0x02, 0, 0x01},
                        std::function<void(
                                pixels::proto::RowGroupFooter &)>(), true);
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 2, 7)
                == PIXELS_INSPECTOR_RANGE_READY,
                "RLE multi-pixel page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "RLE footer did not request the first bitmap");
        pixels::format::FileRange range = nextRange(session);
        require(range.offset == 11 && range.length == 1,
                "RLE first bitmap range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "RLE first bitmap did not request its full pixel");
        range = nextRange(session);
        require(range.offset == 0 && range.length == 4,
                "RLE first pixel range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "RLE first pixel did not continue to the second pixel");
        range = nextRange(session);
        require(range.offset == 4 && range.length == 4,
                "RLE second pixel range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "RLE second pixel did not request the final bitmap");
        range = nextRange(session);
        require(range.offset == 12 && range.length == 1,
                "RLE final bitmap range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "all-null RLE page tail did not finish without content");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":2,\"count\":7,"
                   "\"values\":[\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",null]}",
                "RLE multi-pixel page differs from the golden");
    }
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(
                        false, {0, 0, 0},
                        [](pixels::proto::RowGroupFooter &footer) {
                            footer.mutable_rowgroupindexentry()
                                    ->mutable_columnchunkindexentries(0)
                                    ->set_pixelpositions(1, 3);
                        },
                        true);
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 4)
                == PIXELS_INSPECTOR_RANGE_READY,
                "truncated RLE page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "truncated RLE footer did not request content");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "truncated RLE pixel was not rejected");
    }
}

void testMultiPixelVariablePages()
{
    const std::vector<std::uint8_t> file =
            makeMultiPixelVarcharFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    require(pixels_inspector_begin_page(
                    session.handle(), 0, 0, 4, 6)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR multi-pixel page did not request its footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR footer did not request its trailer");
    pixels::format::FileRange range = nextRange(session);
    require(range.offset == 45 && range.length == 4,
            "VARCHAR fixture trailer range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR trailer did not request the prefix bitmap");
    range = nextRange(session);
    require(range.offset == 10 && range.length == 1,
            "VARCHAR prefix bitmap range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR prefix bitmap did not request the current bitmap");
    range = nextRange(session);
    require(range.offset == 11 && range.length == 1,
            "VARCHAR current bitmap range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR bitmap did not request starts");
    range = nextRange(session);
    require(range.offset == 25 && range.length == 16,
            "VARCHAR first starts range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR starts did not request content");
    range = nextRange(session);
    require(range.offset == 4 && range.length == 4,
            "VARCHAR first content range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR first content did not request final bitmap");
    range = nextRange(session);
    require(range.offset == 12 && range.length == 1,
            "VARCHAR final bitmap range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR final bitmap did not request starts");
    range = nextRange(session);
    require(range.offset == 37 && range.length == 8,
            "VARCHAR final starts range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARCHAR final starts did not request content");
    range = nextRange(session);
    require(range.offset == 8 && range.length == 2,
            "VARCHAR final content range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "VARCHAR final content did not finish the page");
    require(readResult(session)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":4,\"count\":6,"
               "\"values\":[\"d\",\"ee\",null,\"f\",null,\"gg\"]}",
            "VARCHAR multi-pixel page differs from the golden");
}

void testBinaryPage()
{
    const std::vector<std::uint8_t> file = makeBinaryFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    require(pixels_inspector_begin_page(
                    session.handle(), 0, 0, 0, 4)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARBINARY page did not request its footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARBINARY footer did not request its bitmap");
    pixels::format::FileRange range = nextRange(session);
    require(range.offset == 18 && range.length == 2,
            "VARBINARY bitmap range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VARBINARY bitmap did not request its pixel");
    range = nextRange(session);
    require(range.offset == 0 && range.length == 18,
            "VARBINARY pixel range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "VARBINARY pixel did not produce a page");
    require(readResult(session)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":4,"
               "\"values\":[\"AP8=\",null,\"\",\"AQ==\"]}",
            "VARBINARY page differs from the golden");
}

void testVectorPage()
{
    const std::vector<std::uint8_t> file = makeVectorFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    require(pixels_inspector_begin_page(
                    session.handle(), 0, 0, 1, 2)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VECTOR page did not request its footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "VECTOR footer did not request its values");
    const pixels::format::FileRange range = nextRange(session);
    require(range.offset == 16 && range.length == 32,
            "VECTOR value range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "VECTOR bytes did not produce a page");
    require(readResult(session)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":1,\"count\":2,"
               "\"values\":[[3,4],[\"NaN\",\"Infinity\"]]}",
            "VECTOR page differs from the golden");
}

void testDictionaryPages()
{
    for (const bool cascadeRle : {false, true})
    {
        const std::vector<std::uint8_t> file =
                makeDictionaryFixture(cascadeRle);
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 4)
                == PIXELS_INSPECTOR_RANGE_READY,
                "dictionary page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "dictionary footer did not request its trailer");
        pixels::format::FileRange range = nextRange(session);
        require(range.offset == (cascadeRle ? 17U : 64U)
                && range.length == 8,
                "dictionary trailer range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "dictionary trailer did not request starts");
        range = nextRange(session);
        require(range.offset == (cascadeRle ? 13U : 48U)
                && range.length == (cascadeRle ? 4U : 16U),
                "dictionary starts range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "dictionary starts did not request content");
        range = nextRange(session);
        require(range.offset == (cascadeRle ? 7U : 42U)
                && range.length == 6,
                "dictionary content range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "dictionary content did not request the bitmap");
        range = nextRange(session);
        require(range.offset == (cascadeRle ? 5U : 40U)
                && range.length == 2,
                "dictionary bitmap range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "dictionary bitmap did not request IDs");
        range = nextRange(session);
        require(range.offset == 0
                && range.length == (cascadeRle ? 5U : 40U),
                "dictionary ID range is not exact");
        require(supply(session, range, file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "dictionary IDs did not produce a page");
        require(readResult(session)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":4,"
                   "\"values\":[\"cat\",null,\"dog\",\"cat\"]}",
                "dictionary page differs from the golden");
    }
}

void testLongDecimalPage()
{
    const std::vector<std::uint8_t> file =
            makeLongDecimalFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    require(pixels_inspector_begin_page(
                    session.handle(), 0, 0, 0, 2)
            == PIXELS_INSPECTOR_RANGE_READY,
            "long DECIMAL page did not request its footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "long DECIMAL footer did not request values");
    const pixels::format::FileRange range = nextRange(session);
    require(range.offset == 0 && range.length == 32,
            "long DECIMAL value range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "long DECIMAL bytes did not produce a page");
    require(readResult(session)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":2,"
               "\"values\":[\"12.3456\",\"-0.0001\"]}",
            "long DECIMAL page differs from the golden");
}

void testByteRlePage()
{
    const std::vector<std::uint8_t> file =
            makeByteRleFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    require(pixels_inspector_begin_page(
                    session.handle(), 0, 0, 0, 5)
            == PIXELS_INSPECTOR_RANGE_READY,
            "BYTE RLE page did not request its footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "BYTE RLE footer did not request its bitmap");
    pixels::format::FileRange range = nextRange(session);
    require(range.offset == 9 && range.length == 2,
            "BYTE RLE bitmap range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "BYTE RLE bitmap did not request content");
    range = nextRange(session);
    require(range.offset == 0 && range.length == 9,
            "BYTE RLE content range is not exact");
    require(supply(session, range, file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "BYTE RLE content did not produce a page");
    require(readResult(session)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":5,"
               "\"values\":[\"1\",null,\"1\",\"1\",\"-2\"]}",
            "BYTE RLE page differs from the golden");
}

void testMultiPixelMalformedMetadata()
{
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(
                        false, {0, 0, 0},
                        [](pixels::proto::RowGroupFooter &footer) {
                            footer.mutable_rowgroupindexentry()
                                    ->mutable_columnchunkindexentries(0)
                                    ->set_pixelpositions(1, 8);
                        });
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 4)
                == PIXELS_INSPECTOR_RANGE_READY,
                "short-pixel fixture did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "value range crossing its pixel was not rejected");
    }
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(
                        false, {0x02, 0, 0x01},
                        [](pixels::proto::RowGroupFooter &footer) {
                            pixels::proto::ColumnChunkIndex *chunk =
                                    footer.mutable_rowgroupindexentry()
                                            ->mutable_columnchunkindexentries(0);
                            chunk->set_chunklength(
                                    chunk->isnulloffset() + 1U);
                        });
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "short-bitmap fixture did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "truncated per-pixel null bitmaps were not rejected");
    }
    {
        const std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(
                        false, {0, 0, 0},
                        [](pixels::proto::RowGroupFooter &footer) {
                            footer.mutable_rowgroupindexentry()
                                    ->mutable_columnchunkindexentries(0)
                                    ->mutable_pixelstatistics(1)
                                    ->mutable_statistic()
                                    ->clear_hasnull();
                        });
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "missing-statistics fixture did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_MALFORMED_PROTOBUF,
                "missing per-pixel null statistics were not rejected");
    }
}

void testPlainLongDecoder()
{
    const std::uint8_t littleEndian[] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
    std::int64_t values[3] = {};
    pixels::format::FormatError error;
    require(pixels::format::PlainLongDecoder::decode(
                    pixels::format::ByteSpan(
                            littleEndian, sizeof(littleEndian)),
                    true, 0, 3, values, 3, error),
            "little-endian LONG decode failed");
    require(values[0] == 0 && values[1] == -1
            && values[2] == std::numeric_limits<std::int64_t>::min(),
            "little-endian LONG values differ");

    const std::uint8_t bigEndian[] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xD6};
    require(pixels::format::PlainLongDecoder::decode(
                    pixels::format::ByteSpan(
                            bigEndian, sizeof(bigEndian)),
                    false, 0, 2, values, 3, error),
            "big-endian LONG decode failed");
    require(values[0] == 42 && values[1] == -42,
            "big-endian LONG values differ");

    require(!pixels::format::PlainLongDecoder::decode(
                    pixels::format::ByteSpan(
                            bigEndian, sizeof(bigEndian)),
                    false, 1, 2, values, 3, error)
            && error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
            "out-of-bounds LONG range was not rejected");
    require(!pixels::format::PlainLongDecoder::decode(
                    pixels::format::ByteSpan(
                            bigEndian, sizeof(bigEndian)),
                    false, 0, 2, values, 1, error)
            && error.code == pixels::format::ErrorCode::BUFFER_TOO_SMALL,
            "small LONG destination was not rejected");
}

void testPlainScalarDecoder()
{
    pixels::format::FormatError error;

    const std::uint8_t littleBits[] = {0x4D, 0x02};
    bool booleans[7] = {};
    require(pixels::format::PlainScalarDecoder::decodeBoolean(
                    pixels::format::ByteSpan(
                            littleBits, sizeof(littleBits)),
                    true, 1, 7, booleans, 7, error),
            "little-endian BOOLEAN decode failed");
    const bool expectedLittle[] = {
            false, true, true, false, false, true, false};
    require(std::equal(
                    booleans, booleans + 7, expectedLittle),
            "little-endian BOOLEAN values differ");

    const std::uint8_t bigBits[] = {0xB2};
    require(pixels::format::PlainScalarDecoder::decodeBoolean(
                    pixels::format::ByteSpan(bigBits, sizeof(bigBits)),
                    false, 0, 8, booleans, 7, error)
            == false
            && error.code == pixels::format::ErrorCode::BUFFER_TOO_SMALL,
            "small BOOLEAN destination was not rejected");
    bool bigBooleans[8] = {};
    const bool expectedBig[] = {
            true, false, true, true, false, false, true, false};
    require(pixels::format::PlainScalarDecoder::decodeBoolean(
                    pixels::format::ByteSpan(bigBits, sizeof(bigBits)),
                    false, 0, 8, bigBooleans, 8, error)
            && std::equal(
                    bigBooleans, bigBooleans + 8, expectedBig),
            "big-endian BOOLEAN values differ");

    const std::uint8_t bytes[] = {0x80, 0x00, 0x7F};
    std::int8_t byteValues[3] = {};
    require(pixels::format::PlainScalarDecoder::decodeByte(
                    pixels::format::ByteSpan(bytes, sizeof(bytes)),
                    0, 3, byteValues, 3, error)
            && byteValues[0] == std::numeric_limits<std::int8_t>::min()
            && byteValues[1] == 0
            && byteValues[2] == std::numeric_limits<std::int8_t>::max(),
            "BYTE boundary values differ");

    const std::uint8_t int32Little[] = {
            0x00, 0x00, 0x00, 0x80,
            0xFF, 0xFF, 0xFF, 0x7F};
    std::int32_t int32Values[2] = {};
    require(pixels::format::PlainScalarDecoder::decodeInt32(
                    pixels::format::ByteSpan(
                            int32Little, sizeof(int32Little)),
                    true, 0, 2, int32Values, 2, error)
            && int32Values[0] == std::numeric_limits<std::int32_t>::min()
            && int32Values[1] == std::numeric_limits<std::int32_t>::max(),
            "little-endian INT32 boundary values differ");

    const std::uint8_t int32Big[] = {
            0xFF, 0xFF, 0xFF, 0xD6,
            0x00, 0x00, 0x00, 0x2A};
    require(pixels::format::PlainScalarDecoder::decodeInt32(
                    pixels::format::ByteSpan(
                            int32Big, sizeof(int32Big)),
                    false, 0, 2, int32Values, 2, error)
            && int32Values[0] == -42 && int32Values[1] == 42,
            "big-endian INT32 values differ");

    const std::uint8_t floatLittle[] = {
            0x00, 0x00, 0xC0, 0x3F,
            0x00, 0x00, 0x80, 0x7F};
    float floatValues[2] = {};
    require(pixels::format::PlainScalarDecoder::decodeFloat(
                    pixels::format::ByteSpan(
                            floatLittle, sizeof(floatLittle)),
                    true, 0, 2, floatValues, 2, error)
            && floatValues[0] == 1.5F
            && std::isinf(floatValues[1]) && floatValues[1] > 0,
            "FLOAT values differ");

    const std::uint8_t doubleBig[] = {
            0xC0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x7F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    double doubleValues[2] = {};
    require(pixels::format::PlainScalarDecoder::decodeDouble(
                    pixels::format::ByteSpan(
                            doubleBig, sizeof(doubleBig)),
                    false, 0, 2, doubleValues, 2, error)
            && doubleValues[0] == -2.5
            && std::isnan(doubleValues[1]),
            "DOUBLE values differ");

    const std::uint8_t decimalWords[] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    pixels::format::Int128Words decimal[1];
    require(pixels::format::PlainScalarDecoder::decodeInt128(
                    pixels::format::ByteSpan(
                            decimalWords, sizeof(decimalWords)),
                    true, 0, 1, decimal, 1, error)
            && decimal[0].high
               == std::numeric_limits<std::uint64_t>::max()
            && decimal[0].low == 42,
            "INT128 words differ");

    require(!pixels::format::PlainScalarDecoder::decodeDouble(
                    pixels::format::ByteSpan(
                            doubleBig, sizeof(doubleBig) - 1),
                    false, 0, 2, doubleValues, 2, error)
            && error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
            "truncated DOUBLE range was not rejected");
}

void testPlainPixelPlanner()
{
    pixels::format::FormatError error;
    pixels::format::PlainPixelPlan plan;
    bool validity[6] = {};

    require(pixels::format::PlainPixelPlanner::plan(
                    10, 3, 4, false, false, true,
                    pixels::format::ByteSpan(), validity, 6,
                    plan, error)
            && plan.physicalOffset == 3
            && plan.physicalCount == 4
            && plan.pixelPhysicalCount == 10
            && std::all_of(
                    validity, validity + 4,
                    [](bool value) { return value; }),
            "null-free pixel plan differs");

    const std::uint8_t littleNulls[] = {0x12, 0x01};
    const bool expected[] = {true, true, false, true, true, true};
    require(pixels::format::PlainPixelPlanner::plan(
                    10, 2, 6, true, false, true,
                    pixels::format::ByteSpan(
                            littleNulls, sizeof(littleNulls)),
                    validity, 6, plan, error)
            && plan.physicalOffset == 1
            && plan.physicalCount == 5
            && plan.pixelPhysicalCount == 7
            && std::equal(validity, validity + 6, expected),
            "unpadded little-endian null plan differs");

    require(pixels::format::PlainPixelPlanner::plan(
                    10, 2, 6, true, true, true,
                    pixels::format::ByteSpan(
                            littleNulls, sizeof(littleNulls)),
                    validity, 6, plan, error)
            && plan.physicalOffset == 2
            && plan.physicalCount == 6
            && plan.pixelPhysicalCount == 10
            && std::equal(validity, validity + 6, expected),
            "padded null plan differs");

    const std::uint8_t bigNulls[] = {0x48, 0x80};
    require(pixels::format::PlainPixelPlanner::plan(
                    10, 2, 6, true, false, false,
                    pixels::format::ByteSpan(
                            bigNulls, sizeof(bigNulls)),
                    validity, 6, plan, error)
            && plan.physicalOffset == 1
            && plan.physicalCount == 5
            && plan.pixelPhysicalCount == 7
            && std::equal(validity, validity + 6, expected),
            "unpadded big-endian null plan differs");

    require(!pixels::format::PlainPixelPlanner::plan(
                    10, 2, 6, true, false, true,
                    pixels::format::ByteSpan(littleNulls, 1),
                    validity, 6, plan, error)
            && error.code == pixels::format::ErrorCode::INVALID_ARGUMENT,
            "short null bitmap was not rejected");
    require(!pixels::format::PlainPixelPlanner::plan(
                    10, 8, 3, false, false, true,
                    pixels::format::ByteSpan(), validity, 6,
                    plan, error)
            && error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
            "pixel row overflow was not rejected");
}

void testLifecycleAndBuffers()
{
    const std::vector<std::uint8_t> file = readFixture();
    Session session(file.size());
    driveMetadata(session, file);

    std::uint64_t size = 0;
    require(pixels_inspector_result_size(session.handle(), &size)
            == PIXELS_INSPECTOR_OK,
            "result size unavailable");
    std::vector<std::uint8_t> tooSmall(
            static_cast<std::size_t>(size - 1));
    require(pixels_inspector_copy_result(
                    session.handle(), tooSmall.data(), tooSmall.size())
            == PIXELS_INSPECTOR_BUFFER_TOO_SMALL,
            "small output buffer was not rejected");

    const pixels_inspector_handle staleHandle = session.handle();
    session.destroy();
    require(pixels_inspector_begin_metadata(staleHandle)
            == PIXELS_INSPECTOR_INVALID_HANDLE,
            "destroyed handle was accepted");
    require(pixels_inspector_destroy(staleHandle)
            == PIXELS_INSPECTOR_INVALID_HANDLE,
            "duplicate destroy was accepted");
}

void testInvalidRangeAndTerminalState()
{
    const std::vector<std::uint8_t> file = readFixture();
    Session session(file.size());
    require(pixels_inspector_begin_metadata(session.handle())
            == PIXELS_INSPECTOR_RANGE_READY,
            "metadata did not start");
    const pixels::format::FileRange expected = nextRange(session);
    require(pixels_inspector_supply_range(
                    session.handle(), expected.offset - 1, expected.length,
                    file.data() + expected.offset)
            == PIXELS_INSPECTOR_INVALID_ARGUMENT,
            "mismatched range was not rejected");
    require(pixels_inspector_supply_range(
                    session.handle(), expected.offset, expected.length,
                    file.data() + expected.offset)
            == PIXELS_INSPECTOR_INVALID_STATE,
            "failed session did not reject another range");
}

void testPartialRange()
{
    const std::vector<std::uint8_t> file = readFixture();
    Session session(file.size());
    require(pixels_inspector_begin_metadata(session.handle())
            == PIXELS_INSPECTOR_RANGE_READY,
            "metadata did not start");
    const pixels::format::FileRange expected = nextRange(session);
    require(pixels_inspector_supply_range(
                    session.handle(), expected.offset, expected.length - 1,
                    file.data() + expected.offset)
            == PIXELS_INSPECTOR_INVALID_ARGUMENT,
            "partial range was not rejected");
}

void testShortFileAndCancellation()
{
    Session shortFile(7);
    require(pixels_inspector_begin_metadata(shortFile.handle())
            == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
            "short file was not rejected");

    const std::vector<std::uint8_t> file = readFixture();
    Session session(file.size());
    require(pixels_inspector_begin_metadata(session.handle())
            == PIXELS_INSPECTOR_RANGE_READY,
            "metadata did not start");
    require(pixels_inspector_cancel(session.handle())
            == PIXELS_INSPECTOR_CANCELLED,
            "active session was not cancelled");
    require(pixels_inspector_begin_metadata(session.handle())
            == PIXELS_INSPECTOR_INVALID_STATE,
            "cancelled session restarted");
}

void testTailBounds()
{
    std::vector<std::uint8_t> file = readFixture();
    std::fill(file.end() - 8, file.end(), 0xFF);
    Session session(file.size());
    require(pixels_inspector_begin_metadata(session.handle())
            == PIXELS_INSPECTOR_RANGE_READY,
            "metadata did not start");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
            "out-of-file tail offset was not rejected");
}

void testMalformedAndInvalidFileTail()
{
    {
        std::vector<std::uint8_t> file = readFixture();
        std::fill(file.begin() + 506, file.begin() + 782, 0);
        Session session(file.size());
        require(pixels_inspector_begin_metadata(session.handle())
                == PIXELS_INSPECTOR_RANGE_READY,
                "metadata did not start");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "tail pointer was not accepted");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_MALFORMED_PROTOBUF,
                "malformed FileTail was not rejected");
        require(!readError(session).empty(),
                "malformed FileTail did not expose an error");
    }
    {
        std::vector<std::uint8_t> file = readFixture();
        mutateFileTail(file, [](pixels::proto::FileTail &tail) {
            tail.mutable_postscript()->set_version(2);
        });
        Session session(file.size());
        require(pixels_inspector_begin_metadata(session.handle())
                == PIXELS_INSPECTOR_RANGE_READY,
                "metadata did not start");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "tail pointer was not accepted");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_UNSUPPORTED_VERSION,
                "unsupported version was not rejected");
    }
    {
        std::vector<std::uint8_t> file = readFixture();
        mutateFileTail(file, [](pixels::proto::FileTail &tail) {
            tail.mutable_postscript()->set_magic("BROKEN");
        });
        Session session(file.size());
        require(pixels_inspector_begin_metadata(session.handle())
                == PIXELS_INSPECTOR_RANGE_READY,
                "metadata did not start");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "tail pointer was not accepted");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_INVALID_MAGIC,
                "invalid magic was not rejected");
    }
    {
        std::vector<std::uint8_t> file = readFixture();
        mutateFileTail(file, [](pixels::proto::FileTail &tail) {
            tail.mutable_footer()
                    ->mutable_rowgroupinfos(0)
                    ->set_footeroffset(600);
        });
        Session session(file.size());
        require(pixels_inspector_begin_metadata(session.handle())
                == PIXELS_INSPECTOR_RANGE_READY,
                "metadata did not start");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "tail pointer was not accepted");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "row-group footer overlapping FileTail was not rejected");
    }
}

void testUnsupportedPageShape()
{
    {
        std::vector<std::uint8_t> file = readFixture();
        mutateFileTail(file, [](pixels::proto::FileTail &tail) {
            tail.mutable_footer()->mutable_types(0)->set_kind(
                    pixels::proto::Type_Kind_INT);
        });
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_UNSUPPORTED_TYPE,
                "non-LONG column was not rejected");
    }
    {
        std::vector<std::uint8_t> file = readFixture();
        mutateRowGroupFooter(
                file, [](pixels::proto::RowGroupFooter &footer) {
                    footer.mutable_rowgroupencoding()
                            ->mutable_columnchunkencodings(0)
                            ->set_kind(
                                    pixels::proto::
                                    ColumnEncoding_Kind_DICTIONARY);
                });
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_UNSUPPORTED_ENCODING,
                "DICTIONARY validation page was not rejected");
    }
    {
        const std::vector<std::uint8_t> file = readFixture();
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 9, 2)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "page beyond row-group rows was not rejected");
    }
    {
        std::vector<std::uint8_t> file = readFixture();
        mutateFileTail(file, [](pixels::proto::FileTail &tail) {
            pixels::proto::PostScript *postScript =
                    tail.mutable_postscript();
            postScript->set_pixelstride(5);
            postScript->set_writertimezone(
                    postScript->writertimezone() + "x");
        });
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 4, 2)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_MALFORMED_PROTOBUF,
                "pixel stride inconsistent with pixel metadata was not rejected");
    }
    {
        std::vector<std::uint8_t> file = readFixture();
        mutateRowGroupFooter(
                file, [](pixels::proto::RowGroupFooter &footer) {
                    footer.mutable_rowgroupindexentry()
                            ->mutable_columnchunkindexentries(0)
                            ->mutable_pixelstatistics(0)
                            ->mutable_statistic()
                            ->set_hasnull(true);
                    footer.mutable_rowgroupindexentry()
                            ->mutable_columnchunkindexentries(0)
                            ->set_chunklength(82);
                });
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "null-containing page did not request its bitmap");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_MALFORMED_PROTOBUF,
                "inconsistent null bitmap was not rejected");
    }
    {
        std::vector<std::uint8_t> file = readFixture();
        std::fill(file.begin() + 352, file.begin() + 506, 0);
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_MALFORMED_PROTOBUF,
                "malformed RowGroupFooter was not rejected");
    }
}

void testCancellationStates()
{
    const std::vector<std::uint8_t> file = readFixture();
    {
        Session session(file.size());
        require(pixels_inspector_begin_metadata(session.handle())
                == PIXELS_INSPECTOR_RANGE_READY,
                "metadata did not start");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "tail pointer was not accepted");
        require(pixels_inspector_cancel(session.handle())
                == PIXELS_INSPECTOR_CANCELLED,
                "FileTail wait was not cancellable");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_cancel(session.handle())
                == PIXELS_INSPECTOR_CANCELLED,
                "metadata-ready state was not cancellable");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(pixels_inspector_cancel(session.handle())
                == PIXELS_INSPECTOR_CANCELLED,
                "row-group footer wait was not cancellable");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "row-group footer was not accepted");
        require(pixels_inspector_cancel(session.handle())
                == PIXELS_INSPECTOR_CANCELLED,
                "column-chunk wait was not cancellable");
    }
    {
        const std::vector<std::uint8_t> multiPixel =
                makeMultiPixelLongFixture(false, {0x02, 0, 0x01});
        Session session(multiPixel.size());
        driveMetadataFlexible(session, multiPixel);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "nullable page did not request its footer");
        require(supply(session, nextRange(session), multiPixel)
                == PIXELS_INSPECTOR_RANGE_READY,
                "nullable footer did not request its bitmap");
        require(pixels_inspector_cancel(session.handle())
                == PIXELS_INSPECTOR_CANCELLED,
                "per-pixel null bitmap wait was not cancellable");
    }
    {
        const std::vector<std::uint8_t> multiPixel =
                makeMultiPixelLongFixture(false, {0, 0, 0});
        Session session(multiPixel.size());
        driveMetadataFlexible(session, multiPixel);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 2, 6)
                == PIXELS_INSPECTOR_RANGE_READY,
                "cross-pixel page did not request its footer");
        require(supply(session, nextRange(session), multiPixel)
                == PIXELS_INSPECTOR_RANGE_READY,
                "cross-pixel footer did not request values");
        require(supply(session, nextRange(session), multiPixel)
                == PIXELS_INSPECTOR_RANGE_READY,
                "first pixel did not request second-pixel values");
        require(pixels_inspector_cancel(session.handle())
                == PIXELS_INSPECTOR_CANCELLED,
                "second-pixel value wait was not cancellable");
    }
}

void testInvalidPageRequests()
{
    const std::vector<std::uint8_t> file = readFixture();
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 1, 0, 0, 1)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "invalid row group was accepted");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 4, 0, 1)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "invalid column was accepted");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 0)
                == PIXELS_INSPECTOR_INVALID_ARGUMENT,
                "empty page was accepted");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 65537)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "page above the bounded row limit was accepted");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0,
                        std::numeric_limits<std::uint64_t>::max(), 1)
                == PIXELS_INSPECTOR_RANGE_READY,
                "overflowing page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "overflowing page row range was not rejected");
    }
}

} // namespace

int main()
{
    try
    {
        require(pixels_inspector_abi_version()
                == PIXELS_INSPECTOR_ABI_VERSION,
                "unexpected inspector ABI version");
        testPlainScalarDecoder();
        testPlainPixelPlanner();
        testPlainLongDecoder();
        testCanonicalFixture();
        testBoundedPageRange();
        testGenericPlainScalarPages();
        testMultiPixelPlainPages();
        testMultiPixelRunLengthPages();
        testMultiPixelVariablePages();
        testBinaryPage();
        testVectorPage();
        testDictionaryPages();
        testLongDecimalPage();
        testByteRlePage();
        testMultiPixelMalformedMetadata();
        testLifecycleAndBuffers();
        testInvalidRangeAndTerminalState();
        testPartialRange();
        testShortFileAndCancellation();
        testTailBounds();
        testMalformedAndInvalidFileTail();
        testInvalidPageRequests();
        testUnsupportedPageShape();
        testCancellationStates();
        std::cout << "pixels-inspector native conformance: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << "pixels-inspector native conformance: FAIL: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
