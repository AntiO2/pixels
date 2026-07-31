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
#include "format/SchemaValidator.h"
#include "pixels.pb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
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
        "{\"abi\":4,\"version\":1,\"magic\":\"PIXELS\",\"rows\":10,"
        "\"pixelStride\":10000,\"schemaCount\":4,\"rowGroupCount\":1,"
        "\"firstColumn\":{\"name\":\"id\",\"kind\":4},"
        "\"postscript\":{\"contentLength\":\"352\",\"compression\":0,"
        "\"compressionBlockSize\":1,"
        "\"writerTimezone\":\"Central European Standard Time\","
        "\"partitioned\":false,\"columnChunkAlignment\":32,"
        "\"hasHiddenColumn\":false},\"schema\":["
        "{\"id\":0,\"name\":\"id\",\"kind\":4,\"subtypes\":[]},"
        "{\"id\":1,\"name\":\"name\",\"kind\":16,\"subtypes\":[],"
        "\"maximumLength\":25},"
        "{\"id\":2,\"name\":\"birthday\",\"kind\":15,\"subtypes\":[]},"
        "{\"id\":3,\"name\":\"score\",\"kind\":14,\"subtypes\":[],"
        "\"precision\":15,\"scale\":2}],\"fileStatistics\":["
        "{\"numberOfValues\":\"10\",\"containsNull\":false,"
        "\"integer\":{\"minimum\":\"0\",\"maximum\":\"9\","
        "\"sum\":\"45\"}},"
        "{\"numberOfValues\":\"10\",\"containsNull\":false,"
        "\"string\":{\"minimum\":\"Alice\",\"maximum\":\"Tom\","
        "\"sum\":\"47\"}},"
        "{\"numberOfValues\":\"10\",\"containsNull\":false,"
        "\"date\":{\"minimum\":\"-25202\",\"maximum\":\"14389\"}},"
        "{\"numberOfValues\":\"10\",\"containsNull\":false,"
        "\"integer\":{\"minimum\":\"740\",\"maximum\":\"10001\","
        "\"sum\":\"66057\"}}],\"rowGroups\":["
        "{\"index\":0,\"footerOffset\":\"352\",\"footerLength\":154,"
        "\"dataLength\":352,\"rows\":10}],"
        "\"rowGroupStatistics\":[["
        "{\"numberOfValues\":\"10\",\"containsNull\":false,"
        "\"integer\":{\"minimum\":\"0\",\"maximum\":\"9\","
        "\"sum\":\"45\"}},"
        "{\"numberOfValues\":\"10\",\"containsNull\":false,"
        "\"string\":{\"minimum\":\"Alice\",\"maximum\":\"Tom\","
        "\"sum\":\"47\"}},"
        "{\"numberOfValues\":\"10\",\"containsNull\":false,"
        "\"date\":{\"minimum\":\"-25202\",\"maximum\":\"14389\"}},"
        "{\"numberOfValues\":\"10\",\"containsNull\":false,"
        "\"integer\":{\"minimum\":\"740\",\"maximum\":\"10001\","
        "\"sum\":\"66057\"}}]]}";

const std::string EXPECTED_CAPABILITIES =
        "{\"abi\":4,\"page\":\"generic-v1\","
        "\"rowGroup\":\"layout-v1\",\"rows\":\"rows-v1\","
        "\"filter\":\"filter-v1\",\"scan\":\"scan-v2\","
        "\"maxRows\":500,\"defaultRows\":100,"
        "\"defaultScanRows\":20,\"maxScanRows\":500,"
        "\"maxProjectionColumns\":128,\"maxExpressionNodes\":64,"
        "\"maxOrderKeys\":8,\"maxOrderedWindowRows\":4096,\"types\":["
        "{\"kind\":0,\"name\":\"BOOLEAN\"},"
        "{\"kind\":1,\"name\":\"BYTE\"},"
        "{\"kind\":2,\"name\":\"SHORT\"},"
        "{\"kind\":3,\"name\":\"INT\"},"
        "{\"kind\":4,\"name\":\"LONG\"},"
        "{\"kind\":5,\"name\":\"FLOAT\"},"
        "{\"kind\":6,\"name\":\"DOUBLE\"},"
        "{\"kind\":7,\"name\":\"STRING\"},"
        "{\"kind\":8,\"name\":\"BINARY\"},"
        "{\"kind\":9,\"name\":\"TIMESTAMP\"},"
        "{\"kind\":10,\"name\":\"ARRAY\"},"
        "{\"kind\":11,\"name\":\"MAP\"},"
        "{\"kind\":12,\"name\":\"STRUCT\"},"
        "{\"kind\":13,\"name\":\"VARBINARY\"},"
        "{\"kind\":14,\"name\":\"DECIMAL\"},"
        "{\"kind\":15,\"name\":\"DATE\"},"
        "{\"kind\":16,\"name\":\"VARCHAR\"},"
        "{\"kind\":17,\"name\":\"CHAR\"},"
        "{\"kind\":18,\"name\":\"TIME\"},"
        "{\"kind\":19,\"name\":\"VECTOR\"}],"
        "\"encodings\":[\"NONE\",\"RUNLENGTH\",\"DICTIONARY\"],"
        "\"compression\":{\"metadata\":[\"NONE\",\"ZLIB\",\"SNAPPY\","
        "\"LZO\",\"LZ4\",\"ZSTD\"],\"payload\":\"inactive\","
        "\"reason\":\"postscript-v1-unused\"},"
        "\"nested\":\"portable-v1\"}";

const std::string EXPECTED_PAGE =
        "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":10,"
        "\"values\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5\",\"6\","
        "\"7\",\"8\",\"9\"]}";

const std::string EXPECTED_ROW_GROUP =
        "{\"rowGroup\":0,\"columns\":["
        "{\"column\":0,\"encoding\":{\"kind\":0},\"chunk\":{"
        "\"offset\":\"0\",\"length\":80,\"nullOffset\":80,"
        "\"littleEndian\":true,\"nullsPadding\":false,"
        "\"nullAlignment\":0,\"pixels\":[{\"index\":0,\"position\":0,"
        "\"statistics\":{\"numberOfValues\":\"10\","
        "\"containsNull\":false,\"integer\":{\"minimum\":\"0\","
        "\"maximum\":\"9\",\"sum\":\"45\"}}}]}},"
        "{\"column\":1,\"encoding\":{\"kind\":0},\"chunk\":{"
        "\"offset\":\"96\",\"length\":95,\"nullOffset\":47,"
        "\"littleEndian\":true,\"nullsPadding\":false,"
        "\"nullAlignment\":0,\"pixels\":[{\"index\":0,\"position\":0,"
        "\"statistics\":{\"numberOfValues\":\"10\","
        "\"containsNull\":false,\"string\":{\"minimum\":\"Alice\","
        "\"maximum\":\"Tom\",\"sum\":\"47\"}}}]}},"
        "{\"column\":2,\"encoding\":{\"kind\":0},\"chunk\":{"
        "\"offset\":\"192\",\"length\":40,\"nullOffset\":40,"
        "\"littleEndian\":true,\"nullsPadding\":false,"
        "\"nullAlignment\":0,\"pixels\":[{\"index\":0,\"position\":0,"
        "\"statistics\":{\"numberOfValues\":\"10\","
        "\"containsNull\":false,\"date\":{\"minimum\":\"-25202\","
        "\"maximum\":\"14389\"}}}]}},"
        "{\"column\":3,\"encoding\":{\"kind\":0},\"chunk\":{"
        "\"offset\":\"256\",\"length\":80,\"nullOffset\":80,"
        "\"littleEndian\":true,\"nullsPadding\":false,"
        "\"nullAlignment\":0,\"pixels\":[{\"index\":0,\"position\":0,"
        "\"statistics\":{\"numberOfValues\":\"10\","
        "\"containsNull\":false,\"integer\":{\"minimum\":\"740\","
        "\"maximum\":\"10001\",\"sum\":\"66057\"}}}]}}]}";

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

void rewriteFileTail(
        std::vector<std::uint8_t> &file,
        const std::function<void(pixels::proto::FileTail &)> &mutation)
{
    require(file.size() >= 8, "fixture has no tail pointer");
    std::uint64_t tailOffset = 0;
    for (std::size_t byte = file.size() - 8;
         byte < file.size(); ++byte)
    {
        tailOffset = (tailOffset << 8U) | file[byte];
    }
    require(tailOffset <= file.size() - 8,
            "fixture tail pointer is out of bounds");
    pixels::proto::FileTail tail;
    require(tail.ParseFromArray(
                    file.data() + static_cast<std::size_t>(tailOffset),
                    static_cast<int>(
                            file.size() - 8U
                            - static_cast<std::size_t>(tailOffset))),
            "unable to parse fixture FileTail for rewrite");
    mutation(tail);
    std::string serialized;
    require(tail.SerializeToString(&serialized),
            "unable to serialize rewritten FileTail");
    file.resize(static_cast<std::size_t>(tailOffset));
    file.insert(file.end(), serialized.begin(), serialized.end());
    for (int byte = 7; byte >= 0; --byte)
    {
        file.push_back(static_cast<std::uint8_t>(
                tailOffset
                >> (static_cast<std::uint32_t>(byte) * 8U)));
    }
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

void writeLittleUint16(
        std::vector<std::uint8_t> &bytes, std::size_t offset,
        std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void writeLittleUint32(
        std::vector<std::uint8_t> &bytes, std::size_t offset,
        std::uint32_t value)
{
    for (std::uint32_t byte = 0; byte < 4; ++byte)
    {
        bytes[offset + byte] = static_cast<std::uint8_t>(
                value >> (byte * 8U));
    }
}

void writeLittleUint64(
        std::vector<std::uint8_t> &bytes, std::size_t offset,
        std::uint64_t value)
{
    for (std::uint32_t byte = 0; byte < 8; ++byte)
    {
        bytes[offset + byte] = static_cast<std::uint8_t>(
                value >> (byte * 8U));
    }
}

struct TestScanNode
{
    std::uint8_t kind = 0;
    std::uint8_t operation = 0;
    std::uint16_t children = 0;
    std::uint32_t column = 0;
    std::string literal;
};

struct TestScanOrder
{
    std::uint32_t column = 0;
    std::uint8_t direction = 0;
    std::uint8_t nulls = 1;
};

std::vector<std::uint8_t> makeScanPlan(
        bool projectionAll,
        const std::vector<std::uint32_t> &projection,
        const std::vector<TestScanNode> &nodes,
        const std::vector<TestScanOrder> &order,
        std::uint64_t offset, std::uint32_t limit,
        const std::string &cursor = std::string())
{
    std::string literals;
    for (const TestScanNode &node : nodes)
    {
        literals += node.literal;
    }
    const std::size_t total =
            48 + projection.size() * 4 + nodes.size() * 20
            + order.size() * 8 + literals.size() + cursor.size();
    std::vector<std::uint8_t> packet(total, 0);
    packet[0] = 'P';
    packet[1] = 'X';
    packet[2] = 'S';
    packet[3] = 'V';
    writeLittleUint16(packet, 4, 1);
    writeLittleUint16(packet, 6, 48);
    writeLittleUint32(packet, 8, static_cast<std::uint32_t>(total));
    writeLittleUint32(packet, 12, projectionAll ? 1 : 0);
    writeLittleUint16(
            packet, 16,
            static_cast<std::uint16_t>(projection.size()));
    writeLittleUint16(
            packet, 18, static_cast<std::uint16_t>(nodes.size()));
    writeLittleUint16(
            packet, 20, static_cast<std::uint16_t>(order.size()));
    writeLittleUint32(
            packet, 24, static_cast<std::uint32_t>(literals.size()));
    writeLittleUint16(
            packet, 28, static_cast<std::uint16_t>(cursor.size()));
    writeLittleUint64(packet, 32, offset);
    writeLittleUint32(packet, 40, limit);
    std::size_t position = 48;
    for (std::uint32_t column : projection)
    {
        writeLittleUint32(packet, position, column);
        position += 4;
    }
    std::uint32_t literalOffset = 0;
    for (const TestScanNode &node : nodes)
    {
        packet[position] = node.kind;
        packet[position + 1] = node.operation;
        writeLittleUint16(packet, position + 2, node.children);
        writeLittleUint32(packet, position + 4, node.column);
        writeLittleUint32(
                packet, position + 8,
                node.kind == 1 ? literalOffset : 0);
        writeLittleUint32(
                packet, position + 12,
                static_cast<std::uint32_t>(node.literal.size()));
        if (node.kind == 1)
        {
            literalOffset += static_cast<std::uint32_t>(
                    node.literal.size());
        }
        position += 20;
    }
    for (const TestScanOrder &key : order)
    {
        writeLittleUint32(packet, position, key.column);
        packet[position + 4] = key.direction;
        packet[position + 5] = key.nulls;
        position += 8;
    }
    std::copy(literals.begin(), literals.end(), packet.begin() + position);
    position += literals.size();
    std::copy(cursor.begin(), cursor.end(), packet.begin() + position);
    return packet;
}

void appendUint32(
        std::vector<std::uint8_t> &bytes, std::uint32_t value,
        bool littleEndian)
{
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        const std::uint32_t byte =
                littleEndian ? index : 3U - index;
        bytes.push_back(static_cast<std::uint8_t>(
                value >> (byte * 8U)));
    }
}

void appendUint64(
        std::vector<std::uint8_t> &bytes, std::uint64_t value,
        bool littleEndian)
{
    for (std::uint32_t index = 0; index < 8; ++index)
    {
        const std::uint32_t byte =
                littleEndian ? index : 7U - index;
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

std::vector<std::uint8_t> makeFixedScalarFixture(
        pixels::proto::Type_Kind kind, bool littleEndian)
{
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::RowGroupFooter rowGroupFooter;
    require(rowGroupFooter.ParseFromArray(
                    canonical.data() + 352, 154),
            "unable to parse scalar fixture RowGroupFooter");
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse scalar fixture FileTail");

    std::vector<std::uint8_t> columnData;
    switch (kind)
    {
        case pixels::proto::Type_Kind_BOOLEAN:
            columnData.push_back(littleEndian ? 0x0D : 0xB0);
            break;
        case pixels::proto::Type_Kind_BYTE:
            columnData = {0x80, 0xFF, 0x00, 0x7F};
            break;
        case pixels::proto::Type_Kind_SHORT:
            for (const std::int32_t value :
                 std::vector<std::int32_t>{-32768, -1, 0, 32767})
            {
                appendUint32(
                        columnData,
                        static_cast<std::uint32_t>(value),
                        littleEndian);
            }
            break;
        case pixels::proto::Type_Kind_INT:
            for (const std::int32_t value :
                 std::vector<std::int32_t>{
                         std::numeric_limits<std::int32_t>::min(),
                         -1, 0,
                         std::numeric_limits<std::int32_t>::max()})
            {
                appendUint32(
                        columnData,
                        static_cast<std::uint32_t>(value),
                        littleEndian);
            }
            break;
        case pixels::proto::Type_Kind_DATE:
            for (const std::int32_t value :
                 std::vector<std::int32_t>{-1, 0, 1, 20000})
            {
                appendUint32(
                        columnData,
                        static_cast<std::uint32_t>(value),
                        littleEndian);
            }
            break;
        case pixels::proto::Type_Kind_TIME:
            for (const std::int32_t value :
                 std::vector<std::int32_t>{0, 1, 86399999, -1})
            {
                appendUint32(
                        columnData,
                        static_cast<std::uint32_t>(value),
                        littleEndian);
            }
            break;
        case pixels::proto::Type_Kind_LONG:
        case pixels::proto::Type_Kind_TIMESTAMP:
            for (const std::int64_t value :
                 std::vector<std::int64_t>{
                         std::numeric_limits<std::int64_t>::min(),
                         -1, 0,
                         std::numeric_limits<std::int64_t>::max()})
            {
                appendUint64(
                        columnData,
                        static_cast<std::uint64_t>(value),
                        littleEndian);
            }
            break;
        case pixels::proto::Type_Kind_FLOAT:
            for (const float value :
                 std::vector<float>{
                         1.5F, -2.25F,
                         std::numeric_limits<float>::quiet_NaN(),
                         std::numeric_limits<float>::infinity()})
            {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                appendUint32(columnData, bits, littleEndian);
            }
            break;
        case pixels::proto::Type_Kind_DOUBLE:
            for (const double value :
                 std::vector<double>{
                         1.5, -2.25,
                         std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()})
            {
                std::uint64_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                appendUint64(columnData, bits, littleEndian);
            }
            break;
        case pixels::proto::Type_Kind_STRING:
        case pixels::proto::Type_Kind_BINARY:
        case pixels::proto::Type_Kind_ARRAY:
        case pixels::proto::Type_Kind_MAP:
        case pixels::proto::Type_Kind_STRUCT:
        case pixels::proto::Type_Kind_VARBINARY:
        case pixels::proto::Type_Kind_DECIMAL:
        case pixels::proto::Type_Kind_VARCHAR:
        case pixels::proto::Type_Kind_CHAR:
        case pixels::proto::Type_Kind_VECTOR:
            require(false, "kind is not a fixed scalar fixture");
            break;
    }

    pixels::proto::ColumnChunkIndex *chunk =
            rowGroupFooter.mutable_rowgroupindexentry()
                    ->mutable_columnchunkindexentries(0);
    chunk->set_chunkoffset(0);
    chunk->set_chunklength(columnData.size());
    chunk->set_isnulloffset(columnData.size());
    chunk->clear_pixelpositions();
    chunk->add_pixelpositions(0);
    chunk->clear_pixelstatistics();
    pixels::proto::ColumnStatistic *statistic =
            chunk->add_pixelstatistics()->mutable_statistic();
    statistic->set_numberofvalues(4);
    statistic->set_hasnull(false);
    chunk->set_littleendian(littleEndian);
    chunk->set_nullspadding(false);
    rowGroupFooter.mutable_rowgroupencoding()
            ->mutable_columnchunkencodings(0)
            ->set_kind(pixels::proto::ColumnEncoding_Kind_NONE);

    pixels::proto::Type *type =
            fileTail.mutable_footer()->mutable_types(0);
    type->set_kind(kind);
    type->set_name("value");
    type->clear_maximumlength();
    type->clear_precision();
    type->clear_scale();
    type->clear_dimension();
    pixels::proto::RowGroupInformation *rowGroup =
            fileTail.mutable_footer()->mutable_rowgroupinfos(0);
    rowGroup->set_footeroffset(352);
    rowGroup->set_numberofrows(4);
    fileTail.mutable_postscript()->set_pixelstride(4);
    fileTail.mutable_postscript()->set_numberofrows(4);

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize scalar fixture RowGroupFooter");
    rowGroup->set_footerlength(serializedFooter.size());
    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize scalar fixture FileTail");

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

std::vector<std::uint8_t> makeShortDecimalFixture(
        std::uint32_t precision, std::uint32_t scale,
        bool littleEndian, const std::vector<std::int64_t> &values)
{
    require(values.size() == 4,
            "short DECIMAL fixture requires four values");
    std::vector<std::uint8_t> file =
            makeFixedScalarFixture(
                    pixels::proto::Type_Kind_LONG, littleEndian);
    std::vector<std::uint8_t> bytes;
    for (std::int64_t value : values)
    {
        appendUint64(
                bytes, static_cast<std::uint64_t>(value),
                littleEndian);
    }
    std::copy(bytes.begin(), bytes.end(), file.begin());
    rewriteFileTail(
            file, [precision, scale](pixels::proto::FileTail &tail) {
                pixels::proto::Type *type =
                        tail.mutable_footer()->mutable_types(0);
                type->set_kind(pixels::proto::Type_Kind_DECIMAL);
                type->set_precision(precision);
                type->set_scale(scale);
            });
    return file;
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

    // Pixel stride is file-global. Keep this generated fixture genuinely
    // LONG-only so every declared root column has the same three-Pixel shape.
    // Retaining the canonical fixture's other single-Pixel columns would make
    // their indexes inconsistent with the rewritten stride of four rows.
    while (rowGroupFooter.mutable_rowgroupindexentry()
                   ->columnchunkindexentries_size() > 1)
    {
        rowGroupFooter.mutable_rowgroupindexentry()
                ->mutable_columnchunkindexentries()->RemoveLast();
    }
    while (rowGroupFooter.mutable_rowgroupencoding()
                   ->columnchunkencodings_size() > 1)
    {
        rowGroupFooter.mutable_rowgroupencoding()
                ->mutable_columnchunkencodings()->RemoveLast();
    }
    while (fileTail.mutable_footer()->types_size() > 1)
    {
        fileTail.mutable_footer()->mutable_types()->RemoveLast();
    }
    while (fileTail.mutable_footer()->columnstats_size() > 1)
    {
        fileTail.mutable_footer()->mutable_columnstats()->RemoveLast();
    }
    require(fileTail.mutable_footer()->rowgroupstats_size() == 1,
            "canonical fixture must contain one row-group statistic");
    while (fileTail.mutable_footer()->mutable_rowgroupstats(0)
                   ->columnchunkstats_size() > 1)
    {
        fileTail.mutable_footer()->mutable_rowgroupstats(0)
                ->mutable_columnchunkstats()->RemoveLast();
    }
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
        statistics->mutable_statistic()->set_numberofvalues(rows);
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

std::vector<std::uint8_t> makeMultiPixelVarcharFixture(
        pixels::proto::Type_Kind kind =
                pixels::proto::Type_Kind_VARCHAR)
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
    type->set_kind(kind);
    type->set_name("name");
    if (kind == pixels::proto::Type_Kind_VARCHAR
        || kind == pixels::proto::Type_Kind_CHAR)
    {
        type->set_maximumlength(16);
    }
    else
    {
        type->clear_maximumlength();
    }
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

std::vector<std::uint8_t> makeBinaryFixture(
        pixels::proto::Type_Kind kind =
                pixels::proto::Type_Kind_VARBINARY)
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
    type->set_kind(kind);
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

std::vector<std::uint8_t> makeLongDecimalFixture(
        std::uint32_t precision = 38, std::uint32_t scale = 4,
        bool littleEndian = true)
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
    appendUint64(columnData, 0, littleEndian);
    appendUint64(columnData, 123456, littleEndian);
    appendUint64(
            columnData, std::numeric_limits<std::uint64_t>::max(),
            littleEndian);
    appendUint64(
            columnData, std::numeric_limits<std::uint64_t>::max(),
            littleEndian);
    for (std::uint32_t row = 2; row < 10; ++row)
    {
        appendUint64(columnData, 0, littleEndian);
        appendUint64(columnData, 0, littleEndian);
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
    chunk->set_littleendian(littleEndian);
    chunk->set_nullspadding(false);
    rowGroupFooter.mutable_rowgroupencoding()
            ->mutable_columnchunkencodings(0)
            ->set_kind(pixels::proto::ColumnEncoding_Kind_NONE);

    pixels::proto::Type *type =
            fileTail.mutable_footer()->mutable_types(0);
    type->set_kind(pixels::proto::Type_Kind_DECIMAL);
    type->set_name("amount");
    type->set_precision(precision);
    type->set_scale(scale);
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

std::vector<std::uint8_t> makeNestedFixture()
{
    const std::vector<std::uint8_t> canonical = readFixture();
    pixels::proto::FileTail fileTail;
    require(fileTail.ParseFromArray(
                    canonical.data() + 506, 276),
            "unable to parse nested fixture FileTail");
    pixels::proto::Footer *footer = fileTail.mutable_footer();
    footer->clear_types();
    footer->clear_columnstats();
    footer->clear_rowgroupinfos();
    footer->clear_rowgroupstats();

    pixels::proto::Type *root = footer->add_types();
    root->set_kind(pixels::proto::Type_Kind_STRUCT);
    root->set_name("root");
    root->add_subtypes(1);
    root->add_subtypes(3);
    root->add_subtypes(6);
    pixels::proto::Type *array = footer->add_types();
    array->set_kind(pixels::proto::Type_Kind_ARRAY);
    array->set_name("tags");
    array->add_subtypes(2);
    pixels::proto::Type *item = footer->add_types();
    item->set_kind(pixels::proto::Type_Kind_LONG);
    item->set_name("item");
    pixels::proto::Type *map = footer->add_types();
    map->set_kind(pixels::proto::Type_Kind_MAP);
    map->set_name("attrs");
    map->add_subtypes(4);
    map->add_subtypes(5);
    pixels::proto::Type *key = footer->add_types();
    key->set_kind(pixels::proto::Type_Kind_LONG);
    key->set_name("key");
    pixels::proto::Type *mapValue = footer->add_types();
    mapValue->set_kind(pixels::proto::Type_Kind_VARCHAR);
    mapValue->set_name("value");
    mapValue->set_maximumlength(8);
    pixels::proto::Type *label = footer->add_types();
    label->set_kind(pixels::proto::Type_Kind_VARCHAR);
    label->set_name("label");
    label->set_maximumlength(8);

    pixels::proto::RowGroupFooter rowGroupFooter;
    std::vector<std::uint8_t> data;
    const auto addChunk =
            [&](const std::vector<std::uint8_t> &chunkBytes,
                std::uint32_t dataLength, std::uint64_t logicalRows,
                bool hasNull, bool nullsPadding)
    {
        pixels::proto::ColumnChunkIndex *chunk =
                rowGroupFooter.mutable_rowgroupindexentry()
                        ->add_columnchunkindexentries();
        chunk->set_chunkoffset(data.size());
        chunk->set_chunklength(chunkBytes.size());
        chunk->set_isnulloffset(dataLength);
        chunk->add_pixelpositions(0);
        pixels::proto::ColumnStatistic *statistic =
                chunk->add_pixelstatistics()->mutable_statistic();
        statistic->set_numberofvalues(logicalRows);
        statistic->set_hasnull(hasNull);
        chunk->set_littleendian(true);
        chunk->set_nullspadding(nullsPadding);
        rowGroupFooter.mutable_rowgroupencoding()
                ->add_columnchunkencodings()
                ->set_kind(pixels::proto::ColumnEncoding_Kind_NONE);
        data.insert(data.end(), chunkBytes.begin(), chunkBytes.end());
    };

    addChunk({0x02}, 0, 3, true, false);

    std::vector<std::uint8_t> arrayRanges;
    for (const std::int64_t value :
         std::vector<std::int64_t>{0, 2, 2, 3, 3, 5})
    {
        appendLittleInt64(arrayRanges, value);
    }
    addChunk(arrayRanges, arrayRanges.size(), 3, false, false);

    std::vector<std::uint8_t> items;
    for (std::int64_t value = 10; value < 15; ++value)
    {
        appendLittleInt64(items, value);
    }
    addChunk(items, items.size(), 5, false, false);

    std::vector<std::uint8_t> mapRanges;
    for (const std::int64_t value :
         std::vector<std::int64_t>{0, 1, 1, 3, 3, 3})
    {
        appendLittleInt64(mapRanges, value);
    }
    addChunk(mapRanges, mapRanges.size(), 3, false, false);

    std::vector<std::uint8_t> keys;
    for (const std::int64_t value :
         std::vector<std::int64_t>{1, 2, 3})
    {
        appendLittleInt64(keys, value);
    }
    addChunk(keys, keys.size(), 3, false, false);

    std::vector<std::uint8_t> values = {'a', 'b', 'b', 'c'};
    for (const std::uint32_t start :
         std::vector<std::uint32_t>{0, 1, 3, 4})
    {
        appendLittleUint32(values, start);
    }
    appendLittleUint32(values, 4);
    addChunk(values, 4, 3, false, false);

    std::vector<std::uint8_t> labels = {'x', 'y', 'z'};
    for (const std::uint32_t start :
         std::vector<std::uint32_t>{0, 1, 2, 3})
    {
        appendLittleUint32(labels, start);
    }
    appendLittleUint32(labels, 3);
    addChunk(labels, 3, 3, false, false);

    std::string serializedFooter;
    require(rowGroupFooter.SerializeToString(&serializedFooter),
            "unable to serialize nested RowGroupFooter");
    pixels::proto::RowGroupInformation *rowGroup =
            footer->add_rowgroupinfos();
    rowGroup->set_footeroffset(data.size());
    rowGroup->set_footerlength(serializedFooter.size());
    rowGroup->set_datalength(data.size());
    rowGroup->set_numberofrows(3);
    fileTail.mutable_postscript()->set_pixelstride(10);
    fileTail.mutable_postscript()->set_numberofrows(3);
    fileTail.mutable_postscript()->set_contentlength(
            data.size() + serializedFooter.size());

    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize nested FileTail");
    std::vector<std::uint8_t> file = data;
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

std::vector<std::uint8_t> makeTpchLineitemFixture()
{
    const char *const names[] = {
            "l_orderkey", "l_partkey", "l_suppkey", "l_linenumber",
            "l_quantity", "l_extendedprice", "l_discount", "l_tax",
            "l_returnflag", "l_linestatus", "l_shipdate",
            "l_commitdate", "l_receiptdate", "l_shipinstruct",
            "l_shipmode", "l_comment"};
    const pixels::proto::Type_Kind kinds[] = {
            pixels::proto::Type_Kind_LONG,
            pixels::proto::Type_Kind_LONG,
            pixels::proto::Type_Kind_LONG,
            pixels::proto::Type_Kind_INT,
            pixels::proto::Type_Kind_DECIMAL,
            pixels::proto::Type_Kind_DECIMAL,
            pixels::proto::Type_Kind_DECIMAL,
            pixels::proto::Type_Kind_DECIMAL,
            pixels::proto::Type_Kind_CHAR,
            pixels::proto::Type_Kind_CHAR,
            pixels::proto::Type_Kind_DATE,
            pixels::proto::Type_Kind_DATE,
            pixels::proto::Type_Kind_DATE,
            pixels::proto::Type_Kind_CHAR,
            pixels::proto::Type_Kind_CHAR,
            pixels::proto::Type_Kind_VARCHAR};
    pixels::proto::FileTail fileTail;
    pixels::proto::PostScript *postScript =
            fileTail.mutable_postscript();
    postScript->set_version(1);
    postScript->set_numberofrows(4);
    postScript->set_pixelstride(2);
    postScript->set_compression(
            pixels::proto::NONE);
    postScript->set_compressionblocksize(1);
    postScript->set_writertimezone("UTC");
    postScript->set_partitioned(false);
    postScript->set_columnchunkalignment(1);
    postScript->set_hashiddencolumn(false);
    postScript->set_magic("PIXELS");

    pixels::proto::Footer *footer = fileTail.mutable_footer();
    for (std::size_t column = 0; column < 16; ++column)
    {
        pixels::proto::Type *type = footer->add_types();
        type->set_kind(kinds[column]);
        type->set_name(names[column]);
        if (kinds[column] == pixels::proto::Type_Kind_DECIMAL)
        {
            type->set_precision(15);
            type->set_scale(2);
        }
        else if (kinds[column] == pixels::proto::Type_Kind_CHAR)
        {
            const std::uint32_t maximumLengths[] = {
                    1, 1, 25, 10};
            const std::size_t variableIndex =
                    column == 8 ? 0
                    : column == 9 ? 1
                    : column == 13 ? 2 : 3;
            type->set_maximumlength(
                    maximumLengths[variableIndex]);
        }
        else if (kinds[column] == pixels::proto::Type_Kind_VARCHAR)
        {
            type->set_maximumlength(44);
        }
        pixels::proto::ColumnStatistic *fileStatistic =
                footer->add_columnstats();
        fileStatistic->set_numberofvalues(4);
        fileStatistic->set_hasnull(false);
    }

    const auto appendVariable =
            [](std::vector<std::uint8_t> &target,
               const std::vector<std::string> &values)
    {
        std::vector<std::uint32_t> starts;
        starts.push_back(0);
        for (const std::string &value : values)
        {
            target.insert(target.end(), value.begin(), value.end());
            starts.push_back(
                    static_cast<std::uint32_t>(target.size()));
        }
        const std::uint32_t startsOffset =
                static_cast<std::uint32_t>(target.size());
        for (const std::uint32_t start : starts)
        {
            appendLittleUint32(target, start);
        }
        appendLittleUint32(target, startsOffset);
    };

    std::vector<std::uint8_t> body;
    const auto appendRowGroup =
            [&](std::uint32_t rowGroupIndex)
    {
        const std::vector<std::vector<std::string>> strings =
                rowGroupIndex == 0
                ? std::vector<std::vector<std::string>>{
                        {"N", "R"}, {"O", "F"},
                        {"DELIVER", "TAKE"}, {"AIR", "SHIP"},
                        {"first line", "second line"}}
                : std::vector<std::vector<std::string>>{
                        {"A", "N"}, {"F", "O"},
                        {"COLLECT", "DELIVER"}, {"RAIL", "AIR"},
                        {"third line", "fourth line"}};
        pixels::proto::RowGroupFooter rowGroupFooter;
        pixels::proto::RowGroupStatistic *rowStatistics =
                footer->add_rowgroupstats();
        const std::uint64_t dataStart = body.size();
        std::size_t variableIndex = 0;
        for (std::size_t column = 0; column < 16; ++column)
        {
            std::vector<std::uint8_t> chunkBytes;
            const std::uint32_t rowBase = rowGroupIndex * 2U;
            switch (kinds[column])
            {
                case pixels::proto::Type_Kind_LONG:
                    appendLittleInt64(
                            chunkBytes,
                            static_cast<std::int64_t>(
                                    (column + 1U) * 10U
                                    + rowBase + 1U));
                    appendLittleInt64(
                            chunkBytes,
                            static_cast<std::int64_t>(
                                    (column + 1U) * 10U
                                    + rowBase + 2U));
                    break;
                case pixels::proto::Type_Kind_INT:
                    appendLittleUint32(
                            chunkBytes, rowBase + 1U);
                    appendLittleUint32(
                            chunkBytes, rowBase + 2U);
                    break;
                case pixels::proto::Type_Kind_DECIMAL:
                    appendLittleInt64(
                            chunkBytes,
                            static_cast<std::int64_t>(
                                    (column + 1U) * 100U
                                    + rowBase * 25U + 25U));
                    appendLittleInt64(
                            chunkBytes,
                            static_cast<std::int64_t>(
                                    (column + 1U) * 100U
                                    + rowBase * 25U + 50U));
                    break;
                case pixels::proto::Type_Kind_DATE:
                    appendLittleUint32(
                            chunkBytes, 9496U + rowBase);
                    appendLittleUint32(
                            chunkBytes, 9497U + rowBase);
                    break;
                case pixels::proto::Type_Kind_CHAR:
                case pixels::proto::Type_Kind_VARCHAR:
                    appendVariable(
                            chunkBytes, strings[variableIndex]);
                    ++variableIndex;
                    break;
                default:
                    require(false, "unexpected TPC-H lineitem kind");
            }

            pixels::proto::ColumnChunkIndex *chunk =
                    rowGroupFooter.mutable_rowgroupindexentry()
                            ->add_columnchunkindexentries();
            chunk->set_chunkoffset(body.size());
            chunk->set_chunklength(chunkBytes.size());
            if (kinds[column] == pixels::proto::Type_Kind_CHAR
                || kinds[column] == pixels::proto::Type_Kind_VARCHAR)
            {
                chunk->set_isnulloffset(
                        strings[variableIndex - 1U][0].size()
                        + strings[variableIndex - 1U][1].size());
            }
            else
            {
                chunk->set_isnulloffset(chunkBytes.size());
            }
            chunk->add_pixelpositions(0);
            pixels::proto::ColumnStatistic *pixelStatistic =
                    chunk->add_pixelstatistics()->mutable_statistic();
            pixelStatistic->set_numberofvalues(2);
            pixelStatistic->set_hasnull(false);
            chunk->set_littleendian(true);
            chunk->set_nullspadding(false);
            rowGroupFooter.mutable_rowgroupencoding()
                    ->add_columnchunkencodings()
                    ->set_kind(
                            pixels::proto::ColumnEncoding_Kind_NONE);
            body.insert(
                    body.end(), chunkBytes.begin(), chunkBytes.end());

            pixels::proto::ColumnStatistic *rowStatistic =
                    rowStatistics->add_columnchunkstats();
            rowStatistic->set_numberofvalues(2);
            rowStatistic->set_hasnull(false);
        }

        const std::uint64_t footerOffset = body.size();
        std::string serializedFooter;
        require(rowGroupFooter.SerializeToString(&serializedFooter),
                "unable to serialize TPC-H lineitem RowGroupFooter");
        body.insert(body.end(), serializedFooter.begin(),
                    serializedFooter.end());
        pixels::proto::RowGroupInformation *rowGroup =
                footer->add_rowgroupinfos();
        rowGroup->set_footeroffset(footerOffset);
        rowGroup->set_footerlength(serializedFooter.size());
        rowGroup->set_datalength(footerOffset - dataStart);
        rowGroup->set_numberofrows(2);
    };

    appendRowGroup(0);
    appendRowGroup(1);
    postScript->set_contentlength(body.size());

    std::string serializedTail;
    require(fileTail.SerializeToString(&serializedTail),
            "unable to serialize TPC-H lineitem FileTail");
    std::vector<std::uint8_t> file = body;
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

std::string decodePage(
        const std::vector<std::uint8_t> &file,
        std::uint32_t column, std::uint64_t offset,
        std::uint32_t count, std::uint32_t rowGroup = 0)
{
    Session session(file.size());
    driveMetadataFlexible(session, file);
    require(pixels_inspector_begin_page(
                    session.handle(), rowGroup, column, offset, count)
            == PIXELS_INSPECTOR_RANGE_READY,
            "generic fixture page did not request its footer");
    pixels_inspector_status status = PIXELS_INSPECTOR_RANGE_READY;
    std::size_t suppliedRanges = 0;
    while (status == PIXELS_INSPECTOR_RANGE_READY)
    {
        status = supply(session, nextRange(session), file);
        ++suppliedRanges;
        require(suppliedRanges < 64,
                "generic fixture page did not converge");
    }
    require(status == PIXELS_INSPECTOR_RESULT_READY,
            "generic fixture ranges did not produce a page");
    return readResult(session);
}

void verifyMultiPixelLongRoundTrip(
        const std::vector<std::uint8_t> &file)
{
    require(file.size() >= 8,
            "multi-pixel fixture has no tail pointer");
    std::uint64_t tailOffset = 0;
    for (std::size_t byte = file.size() - 8;
         byte < file.size(); ++byte)
    {
        tailOffset = (tailOffset << 8U) | file[byte];
    }
    require(tailOffset <= file.size() - 8,
            "multi-pixel fixture tail pointer is out of bounds");

    pixels::proto::FileTail tail;
    require(tail.ParseFromArray(
                    file.data() + static_cast<std::size_t>(tailOffset),
                    static_cast<int>(
                            file.size() - 8U
                            - static_cast<std::size_t>(tailOffset))),
            "unable to parse generated multi-pixel FileTail");
    require(tail.has_postscript()
            && tail.postscript().pixelstride() == 4
            && tail.postscript().numberofrows() == 10,
            "multi-pixel FileTail does not describe 10 rows at stride 4");
    require(tail.has_footer()
            && tail.footer().rowgroupinfos_size() == 1
            && tail.footer().types_size() == 1
            && tail.footer().columnstats_size() == 1
            && tail.footer().rowgroupstats_size() == 1
            && tail.footer().rowgroupstats(0)
                       .columnchunkstats_size() == 1,
            "multi-pixel fixture must contain one LONG column and row group");

    const pixels::proto::RowGroupInformation &rowGroup =
            tail.footer().rowgroupinfos(0);
    require(rowGroup.numberofrows() == 10
            && rowGroup.footeroffset() <= file.size()
            && rowGroup.footerlength()
               <= file.size() - rowGroup.footeroffset(),
            "multi-pixel row-group bounds or row count are invalid");
    pixels::proto::RowGroupFooter footer;
    require(footer.ParseFromArray(
                    file.data()
                    + static_cast<std::size_t>(rowGroup.footeroffset()),
                    static_cast<int>(rowGroup.footerlength())),
            "unable to parse generated multi-pixel RowGroupFooter");
    require(footer.has_rowgroupindexentry()
            && footer.rowgroupindexentry()
                       .columnchunkindexentries_size() == 1
            && footer.has_rowgroupencoding()
            && footer.rowgroupencoding()
                       .columnchunkencodings_size() == 1,
            "multi-pixel row group must contain one column chunk");
    const pixels::proto::ColumnChunkIndex &chunk =
            footer.rowgroupindexentry().columnchunkindexentries(0);
    const std::uint32_t expectedPositions[] = {0, 32, 64};
    const std::uint64_t expectedCounts[] = {4, 4, 2};
    require(chunk.pixelpositions_size() == 3
            && chunk.pixelstatistics_size() == 3,
            "multi-pixel row group must contain three Pixel segments");
    std::uint64_t coveredRows = 0;
    for (int pixel = 0; pixel < 3; ++pixel)
    {
        require(chunk.pixelpositions(pixel)
                        == expectedPositions[pixel],
                "multi-pixel segment has an incorrect start position");
        const pixels::proto::PixelStatistic &statistics =
                chunk.pixelstatistics(pixel);
        require(statistics.has_statistic()
                && statistics.statistic().has_numberofvalues()
                && statistics.statistic().numberofvalues()
                           == expectedCounts[pixel],
                "multi-pixel segment has an incorrect logical row count");
        coveredRows += statistics.statistic().numberofvalues();
    }
    require(coveredRows == rowGroup.numberofrows(),
            "multi-pixel segment row counts do not cover the row group");

    Session session(file.size());
    driveMetadataFlexible(session, file);
    const std::string metadata = readResult(session);
    require(metadata.find("\"pixelStride\":4") != std::string::npos
            && metadata.find("\"rowGroupCount\":1")
                       != std::string::npos,
            "Core metadata did not preserve multi-pixel shape");
    require(pixels_inspector_begin_row_group(session.handle(), 0)
            == PIXELS_INSPECTOR_RANGE_READY,
            "Core row-group inspection did not request the footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "Core did not parse the generated multi-pixel row group");
    const std::string layout = readResult(session);
    require(layout.find("\"position\":0") != std::string::npos
            && layout.find("\"position\":32") != std::string::npos
            && layout.find("\"position\":64") != std::string::npos
            && layout.find("\"numberOfValues\":\"2\"")
                       != std::string::npos,
            "Core row-group layout lost generated Pixel boundaries");

    require(decodePage(file, 0, 0, 10)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":10,"
               "\"values\":[\"0\",\"1\",\"2\",\"3\",\"4\",\"5\","
               "\"6\",\"7\",\"8\",\"9\"]}",
            "Core page decode did not round-trip multi-pixel values 0..9");
}

std::string driveOperation(
        Session &session, const std::vector<std::uint8_t> &file,
        pixels_inspector_status status)
{
    std::size_t suppliedRanges = 0;
    while (status == PIXELS_INSPECTOR_RANGE_READY)
    {
        status = supply(session, nextRange(session), file);
        ++suppliedRanges;
        require(suppliedRanges < 4096,
                "composite operation did not converge");
    }
    require(status == PIXELS_INSPECTOR_RESULT_READY,
            "composite operation did not produce a result (status "
            + std::to_string(status) + "): "
            + (status >= PIXELS_INSPECTOR_INVALID_ARGUMENT
               ? readError(session) : std::string()));
    return readResult(session);
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

void testFixedScalarTypePages()
{
    struct Case
    {
        pixels::proto::Type_Kind kind;
        const char *values;
    };
    const Case cases[] = {
            {pixels::proto::Type_Kind_BOOLEAN,
             "[true,false,true,true]"},
            {pixels::proto::Type_Kind_BYTE,
             "[\"-128\",\"-1\",\"0\",\"127\"]"},
            {pixels::proto::Type_Kind_SHORT,
             "[\"-32768\",\"-1\",\"0\",\"32767\"]"},
            {pixels::proto::Type_Kind_INT,
             "[\"-2147483648\",\"-1\",\"0\",\"2147483647\"]"},
            {pixels::proto::Type_Kind_LONG,
             "[\"-9223372036854775808\",\"-1\",\"0\","
             "\"9223372036854775807\"]"},
            {pixels::proto::Type_Kind_FLOAT,
             "[1.5,-2.25,\"NaN\",\"Infinity\"]"},
            {pixels::proto::Type_Kind_DOUBLE,
             "[1.5,-2.25,\"NaN\",\"Infinity\"]"},
            {pixels::proto::Type_Kind_TIMESTAMP,
             "[\"-9223372036854775808\",\"-1\",\"0\","
             "\"9223372036854775807\"]"},
            {pixels::proto::Type_Kind_DATE,
             "[\"-1\",\"0\",\"1\",\"20000\"]"},
            {pixels::proto::Type_Kind_TIME,
             "[\"0\",\"1\",\"86399999\",\"-1\"]"}};
    for (const Case &test : cases)
    {
        for (const bool littleEndian : {false, true})
        {
            const std::vector<std::uint8_t> file =
                    makeFixedScalarFixture(
                            test.kind, littleEndian);
            const std::string expected =
                    "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                    "\"count\":4,\"values\":"
                    + std::string(test.values) + "}";
            require(decodePage(file, 0, 0, 4) == expected,
                    "fixed scalar page differs from the golden");
        }
    }
}

void testRemainingVariableTypePages()
{
    const std::string stringExpected =
            "{\"rowGroup\":0,\"column\":0,\"offset\":4,\"count\":6,"
            "\"values\":[\"d\",\"ee\",null,\"f\",null,\"gg\"]}";
    require(decodePage(
                    makeMultiPixelVarcharFixture(
                            pixels::proto::Type_Kind_STRING),
                    0, 4, 6)
            == stringExpected,
            "STRING page differs from the golden");
    require(decodePage(
                    makeMultiPixelVarcharFixture(
                            pixels::proto::Type_Kind_CHAR),
                    0, 4, 6)
            == stringExpected,
            "CHAR page differs from the golden");

    require(decodePage(
                    makeBinaryFixture(
                            pixels::proto::Type_Kind_BINARY),
                    0, 0, 4)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":4,"
               "\"values\":[\"AP8=\",null,\"\",\"AQ==\"]}",
            "BINARY page differs from the golden");
}

void testCapabilities()
{
    std::uint64_t size = 0;
    require(pixels_inspector_capabilities_size(&size)
            == PIXELS_INSPECTOR_OK,
            "capability size is unavailable");
    require(size == EXPECTED_CAPABILITIES.size(),
            "capability size differs from the golden");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    require(pixels_inspector_copy_capabilities(
                    bytes.data(), bytes.size())
            == PIXELS_INSPECTOR_OK,
            "unable to copy capabilities");
    require(std::string(bytes.begin(), bytes.end())
            == EXPECTED_CAPABILITIES,
            "capabilities differ from the golden");
    require(pixels_inspector_capabilities_size(nullptr)
            == PIXELS_INSPECTOR_INVALID_ARGUMENT,
            "null capability size pointer was accepted");
    require(pixels_inspector_copy_capabilities(nullptr, size)
            == PIXELS_INSPECTOR_INVALID_ARGUMENT,
            "null capability destination was accepted");
    require(pixels_inspector_copy_capabilities(
                    bytes.data(), size - 1U)
            == PIXELS_INSPECTOR_BUFFER_TOO_SMALL,
            "short capability buffer was accepted");
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

void testRowGroupLayoutAndRepeatedPages()
{
    const std::vector<std::uint8_t> file = readFixture();
    Session session(file.size());
    driveMetadata(session, file);
    require(pixels_inspector_begin_row_group(session.handle(), 0)
            == PIXELS_INSPECTOR_RANGE_READY,
            "row-group layout did not request its footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "row-group footer did not produce layout");
    require(readResult(session) == EXPECTED_ROW_GROUP,
            "row-group layout differs from the golden");

    for (std::uint64_t offset : {0U, 2U})
    {
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, offset, 2)
                == PIXELS_INSPECTOR_RANGE_READY,
                "repeated page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RANGE_READY,
                "repeated page footer did not request values");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_RESULT_READY,
                "repeated page values did not produce a result");
    }
    require(readResult(session)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":2,\"count\":2,"
               "\"values\":[\"2\",\"3\"]}",
            "second page differs from the golden");
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
        verifyMultiPixelLongRoundTrip(file);
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

void testNestedPage()
{
    const std::vector<std::uint8_t> file = makeNestedFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    const std::string metadata = readResult(session);
    require(metadata.find(
                    "\"rows\":3,\"pixelStride\":10,"
                    "\"schemaCount\":7,\"rowGroupCount\":1")
            != std::string::npos
            && metadata.find(
                    "\"firstColumn\":{\"name\":\"root\",\"kind\":12}")
               != std::string::npos
            && metadata.find(
                    "{\"id\":0,\"name\":\"root\",\"kind\":12,"
                    "\"subtypes\":[1,3,6]}")
               != std::string::npos,
            "nested metadata differs from the golden");
    require(pixels_inspector_begin_page(
                    session.handle(), 0, 0, 0, 3)
            == PIXELS_INSPECTOR_RANGE_READY,
            "nested page did not request its footer");

    pixels_inspector_status status = PIXELS_INSPECTOR_RANGE_READY;
    std::size_t suppliedRanges = 0;
    while (status == PIXELS_INSPECTOR_RANGE_READY)
    {
        status = supply(session, nextRange(session), file);
        ++suppliedRanges;
        require(suppliedRanges < 32,
                "nested page did not converge");
    }
    require(status == PIXELS_INSPECTOR_RESULT_READY,
            "nested ranges did not produce a page");
    require(readResult(session)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":3,"
               "\"values\":[{\"tags\":[\"10\",\"11\"],"
               "\"attrs\":[[\"1\",\"a\"]],\"label\":\"x\"},null,"
               "{\"tags\":[\"13\",\"14\"],\"attrs\":[],"
               "\"label\":\"z\"}]}",
            "nested page differs from the golden");
}

void testTpchLineitemFixture()
{
    const std::vector<std::uint8_t> file =
            makeTpchLineitemFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    const std::string metadata = readResult(session);
    require(metadata.find(
                    "\"schemaCount\":16,\"rowGroupCount\":2")
            != std::string::npos,
            "TPC-H lineitem metadata has the wrong shape");
    require(metadata.find(
                    "{\"id\":0,\"name\":\"l_orderkey\",\"kind\":4")
            != std::string::npos
            && metadata.find(
                    "{\"id\":15,\"name\":\"l_comment\",\"kind\":16")
               != std::string::npos,
            "TPC-H lineitem schema inventory differs");

    require(pixels_inspector_begin_row_group(session.handle(), 1)
            == PIXELS_INSPECTOR_RANGE_READY,
            "TPC-H lineitem layout did not request its footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RESULT_READY,
            "TPC-H lineitem footer did not produce layout");
    const std::string layout = readResult(session);
    require(layout.find("\"rowGroup\":1") != std::string::npos
            && layout.find("\"column\":15") != std::string::npos,
            "TPC-H lineitem layout omits its final column");

    require(decodePage(file, 0, 0, 2)
            == "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
               "\"count\":2,\"values\":[\"11\",\"12\"]}",
            "TPC-H lineitem LONG preview differs");
    require(decodePage(file, 15, 0, 2)
            == "{\"rowGroup\":0,\"column\":15,\"offset\":0,"
               "\"count\":2,\"values\":[\"first line\","
               "\"second line\"]}",
            "TPC-H lineitem VARCHAR preview differs");
    require(decodePage(file, 0, 0, 2, 1)
            == "{\"rowGroup\":1,\"column\":0,\"offset\":0,"
               "\"count\":2,\"values\":[\"13\",\"14\"]}",
            "TPC-H lineitem second row-group LONG preview differs");
    require(decodePage(file, 15, 0, 2, 1)
            == "{\"rowGroup\":1,\"column\":15,\"offset\":0,"
               "\"count\":2,\"values\":[\"third line\","
               "\"fourth line\"]}",
            "TPC-H lineitem second row-group VARCHAR preview differs");
}

void testRowProjection()
{
    const std::vector<std::uint8_t> file = readFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    const std::uint32_t columns[] = {0, 1, 3};
    const std::string result = driveOperation(
            session, file,
            pixels_inspector_begin_rows(
                    session.handle(), 0, columns, 3, 2, 3));
    require(result.find(
                    "{\"operation\":\"rows-v1\",\"rowGroup\":0,"
                    "\"offset\":\"2\",\"count\":3")
            == 0,
            "row projection header differs");
    require(result.find(
                    "\"columns\":[{\"id\":0,\"name\":\"id\",\"kind\":4},"
                    "{\"id\":1,\"name\":\"name\",\"kind\":16},"
                    "{\"id\":3,\"name\":\"score\",\"kind\":14}]")
            != std::string::npos,
            "row projection descriptors differ");
    for (std::uint32_t row = 2; row < 5; ++row)
    {
        const std::string identity =
                "{\"rowGroup\":0,\"localRow\":\""
                + std::to_string(row)
                + "\",\"absoluteRow\":\""
                + std::to_string(row) + "\",\"values\":[";
        require(result.find(identity) != std::string::npos,
                "row projection lost stable row identity");
    }

    {
        Session invalid(file.size());
        driveMetadataFlexible(invalid, file);
        const std::uint32_t duplicate[] = {0, 0};
        require(pixels_inspector_begin_rows(
                        invalid.handle(), 0, duplicate, 2, 0, 1)
                == PIXELS_INSPECTOR_INVALID_ARGUMENT,
                "duplicate row projection was accepted");
    }
    {
        Session invalid(file.size());
        driveMetadataFlexible(invalid, file);
        require(pixels_inspector_begin_rows(
                        invalid.handle(), 0, columns, 3, 0, 501)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "row projection above 500 rows was accepted");
    }
    {
        const std::vector<std::uint8_t> nested = makeNestedFixture();
        Session invalid(nested.size());
        driveMetadataFlexible(invalid, nested);
        const std::uint32_t child[] = {2};
        require(pixels_inspector_begin_rows(
                        invalid.handle(), 0, child, 1, 0, 1)
                == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
                "nested child was accepted as a root projection");
    }
}

void testWholeFileFilter()
{
    const std::vector<std::uint8_t> file = readFixture();
    const std::uint32_t columns[] = {0};
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::string literal = "8";
        const std::string result = driveOperation(
                session, file,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_GE,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        nullptr, 0, 100));
        require(result
                == "{\"operation\":\"filter-v1\",\"columns\":["
                   "{\"id\":0,\"name\":\"id\",\"kind\":4}],\"rows\":["
                   "{\"rowGroup\":0,\"localRow\":\"8\","
                   "\"absoluteRow\":\"8\",\"values\":[\"8\"]},"
                   "{\"rowGroup\":0,\"localRow\":\"9\","
                   "\"absoluteRow\":\"9\",\"values\":[\"9\"]}],"
                   "\"progress\":{\"scannedRowGroups\":1,"
                   "\"prunedRowGroups\":0,\"scannedRows\":\"10\","
                   "\"prunedRows\":\"0\"},\"matched\":2,"
                   "\"completed\":true,\"truncated\":false,"
                   "\"cursor\":null}",
                "whole-file filter result differs from the golden");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::string literal = "0";
        const std::string first = driveOperation(
                session, file,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_GE,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        nullptr, 0, 1));
        require(first.find("\"matched\":1,\"completed\":false,"
                           "\"truncated\":true,\"cursor\":\"v1:0:1\"}")
                != std::string::npos,
                "filter did not return a bounded continuation");

        const std::string cursor = "v1:0:1";
        const std::string second = driveOperation(
                session, file,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_GE,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        reinterpret_cast<const std::uint8_t *>(
                                cursor.data()),
                        cursor.size(), 1));
        require(second.find("\"localRow\":\"1\"")
                != std::string::npos
                && second.find("\"cursor\":\"v1:0:2\"")
                   != std::string::npos,
                "filter continuation did not resume exactly");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::string literal = "20";
        const std::string result = driveOperation(
                session, file,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_EQ,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        nullptr, 0, 0));
        require(result.find(
                    "\"scannedRowGroups\":0,\"prunedRowGroups\":1,"
                    "\"scannedRows\":\"0\",\"prunedRows\":\"10\"")
                != std::string::npos,
                "filter did not conservatively prune impossible statistics");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::uint32_t stringColumns[] = {0, 1};
        const std::string literal = "Alice";
        const std::string result = driveOperation(
                session, file,
                pixels_inspector_begin_filter(
                        session.handle(), 1,
                        PIXELS_INSPECTOR_FILTER_CONTAINS,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), stringColumns, 2,
                        nullptr, 0, 100));
        require(result.find("\"matched\":1")
                != std::string::npos
                && result.find("\"values\":[\"4\",\"Alice\"]")
                   != std::string::npos,
                "case-sensitive contains filter differs");
    }
    {
        std::vector<std::uint8_t> nullable =
                makeByteRleFixture();
        rewriteFileTail(
                nullable, [](pixels::proto::FileTail &tail) {
                    tail.mutable_footer()
                            ->mutable_rowgroupstats(0)
                            ->mutable_columnchunkstats(0)
                            ->set_hasnull(true);
                });
        Session session(nullable.size());
        driveMetadataFlexible(session, nullable);
        const std::string result = driveOperation(
                session, nullable,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_IS_NULL,
                        nullptr, 0, columns, 1,
                        nullptr, 0, 100));
        require(result.find("\"localRow\":\"1\"")
                != std::string::npos,
                "is-null filter did not match the null row");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::string literal = "01";
        require(pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_EQ,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        nullptr, 0, 100)
                == PIXELS_INSPECTOR_INVALID_ARGUMENT,
                "non-canonical integer literal was accepted");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::string literal = "0";
        const std::string cursor = "v1:00:1";
        require(pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_EQ,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        reinterpret_cast<const std::uint8_t *>(
                                cursor.data()),
                        cursor.size(), 100)
                == PIXELS_INSPECTOR_INVALID_ARGUMENT,
                "non-canonical filter cursor was accepted");
    }
    require(pixels_inspector_begin_rows(
                    0, 0, columns, 1, 0, 1)
            == PIXELS_INSPECTOR_INVALID_HANDLE,
            "unknown rows-v1 handle was accepted");
    require(pixels_inspector_begin_rows(
                    0, 0, nullptr, 1, 0, 1)
            == PIXELS_INSPECTOR_INVALID_ARGUMENT,
            "null rows-v1 projection pointer was accepted");
    require(pixels_inspector_begin_filter(
                    0, 0, PIXELS_INSPECTOR_FILTER_EQ,
                    nullptr, 1, columns, 1,
                    nullptr, 0, 100)
            == PIXELS_INSPECTOR_INVALID_ARGUMENT,
            "null filter literal pointer was accepted");

    {
        const std::vector<std::uint8_t> lineitem =
                makeTpchLineitemFixture();
        Session session(lineitem.size());
        driveMetadataFlexible(session, lineitem);
        const std::uint32_t projection[] = {0, 15};
        const std::string literal = "13";
        const std::string result = driveOperation(
                session, lineitem,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_GE,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), projection, 2,
                        nullptr, 0, 100));
        require(result.find(
                    "{\"rowGroup\":1,\"localRow\":\"0\","
                    "\"absoluteRow\":\"2\",\"values\":[\"13\","
                    "\"third line\"]}")
                != std::string::npos
                && result.find(
                    "{\"rowGroup\":1,\"localRow\":\"1\","
                    "\"absoluteRow\":\"3\",\"values\":[\"14\","
                    "\"fourth line\"]}")
                   != std::string::npos,
                "whole-file filter lost cross-row-group ordering");
    }
}

void testScanV2()
{
    const std::vector<std::uint8_t> file = readFixture();
    const std::vector<TestScanNode> all = {TestScanNode{}};
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::vector<std::uint8_t> packet =
                makeScanPlan(true, {}, all, {}, 0, 0);
        const std::string result = driveOperation(
                session, file, pixels_inspector_begin_scan(
                        session.handle(), packet.data(),
                        static_cast<std::uint32_t>(packet.size())));
        require(result.find(
                    "{\"operation\":\"scan-v2\",\"columns\":["
                    "{\"id\":0,\"name\":\"id\",\"kind\":4}")
                == 0
                && result.find("\"limit\":20,\"returned\":10")
                   != std::string::npos
                && result.find(
                    "\"ordered\":false,\"completion\":\"complete\"")
                   != std::string::npos,
                "default scan-v2 plan differs");
        std::uint8_t progress[PIXELS_INSPECTOR_SCAN_PROGRESS_V1_BYTES] = {};
        require(pixels_inspector_copy_scan_progress(
                        session.handle(), progress, sizeof(progress))
                == PIXELS_INSPECTOR_OK
                && progress[0] == 1 && progress[4] == 3,
                "scan-v2 terminal progress snapshot differs");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::vector<std::uint8_t> packet =
                makeScanPlan(false, {0}, all, {}, 2, 3);
        const std::string result = driveOperation(
                session, file, pixels_inspector_begin_scan(
                        session.handle(), packet.data(),
                        static_cast<std::uint32_t>(packet.size())));
        require(result.find("\"localRow\":\"2\"")
                != std::string::npos
                && result.find("\"localRow\":\"4\"")
                   != std::string::npos,
                "natural scan offset/limit is not stable");
        const std::string marker = "\"cursor\":\"";
        const std::size_t cursorStart =
                result.find(marker) + marker.size();
        const std::size_t cursorEnd =
                result.find('"', cursorStart);
        require(cursorStart >= marker.size()
                && cursorEnd != std::string::npos
                && cursorEnd - cursorStart == 54,
                "natural scan cursor is not opaque PXC2");
        const std::string cursor =
                result.substr(cursorStart, cursorEnd - cursorStart);

        const std::vector<std::uint8_t> continuation =
                makeScanPlan(false, {0}, all, {}, 0, 2, cursor);
        const std::string next = driveOperation(
                session, file, pixels_inspector_begin_scan(
                        session.handle(), continuation.data(),
                        static_cast<std::uint32_t>(
                                continuation.size())));
        require(next.find("\"localRow\":\"5\"")
                != std::string::npos
                && next.find("\"localRow\":\"6\"")
                   != std::string::npos,
                "natural scan continuation has a gap or duplicate");
        const std::vector<std::uint8_t> changedPlan =
                makeScanPlan(false, {1}, all, {}, 0, 2, cursor);
        require(pixels_inspector_begin_scan(
                        session.handle(), changedPlan.data(),
                        static_cast<std::uint32_t>(
                                changedPlan.size()))
                == PIXELS_INSPECTOR_INVALID_ARGUMENT,
                "PXC2 cursor was accepted for a changed plan");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::vector<TestScanNode> expression = {
                {1, PIXELS_INSPECTOR_FILTER_GE, 0, 0, "8"},
                {1, PIXELS_INSPECTOR_FILTER_LT, 0, 0, "9"},
                {2, 0, 2, 0, ""}};
        const std::vector<std::uint8_t> packet =
                makeScanPlan(false, {0}, expression, {}, 0, 20);
        const std::string result = driveOperation(
                session, file, pixels_inspector_begin_scan(
                        session.handle(), packet.data(),
                        static_cast<std::uint32_t>(packet.size())));
        require(result.find("\"returned\":1")
                != std::string::npos
                && result.find("\"values\":[\"8\"]")
                   != std::string::npos,
                "scan-v2 composite AND differs from 3VL semantics");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::vector<TestScanNode> expression = {
                {1, PIXELS_INSPECTOR_FILTER_EQ, 0, 0, "20"}};
        const std::vector<std::uint8_t> packet =
                makeScanPlan(false, {0}, expression, {}, 0, 20);
        const std::string result = driveOperation(
                session, file, pixels_inspector_begin_scan(
                        session.handle(), packet.data(),
                        static_cast<std::uint32_t>(packet.size())));
        require(result.find("\"prunedRowGroups\":1")
                != std::string::npos
                && result.find("\"prunedRows\":\"10\"")
                   != std::string::npos,
                "scan-v2 did not conservatively prune impossible stats");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::vector<TestScanNode> expression = {
                {1, PIXELS_INSPECTOR_FILTER_CONTAINS, 0, 1, ""}};
        const std::vector<std::uint8_t> packet =
                makeScanPlan(false, {0, 1}, expression, {}, 0, 20);
        const std::string result = driveOperation(
                session, file, pixels_inspector_begin_scan(
                        session.handle(), packet.data(),
                        static_cast<std::uint32_t>(packet.size())));
        require(result.find("\"returned\":10")
                != std::string::npos,
                "empty-string scan predicate was rejected or misread");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::vector<TestScanOrder> order = {{0, 1, 1}};
        const std::vector<std::uint8_t> packet =
                makeScanPlan(false, {0}, all, order, 0, 3);
        const std::string result = driveOperation(
                session, file, pixels_inspector_begin_scan(
                        session.handle(), packet.data(),
                        static_cast<std::uint32_t>(packet.size())));
        require(result.find(
                    "\"values\":[\"9\"]},{\"rowGroup\":0,"
                    "\"localRow\":\"8\"")
                != std::string::npos
                && result.find("\"localRow\":\"7\"")
                   != std::string::npos
                && result.find("\"ordered\":true")
                   != std::string::npos,
                "ordered scan Top-K result differs");
        const std::string marker = "\"cursor\":\"";
        const std::size_t cursorStart =
                result.find(marker) + marker.size();
        const std::size_t cursorEnd =
                result.find('"', cursorStart);
        require(cursorStart >= marker.size()
                && cursorEnd != std::string::npos
                && cursorEnd - cursorStart == 54,
                "ordered scan did not emit an opaque PXC2 cursor");
        const std::string cursor =
                result.substr(cursorStart, cursorEnd - cursorStart);
        const std::vector<std::uint8_t> continuation =
                makeScanPlan(false, {0}, all, order, 0, 3, cursor);
        const std::string next = driveOperation(
                session, file, pixels_inspector_begin_scan(
                        session.handle(), continuation.data(),
                        static_cast<std::uint32_t>(
                                continuation.size())));
        require(next.find("\"values\":[\"6\"]")
                != std::string::npos
                && next.find("\"values\":[\"5\"]")
                   != std::string::npos
                && next.find("\"values\":[\"4\"]")
                   != std::string::npos,
                "ordered PXC2 continuation has a gap or duplicate");
    }
    {
        Session session(file.size());
        driveMetadataFlexible(session, file);
        std::vector<std::uint8_t> malformed =
                makeScanPlan(true, {}, all, {}, 0, 20);
        malformed[44] = 1;
        require(pixels_inspector_begin_scan(
                        session.handle(), malformed.data(),
                        static_cast<std::uint32_t>(malformed.size()))
                == PIXELS_INSPECTOR_INVALID_ARGUMENT,
                "nonzero scan reserved field was accepted");
    }
}

void testCompatibilityMetadata()
{
    const pixels::proto::CompressionKind compressions[] = {
            pixels::proto::NONE, pixels::proto::ZLIB,
            pixels::proto::SNAPPY, pixels::proto::LZO,
            pixels::proto::LZ4, pixels::proto::ZSTD};
    for (const pixels::proto::CompressionKind compression :
         compressions)
    {
        std::vector<std::uint8_t> file = readFixture();
        rewriteFileTail(
                file, [compression](pixels::proto::FileTail &tail) {
                    tail.mutable_postscript()->set_compression(
                            compression);
                });
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::string marker =
                "\"compression\":"
                + std::to_string(static_cast<int>(compression));
        require(readResult(session).find(marker)
                != std::string::npos,
                "compression enum metadata was not preserved");
        require(decodePage(file, 0, 0, 2)
                == "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                   "\"count\":2,\"values\":[\"0\",\"1\"]}",
                "inactive compression metadata changed payload decoding");
    }

    const std::vector<std::string> timezones = {
            "", "UTC", "America/Shanghai",
            "Pacific Standard Time", "not/a-zone"};
    for (const std::string &timezone : timezones)
    {
        std::vector<std::uint8_t> file = readFixture();
        rewriteFileTail(
                file, [&timezone](pixels::proto::FileTail &tail) {
                    tail.mutable_postscript()->set_writertimezone(
                            timezone);
                });
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(readResult(session).find(
                    "\"writerTimezone\":\""
                    + timezone + "\"")
                != std::string::npos,
                "writer timezone provenance was not preserved");
        require(decodePage(file, 0, 0, 1).find(
                    "\"values\":[\"0\"]")
                != std::string::npos,
                "writer timezone changed stored scalar values");
    }
}

void testDecimalAndTimestampBoundaries()
{
    struct DecimalCase
    {
        std::uint32_t precision;
        std::uint32_t scale;
        const char *expected;
    };
    const DecimalCase shortCases[] = {
            {1, 0, "[\"9\",\"-9\",\"0\",\"1\"]"},
            {18, 18,
             "[\"0.000000000000000001\","
             "\"-0.000000000000000001\","
             "\"0.000000000000000000\","
             "\"0.999999999999999999\"]"}};
    for (const bool littleEndian : {false, true})
    {
        for (const DecimalCase &test : shortCases)
        {
            const std::vector<std::int64_t> values =
                    test.precision == 1
                    ? std::vector<std::int64_t>{9, -9, 0, 1}
                    : std::vector<std::int64_t>{
                            1, -1, 0, 999999999999999999LL};
            const std::string result = decodePage(
                    makeShortDecimalFixture(
                            test.precision, test.scale,
                            littleEndian, values),
                    0, 0, 4);
            require(result.find(test.expected) != std::string::npos,
                    "short DECIMAL boundary differs");
        }
        for (const DecimalCase &test :
             std::vector<DecimalCase>{
                     {19, 0, "[\"123456\",\"-1\"]"},
                     {38, 4, "[\"12.3456\",\"-0.0001\"]"}})
        {
            const std::string result = decodePage(
                    makeLongDecimalFixture(
                            test.precision, test.scale,
                            littleEndian),
                    0, 0, 2);
            require(result.find(test.expected) != std::string::npos,
                    "long DECIMAL boundary differs");
        }
    }

    for (std::uint32_t precision = 0; precision <= 6; ++precision)
    {
        for (const bool littleEndian : {false, true})
        {
            std::vector<std::uint8_t> file =
                    makeFixedScalarFixture(
                            pixels::proto::Type_Kind_TIMESTAMP,
                            littleEndian);
            rewriteFileTail(
                    file, [precision](pixels::proto::FileTail &tail) {
                        tail.mutable_footer()
                                ->mutable_types(0)
                                ->set_precision(precision);
                    });
            Session session(file.size());
            driveMetadataFlexible(session, file);
            require(readResult(session).find(
                        "\"precision\":"
                        + std::to_string(precision))
                    != std::string::npos,
                    "TIMESTAMP precision metadata was not preserved");
            require(decodePage(file, 0, 0, 4).find(
                        "\"values\":[\"-9223372036854775808\","
                        "\"-1\",\"0\",\"9223372036854775807\"]")
                    != std::string::npos,
                    "TIMESTAMP precision changed stored microseconds");
        }
    }

    std::vector<std::uint8_t> rle =
            makeMultiPixelLongFixture(
                    false, {0, 0, 0},
                    std::function<void(
                            pixels::proto::RowGroupFooter &)>(), true);
    rewriteFileTail(
            rle, [](pixels::proto::FileTail &tail) {
                pixels::proto::Type *type =
                        tail.mutable_footer()->mutable_types(0);
                type->set_kind(
                        pixels::proto::Type_Kind_TIMESTAMP);
                type->set_precision(6);
            });
    require(decodePage(rle, 0, 2, 5).find(
                "\"values\":[\"2\",\"3\",\"4\",\"5\",\"6\"]")
            != std::string::npos,
            "RUNLENGTH TIMESTAMP page differs");
}

void testSupportedEncodingMatrix()
{
    const pixels::proto::Type_Kind rleKinds[] = {
            pixels::proto::Type_Kind_SHORT,
            pixels::proto::Type_Kind_INT,
            pixels::proto::Type_Kind_LONG,
            pixels::proto::Type_Kind_DATE,
            pixels::proto::Type_Kind_TIME,
            pixels::proto::Type_Kind_TIMESTAMP};
    for (const pixels::proto::Type_Kind kind : rleKinds)
    {
        std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(
                        false, {0, 0, 0},
                        std::function<void(
                                pixels::proto::RowGroupFooter &)>(), true);
        rewriteFileTail(
                file, [kind](pixels::proto::FileTail &tail) {
                    pixels::proto::Type *type =
                            tail.mutable_footer()->mutable_types(0);
                    type->set_kind(kind);
                    if (kind == pixels::proto::Type_Kind_TIMESTAMP)
                    {
                        type->set_precision(6);
                    }
                });
        require(decodePage(file, 0, 0, 10).find(
                    "\"values\":[\"0\",\"1\",\"2\",\"3\",\"4\","
                    "\"5\",\"6\",\"7\",\"8\",\"9\"]")
                != std::string::npos,
                "supported RUNLENGTH type differs");
    }

    const pixels::proto::Type_Kind dictionaryKinds[] = {
            pixels::proto::Type_Kind_STRING,
            pixels::proto::Type_Kind_VARCHAR,
            pixels::proto::Type_Kind_CHAR};
    for (const bool cascadeRle : {false, true})
    {
        for (const pixels::proto::Type_Kind kind : dictionaryKinds)
        {
            std::vector<std::uint8_t> file =
                    makeDictionaryFixture(cascadeRle);
            rewriteFileTail(
                    file, [kind](pixels::proto::FileTail &tail) {
                        tail.mutable_footer()
                                ->mutable_types(0)
                                ->set_kind(kind);
                    });
            require(decodePage(file, 0, 0, 4).find(
                        "\"values\":[\"cat\",null,\"dog\",\"cat\"]")
                    != std::string::npos,
                    "supported DICTIONARY type differs");
        }
    }

    {
        std::vector<std::uint8_t> file =
                makeFixedScalarFixture(
                        pixels::proto::Type_Kind_DOUBLE, true);
        rewriteFileTail(
                file, [](pixels::proto::FileTail &tail) {
                    tail.mutable_footer()
                            ->mutable_types(0)
                            ->set_kind(
                                    pixels::proto::Type_Kind_DOUBLE);
                });
        // A declared but invalid type/encoding pair must fail explicitly.
        std::uint64_t tailOffset = 0;
        for (std::size_t byte = file.size() - 8;
             byte < file.size(); ++byte)
        {
            tailOffset = (tailOffset << 8U) | file[byte];
        }
        pixels::proto::FileTail tail;
        require(tail.ParseFromArray(
                        file.data() + tailOffset,
                        static_cast<int>(
                                file.size() - tailOffset - 8U)),
                "unable to parse invalid-pair FileTail");
        const pixels::proto::RowGroupInformation &information =
                tail.footer().rowgroupinfos(0);
        pixels::proto::RowGroupFooter footer;
        require(footer.ParseFromArray(
                        file.data() + information.footeroffset(),
                        information.footerlength()),
                "unable to parse invalid-pair footer");
        footer.mutable_rowgroupencoding()
                ->mutable_columnchunkencodings(0)
                ->set_kind(
                        pixels::proto::ColumnEncoding_Kind_RUNLENGTH);
        std::string serialized;
        require(footer.SerializeToString(&serialized)
                && serialized.size() == information.footerlength(),
                "invalid-pair footer changed size");
        std::copy(serialized.begin(), serialized.end(),
                  file.begin() + information.footeroffset());
        Session session(file.size());
        driveMetadataFlexible(session, file);
        require(pixels_inspector_begin_page(
                        session.handle(), 0, 0, 0, 1)
                == PIXELS_INSPECTOR_RANGE_READY,
                "invalid type/encoding pair did not request footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_UNSUPPORTED_ENCODING,
                "invalid type/encoding pair was not rejected explicitly");
    }
}

void testDecimalAndTimestampOperations()
{
    {
        const std::vector<std::uint8_t> file =
                makeShortDecimalFixture(
                        18, 18, true,
                        {1, -1, 0, 999999999999999999LL});
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::uint32_t columns[] = {0};
        const std::string rows = driveOperation(
                session, file,
                pixels_inspector_begin_rows(
                        session.handle(), 0, columns, 1, 0, 4));
        require(rows.find(
                    "\"values\":[\"0.000000000000000001\"]")
                != std::string::npos
                && rows.find(
                    "\"values\":[\"0.999999999999999999\"]")
                   != std::string::npos,
                "DECIMAL row projection lost exact scale");
        const std::string literal = "0";
        const std::string filtered = driveOperation(
                session, file,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_GE,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        nullptr, 0, 100));
        require(filtered.find("\"matched\":3")
                != std::string::npos,
                "DECIMAL typed comparison differs");
    }
    {
        std::vector<std::uint8_t> file =
                makeFixedScalarFixture(
                        pixels::proto::Type_Kind_TIMESTAMP, true);
        rewriteFileTail(
                file, [](pixels::proto::FileTail &tail) {
                    tail.mutable_footer()
                            ->mutable_types(0)
                            ->set_precision(6);
                    tail.mutable_postscript()->set_writertimezone(
                            "America/Shanghai");
                });
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::uint32_t columns[] = {0};
        const std::string literal = "0";
        const std::string filtered = driveOperation(
                session, file,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_LT,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        nullptr, 0, 100));
        require(filtered.find("\"matched\":2")
                != std::string::npos
                && filtered.find(
                    "\"values\":[\"-9223372036854775808\"]")
                   != std::string::npos,
                "TIMESTAMP typed comparison changed exact microseconds");
    }
    {
        const std::vector<std::uint8_t> file =
                makeFixedScalarFixture(
                        pixels::proto::Type_Kind_DOUBLE, true);
        Session session(file.size());
        driveMetadataFlexible(session, file);
        const std::uint32_t columns[] = {0};
        const std::string literal = "0";
        const std::string filtered = driveOperation(
                session, file,
                pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_GT,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        nullptr, 0, 100));
        require(filtered.find("\"matched\":2")
                != std::string::npos
                && filtered.find("\"values\":[1.5]")
                   != std::string::npos
                && filtered.find("\"values\":[\"Infinity\"]")
                   != std::string::npos,
                "floating predicate special-value semantics differ");
    }
}

void testNestedCancellation()
{
    const std::vector<std::uint8_t> file = makeNestedFixture();
    Session session(file.size());
    driveMetadataFlexible(session, file);
    require(pixels_inspector_begin_page(
                    session.handle(), 0, 0, 0, 3)
            == PIXELS_INSPECTOR_RANGE_READY,
            "nested cancellation page did not request its footer");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "nested footer did not request the root bitmap");
    require(supply(session, nextRange(session), file)
            == PIXELS_INSPECTOR_RANGE_READY,
            "nested root did not request child metadata");
    require(pixels_inspector_cancel(session.handle())
            == PIXELS_INSPECTOR_CANCELLED,
            "nested child wait was not cancellable");
}

void testMalformedNestedRange()
{
    std::vector<std::uint8_t> file = makeNestedFixture();
    require(file.size() > 41, "nested fixture is unexpectedly short");
    file[41] = 9;
    Session session(file.size());
    driveMetadataFlexible(session, file);
    require(pixels_inspector_begin_page(
                    session.handle(), 0, 0, 0, 3)
            == PIXELS_INSPECTOR_RANGE_READY,
            "malformed nested page did not request its footer");
    pixels_inspector_status status = PIXELS_INSPECTOR_RANGE_READY;
    while (status == PIXELS_INSPECTOR_RANGE_READY)
    {
        status = supply(session, nextRange(session), file);
    }
    require(status == PIXELS_INSPECTOR_OUT_OF_BOUNDS,
            "collection range beyond its child rows was accepted");
}

void testSchemaValidation()
{
    pixels::format::FormatError error;
    {
        pixels::proto::Footer footer;
        pixels::proto::Type *root = footer.add_types();
        root->set_kind(pixels::proto::Type_Kind_STRUCT);
        root->set_name("root");
        root->add_subtypes(1);
        root->add_subtypes(2);
        pixels::proto::Type *first = footer.add_types();
        first->set_kind(pixels::proto::Type_Kind_LONG);
        first->set_name("duplicate");
        pixels::proto::Type *second = footer.add_types();
        second->set_kind(pixels::proto::Type_Kind_LONG);
        second->set_name("duplicate");
        require(!pixels::format::SchemaValidator::validate(
                        footer, error)
                && error.code
                   == pixels::format::ErrorCode::MALFORMED_PROTOBUF,
                "duplicate STRUCT field names were accepted");
    }
    {
        pixels::proto::Footer footer;
        pixels::proto::Type *left = footer.add_types();
        left->set_kind(pixels::proto::Type_Kind_ARRAY);
        left->set_name("left");
        left->add_subtypes(2);
        pixels::proto::Type *right = footer.add_types();
        right->set_kind(pixels::proto::Type_Kind_ARRAY);
        right->set_name("right");
        right->add_subtypes(2);
        pixels::proto::Type *shared = footer.add_types();
        shared->set_kind(pixels::proto::Type_Kind_LONG);
        shared->set_name("shared");
        require(!pixels::format::SchemaValidator::validate(
                        footer, error)
                && error.code
                   == pixels::format::ErrorCode::MALFORMED_PROTOBUF,
                "shared nested child was accepted");
    }
    {
        pixels::proto::Footer footer;
        for (std::uint32_t depth = 0; depth < 33; ++depth)
        {
            pixels::proto::Type *array = footer.add_types();
            array->set_kind(pixels::proto::Type_Kind_ARRAY);
            array->set_name("level");
            array->add_subtypes(depth + 1U);
        }
        pixels::proto::Type *leaf = footer.add_types();
        leaf->set_kind(pixels::proto::Type_Kind_LONG);
        leaf->set_name("leaf");
        require(!pixels::format::SchemaValidator::validate(
                        footer, error)
                && error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
                "schema above the nesting-depth limit was accepted");
    }
    {
        pixels::proto::Footer footer;
        pixels::proto::Type *value = footer.add_types();
        value->set_kind(pixels::proto::Type_Kind_VARBINARY);
        value->set_name("oversized");
        value->set_maximumlength(16777217);
        require(!pixels::format::SchemaValidator::validate(
                        footer, error)
                && error.code
                   == pixels::format::ErrorCode::MALFORMED_PROTOBUF,
                "schema above the variable-value limit was accepted");
    }
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
    {
        Session session(file.size());
        driveMetadata(session, file);
        const std::uint32_t columns[] = {0, 1};
        require(pixels_inspector_begin_rows(
                        session.handle(), 0, columns, 2, 0, 2)
                == PIXELS_INSPECTOR_RANGE_READY,
                "row projection did not request its first child");
        require(pixels_inspector_cancel(session.handle())
                == PIXELS_INSPECTOR_CANCELLED,
                "row projection was not cancellable");
    }
    {
        Session session(file.size());
        driveMetadata(session, file);
        const std::uint32_t columns[] = {0};
        const std::string literal = "0";
        require(pixels_inspector_begin_filter(
                        session.handle(), 0,
                        PIXELS_INSPECTOR_FILTER_GE,
                        reinterpret_cast<const std::uint8_t *>(
                                literal.data()),
                        literal.size(), columns, 1,
                        nullptr, 0, 100)
                == PIXELS_INSPECTOR_RANGE_READY,
                "filter did not request its predicate page");
        require(pixels_inspector_cancel(session.handle())
                == PIXELS_INSPECTOR_CANCELLED,
                "filter was not cancellable");
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

void writeBytes(
        const std::filesystem::path &path,
        const std::vector<std::uint8_t> &bytes)
{
    std::ofstream output(path, std::ios::binary);
    require(output.good(), "unable to create conformance fixture");
    if (!bytes.empty())
    {
        output.write(
                reinterpret_cast<const char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    require(output.good(), "unable to write conformance fixture");
}

void writeConformanceCorpus(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    std::string manifest = "{\"abi\":4,\"cases\":[";
    bool first = true;
    const auto addCase =
            [&](const std::string &name, const std::string &fileName,
                std::uint32_t column, std::uint64_t offset,
                std::uint32_t count, const std::string &expected)
    {
        if (!first)
        {
            manifest += ",";
        }
        first = false;
        manifest += "{\"name\":\"" + name
                    + "\",\"file\":\"" + fileName
                    + "\",\"column\":" + std::to_string(column)
                    + ",\"offset\":" + std::to_string(offset)
                    + ",\"count\":" + std::to_string(count)
                    + ",\"expected\":" + expected + "}";
    };

    struct FixedCase
    {
        pixels::proto::Type_Kind kind;
        const char *name;
        const char *values;
    };
    const FixedCase fixedCases[] = {
            {pixels::proto::Type_Kind_BOOLEAN, "BOOLEAN",
             "[true,false,true,true]"},
            {pixels::proto::Type_Kind_BYTE, "BYTE",
             "[\"-128\",\"-1\",\"0\",\"127\"]"},
            {pixels::proto::Type_Kind_SHORT, "SHORT",
             "[\"-32768\",\"-1\",\"0\",\"32767\"]"},
            {pixels::proto::Type_Kind_INT, "INT",
             "[\"-2147483648\",\"-1\",\"0\",\"2147483647\"]"},
            {pixels::proto::Type_Kind_LONG, "LONG",
             "[\"-9223372036854775808\",\"-1\",\"0\","
             "\"9223372036854775807\"]"},
            {pixels::proto::Type_Kind_FLOAT, "FLOAT",
             "[1.5,-2.25,\"NaN\",\"Infinity\"]"},
            {pixels::proto::Type_Kind_DOUBLE, "DOUBLE",
             "[1.5,-2.25,\"NaN\",\"Infinity\"]"},
            {pixels::proto::Type_Kind_TIMESTAMP, "TIMESTAMP",
             "[\"-9223372036854775808\",\"-1\",\"0\","
             "\"9223372036854775807\"]"},
            {pixels::proto::Type_Kind_DATE, "DATE",
             "[\"-1\",\"0\",\"1\",\"20000\"]"},
            {pixels::proto::Type_Kind_TIME, "TIME",
             "[\"0\",\"1\",\"86399999\",\"-1\"]"}};
    for (const FixedCase &test : fixedCases)
    {
        const std::string fileName =
                std::string(test.name) + ".pxl";
        writeBytes(
                directory / fileName,
                makeFixedScalarFixture(test.kind, true));
        addCase(
                test.name, fileName, 0, 0, 4,
                "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                "\"count\":4,\"values\":"
                + std::string(test.values) + "}");
    }

    const std::string variableExpected =
            "{\"rowGroup\":0,\"column\":0,\"offset\":4,\"count\":6,"
            "\"values\":[\"d\",\"ee\",null,\"f\",null,\"gg\"]}";
    for (const std::pair<pixels::proto::Type_Kind, const char *> &test :
         std::vector<
                 std::pair<pixels::proto::Type_Kind, const char *>>{
                 {pixels::proto::Type_Kind_STRING, "STRING"},
                 {pixels::proto::Type_Kind_VARCHAR, "VARCHAR"},
                 {pixels::proto::Type_Kind_CHAR, "CHAR"}})
    {
        const std::string fileName =
                std::string(test.second) + ".pxl";
        writeBytes(
                directory / fileName,
                makeMultiPixelVarcharFixture(test.first));
        addCase(
                test.second, fileName, 0, 4, 6,
                variableExpected);
    }

    const std::string binaryExpected =
            "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":4,"
            "\"values\":[\"AP8=\",null,\"\",\"AQ==\"]}";
    for (const std::pair<pixels::proto::Type_Kind, const char *> &test :
         std::vector<
                 std::pair<pixels::proto::Type_Kind, const char *>>{
                 {pixels::proto::Type_Kind_BINARY, "BINARY"},
                 {pixels::proto::Type_Kind_VARBINARY, "VARBINARY"}})
    {
        const std::string fileName =
                std::string(test.second) + ".pxl";
        writeBytes(
                directory / fileName,
                makeBinaryFixture(test.first));
        addCase(
                test.second, fileName, 0, 0, 4,
                binaryExpected);
    }

    writeBytes(directory / "DECIMAL.pxl",
               makeLongDecimalFixture());
    addCase(
            "DECIMAL", "DECIMAL.pxl", 0, 0, 2,
            "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":2,"
            "\"values\":[\"12.3456\",\"-0.0001\"]}");
    writeBytes(directory / "VECTOR.pxl", makeVectorFixture());
    addCase(
            "VECTOR", "VECTOR.pxl", 0, 1, 2,
            "{\"rowGroup\":0,\"column\":0,\"offset\":1,\"count\":2,"
            "\"values\":[[3,4],[\"NaN\",\"Infinity\"]]}");

    writeBytes(directory / "NESTED.pxl", makeNestedFixture());
    addCase(
            "ARRAY", "NESTED.pxl", 1, 0, 3,
            "{\"rowGroup\":0,\"column\":1,\"offset\":0,\"count\":3,"
            "\"values\":[[\"10\",\"11\"],[\"12\"],[\"13\",\"14\"]]}");
    addCase(
            "MAP", "NESTED.pxl", 3, 0, 3,
            "{\"rowGroup\":0,\"column\":3,\"offset\":0,\"count\":3,"
            "\"values\":[[[\"1\",\"a\"]],"
            "[[\"2\",\"bb\"],[\"3\",\"c\"]],[]]}");
    addCase(
            "STRUCT", "NESTED.pxl", 0, 0, 3,
            "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":3,"
            "\"values\":[{\"tags\":[\"10\",\"11\"],"
            "\"attrs\":[[\"1\",\"a\"]],\"label\":\"x\"},null,"
            "{\"tags\":[\"13\",\"14\"],\"attrs\":[],"
            "\"label\":\"z\"}]}");

    manifest += "],\"compatibility\":[";
    first = true;

    writeBytes(directory / "RLE-BYTE.pxl", makeByteRleFixture());
    addCase(
            "RLE-BYTE", "RLE-BYTE.pxl", 0, 0, 5,
            "{\"rowGroup\":0,\"column\":0,\"offset\":0,\"count\":5,"
            "\"values\":[\"1\",null,\"1\",\"1\",\"-2\"]}");
    const std::pair<pixels::proto::Type_Kind, const char *> rleCases[] = {
            {pixels::proto::Type_Kind_SHORT, "SHORT"},
            {pixels::proto::Type_Kind_INT, "INT"},
            {pixels::proto::Type_Kind_LONG, "LONG"},
            {pixels::proto::Type_Kind_DATE, "DATE"},
            {pixels::proto::Type_Kind_TIME, "TIME"},
            {pixels::proto::Type_Kind_TIMESTAMP, "TIMESTAMP"}};
    for (const auto &test : rleCases)
    {
        std::vector<std::uint8_t> file =
                makeMultiPixelLongFixture(
                        false, {0, 0, 0},
                        std::function<void(
                                pixels::proto::RowGroupFooter &)>(), true);
        rewriteFileTail(
                file, [&test](pixels::proto::FileTail &tail) {
                    pixels::proto::Type *type =
                            tail.mutable_footer()->mutable_types(0);
                    type->set_kind(test.first);
                    if (test.first
                        == pixels::proto::Type_Kind_TIMESTAMP)
                    {
                        type->set_precision(6);
                    }
                });
        const std::string name =
                "RLE-" + std::string(test.second);
        const std::string fileName = name + ".pxl";
        writeBytes(directory / fileName, file);
        addCase(
                name, fileName, 0, 0, 10,
                "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                "\"count\":10,\"values\":[\"0\",\"1\",\"2\",\"3\","
                "\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"]}");
    }

    const std::pair<pixels::proto::Type_Kind, const char *>
            dictionaryCases[] = {
                    {pixels::proto::Type_Kind_STRING, "STRING"},
                    {pixels::proto::Type_Kind_VARCHAR, "VARCHAR"},
                    {pixels::proto::Type_Kind_CHAR, "CHAR"}};
    for (const bool cascade : {false, true})
    {
        for (const auto &test : dictionaryCases)
        {
            std::vector<std::uint8_t> file =
                    makeDictionaryFixture(cascade);
            rewriteFileTail(
                    file, [&test](pixels::proto::FileTail &tail) {
                        tail.mutable_footer()
                                ->mutable_types(0)
                                ->set_kind(test.first);
                    });
            const std::string name =
                    "DICTIONARY-"
                    + std::string(test.second)
                    + (cascade ? "-RLE" : "-PLAIN");
            const std::string fileName = name + ".pxl";
            writeBytes(directory / fileName, file);
            addCase(
                    name, fileName, 0, 0, 4,
                    "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                    "\"count\":4,\"values\":[\"cat\",null,"
                    "\"dog\",\"cat\"]}");
        }
    }

    for (const bool littleEndian : {false, true})
    {
        const std::string endian =
                littleEndian ? "LE" : "BE";
        struct ShortDecimalCase
        {
            std::uint32_t precision;
            std::uint32_t scale;
            std::vector<std::int64_t> values;
            const char *expected;
        };
        const ShortDecimalCase shortDecimals[] = {
                {1, 0, {9, -9, 0, 1},
                 "[\"9\",\"-9\",\"0\",\"1\"]"},
                {18, 18, {1, -1, 0, 999999999999999999LL},
                 "[\"0.000000000000000001\","
                 "\"-0.000000000000000001\","
                 "\"0.000000000000000000\","
                 "\"0.999999999999999999\"]"}};
        for (const ShortDecimalCase &test : shortDecimals)
        {
            const std::string name =
                    "DECIMAL-" + std::to_string(test.precision)
                    + "-" + endian;
            const std::string fileName = name + ".pxl";
            writeBytes(
                    directory / fileName,
                    makeShortDecimalFixture(
                            test.precision, test.scale,
                            littleEndian, test.values));
            addCase(
                    name, fileName, 0, 0, 4,
                    "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                    "\"count\":4,\"values\":"
                    + std::string(test.expected) + "}");
        }
        const std::pair<std::uint32_t, std::uint32_t>
                longDecimals[] = {{19, 0}, {38, 4}};
        for (const auto &test : longDecimals)
        {
            const std::string name =
                    "DECIMAL-" + std::to_string(test.first)
                    + "-" + endian;
            const std::string fileName = name + ".pxl";
            writeBytes(
                    directory / fileName,
                    makeLongDecimalFixture(
                            test.first, test.second,
                            littleEndian));
            const std::string values =
                    test.first == 19
                    ? "[\"123456\",\"-1\"]"
                    : "[\"12.3456\",\"-0.0001\"]";
            addCase(
                    name, fileName, 0, 0, 2,
                    "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                    "\"count\":2,\"values\":" + values + "}");
        }

        for (std::uint32_t precision = 0;
             precision <= 6; ++precision)
        {
            std::vector<std::uint8_t> file =
                    makeFixedScalarFixture(
                            pixels::proto::Type_Kind_TIMESTAMP,
                            littleEndian);
            rewriteFileTail(
                    file, [precision](pixels::proto::FileTail &tail) {
                        tail.mutable_footer()
                                ->mutable_types(0)
                                ->set_precision(precision);
                    });
            const std::string name =
                    "TIMESTAMP-" + std::to_string(precision)
                    + "-" + endian;
            const std::string fileName = name + ".pxl";
            writeBytes(directory / fileName, file);
            addCase(
                    name, fileName, 0, 0, 4,
                    "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                    "\"count\":4,\"values\":["
                    "\"-9223372036854775808\",\"-1\",\"0\","
                    "\"9223372036854775807\"]}");
        }
    }

    const std::pair<const char *, const char *> timezoneCases[] = {
            {"empty", ""}, {"UTC", "UTC"},
            {"IANA", "America/Shanghai"},
            {"Windows", "Pacific Standard Time"},
            {"unknown", "not/a-zone"}};
    for (const auto &test : timezoneCases)
    {
        std::vector<std::uint8_t> file =
                makeFixedScalarFixture(
                        pixels::proto::Type_Kind_TIMESTAMP, true);
        rewriteFileTail(
                file, [&test](pixels::proto::FileTail &tail) {
                    tail.mutable_postscript()->set_writertimezone(
                            test.second);
                });
        const std::string name =
                "TIMEZONE-" + std::string(test.first);
        const std::string fileName = name + ".pxl";
        writeBytes(directory / fileName, file);
        addCase(
                name, fileName, 0, 1, 2,
                "{\"rowGroup\":0,\"column\":0,\"offset\":1,"
                "\"count\":2,\"values\":[\"-1\",\"0\"]}");
    }

    const std::pair<pixels::proto::CompressionKind, const char *>
            compressionCases[] = {
                    {pixels::proto::NONE, "NONE"},
                    {pixels::proto::ZLIB, "ZLIB"},
                    {pixels::proto::SNAPPY, "SNAPPY"},
                    {pixels::proto::LZO, "LZO"},
                    {pixels::proto::LZ4, "LZ4"},
                    {pixels::proto::ZSTD, "ZSTD"}};
    for (const auto &test : compressionCases)
    {
        std::vector<std::uint8_t> file = readFixture();
        rewriteFileTail(
                file, [&test](pixels::proto::FileTail &tail) {
                    tail.mutable_postscript()->set_compression(
                            test.first);
                });
        const std::string name =
                "COMPRESSION-" + std::string(test.second);
        const std::string fileName = name + ".pxl";
        writeBytes(directory / fileName, file);
        addCase(
                name, fileName, 0, 0, 2,
                "{\"rowGroup\":0,\"column\":0,\"offset\":0,"
                "\"count\":2,\"values\":[\"0\",\"1\"]}");
    }

    manifest += "]}\n";
    std::ofstream output(directory / "manifest.json");
    require(output.good(), "unable to create corpus manifest");
    output << manifest;
    require(output.good(), "unable to write corpus manifest");
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 3
            && std::string(argv[1]) == "--write-corpus")
        {
            writeConformanceCorpus(argv[2]);
            std::cout << "pixels-inspector conformance corpus: PASS\n";
            return EXIT_SUCCESS;
        }
        if (argc == 3
            && std::string(argv[1])
               == "--write-tpch-lineitem")
        {
            writeBytes(argv[2], makeTpchLineitemFixture());
            std::cout << "pixels-inspector TPC-H lineitem fixture: PASS\n";
            return EXIT_SUCCESS;
        }
        if (argc == 3
            && std::string(argv[1])
               == "--write-multi-pixel-long")
        {
            const std::vector<std::uint8_t> fixture =
                    makeMultiPixelLongFixture(false, {0, 0, 0});
            verifyMultiPixelLongRoundTrip(fixture);
            writeBytes(argv[2], fixture);
            std::cout << "pixels-inspector multi-pixel LONG fixture: PASS\n";
            return EXIT_SUCCESS;
        }
        if (argc == 2
            && std::string(argv[1])
               == "--print-canonical-metadata")
        {
            const std::vector<std::uint8_t> file = readFixture();
            Session session(file.size());
            driveMetadata(session, file);
            std::cout << readResult(session) << "\n";
            return EXIT_SUCCESS;
        }
        if (argc == 2
            && std::string(argv[1])
               == "--print-canonical-row-group")
        {
            const std::vector<std::uint8_t> file = readFixture();
            Session session(file.size());
            driveMetadata(session, file);
            require(pixels_inspector_begin_row_group(
                            session.handle(), 0)
                    == PIXELS_INSPECTOR_RANGE_READY,
                    "row-group inspection did not start");
            require(supply(session, nextRange(session), file)
                    == PIXELS_INSPECTOR_RESULT_READY,
                    "row-group footer did not produce a result");
            std::cout << readResult(session) << "\n";
            return EXIT_SUCCESS;
        }
        require(pixels_inspector_abi_version()
                == PIXELS_INSPECTOR_ABI_VERSION,
                "unexpected inspector ABI version");
        testPlainScalarDecoder();
        testPlainPixelPlanner();
        testPlainLongDecoder();
        testCapabilities();
        testCanonicalFixture();
        testFixedScalarTypePages();
        testRemainingVariableTypePages();
        testBoundedPageRange();
        testRowGroupLayoutAndRepeatedPages();
        testGenericPlainScalarPages();
        testMultiPixelPlainPages();
        testMultiPixelRunLengthPages();
        testMultiPixelVariablePages();
        testBinaryPage();
        testVectorPage();
        testDictionaryPages();
        testLongDecimalPage();
        testByteRlePage();
        testTpchLineitemFixture();
        testRowProjection();
        testWholeFileFilter();
        testScanV2();
        testCompatibilityMetadata();
        testDecimalAndTimestampBoundaries();
        testSupportedEncodingMatrix();
        testDecimalAndTimestampOperations();
        testNestedPage();
        testNestedCancellation();
        testMalformedNestedRange();
        testSchemaValidation();
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
