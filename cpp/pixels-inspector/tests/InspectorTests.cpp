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
                        session.handle(), 0, 1, 0, 1)
                == PIXELS_INSPECTOR_RANGE_READY,
                "VARCHAR page did not request its row-group footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_UNSUPPORTED_TYPE,
                "VARCHAR was accepted as a plain scalar");
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
            && std::equal(validity, validity + 6, expected),
            "unpadded little-endian null plan differs");

    require(pixels::format::PlainPixelPlanner::plan(
                    10, 2, 6, true, true, true,
                    pixels::format::ByteSpan(
                            littleNulls, sizeof(littleNulls)),
                    validity, 6, plan, error)
            && plan.physicalOffset == 2
            && plan.physicalCount == 6
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
                                    ColumnEncoding_Kind_RUNLENGTH);
                });
        Session session(file.size());
        driveMetadata(session, file);
        require(pixels_inspector_begin_plain_long_page(
                        session.handle(), 0, 0, 0, 10)
                == PIXELS_INSPECTOR_RANGE_READY,
                "page did not request its footer");
        require(supply(session, nextRange(session), file)
                == PIXELS_INSPECTOR_UNSUPPORTED_ENCODING,
                "RUNLENGTH validation page was not rejected");
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
                == PIXELS_INSPECTOR_UNSUPPORTED_ENCODING,
                "page crossing a pixel boundary was not rejected");
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
