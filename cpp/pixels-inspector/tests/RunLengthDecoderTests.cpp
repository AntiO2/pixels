/*
 * Copyright 2026 PixelsDB.
 *
 * Native conformance tests for the portable Pixels RLEv2 decoder.
 */

#include "format/RunLengthIntDecoder.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void decodeAndRequire(
        const std::vector<std::uint8_t> &encoded, bool signedValues,
        const std::vector<std::int64_t> &expected,
        const std::string &label)
{
    std::vector<std::int64_t> actual(expected.size());
    std::size_t consumed = 0;
    pixels::format::FormatError error;
    require(pixels::format::RunLengthIntDecoder::decode(
                    pixels::format::ByteSpan(
                            encoded.data(), encoded.size()),
                    signedValues, expected.size(), actual.data(),
                    actual.size(), consumed, error),
            label + " failed: " + error.message);
    require(actual == expected, label + " values differ");
    require(consumed == encoded.size(), label + " byte count differs");
}

void testEncodingKinds()
{
    decodeAndRequire(
            {0x00, 0x09}, true, {-5, -5, -5},
            "SHORT_REPEAT");
    decodeAndRequire(
            {0x42, 0x03, 0x1B}, true, {0, -1, 1, -2},
            "DIRECT signed");
    decodeAndRequire(
            {0x42, 0x03, 0x1B}, false, {0, 1, 2, 3},
            "DIRECT unsigned");
    decodeAndRequire(
            {0x82, 0x03, 0x07, 0x21, 0x0A, 0x1A, 0xFD, 0xC0},
            true, {10, 11, 12, 1000}, "PATCHED_BASE");
    decodeAndRequire(
            {0xC0, 0x03, 0x14, 0x04}, true,
            {10, 12, 14, 16}, "DELTA fixed");
    decodeAndRequire(
            {0xC4, 0x03, 0x14, 0x04, 0x70}, true,
            {10, 12, 15, 19}, "DELTA variable");
}

void testMultipleRuns()
{
    decodeAndRequire(
            {0x00, 0x02, 0x42, 0x03, 0x1B},
            true, {1, 1, 1, 0, -1, 1, -2},
            "multiple RLE runs");
}

void testInt64Boundaries()
{
    decodeAndRequire(
            {0x7E, 0x01,
             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE},
            true,
            {std::numeric_limits<std::int64_t>::min(),
             std::numeric_limits<std::int64_t>::max()},
            "DIRECT int64 boundaries");
}

void testMalformedStreams()
{
    pixels::format::FormatError error;
    std::int64_t values[4] = {};
    std::size_t consumed = 99;

    const std::uint8_t truncated[] = {0x42, 0x03};
    require(!pixels::format::RunLengthIntDecoder::decode(
                    pixels::format::ByteSpan(
                            truncated, sizeof(truncated)),
                    true, 4, values, 4, consumed, error)
            && error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS
            && consumed == 0,
            "truncated DIRECT stream was not rejected");

    const std::uint8_t oversizedRun[] = {0x00, 0x02};
    require(!pixels::format::RunLengthIntDecoder::decode(
                    pixels::format::ByteSpan(
                            oversizedRun, sizeof(oversizedRun)),
                    true, 2, values, 4, consumed, error)
            && error.code
               == pixels::format::ErrorCode::MALFORMED_PROTOBUF,
            "run exceeding the expected count was not rejected");

    const std::uint8_t trailing[] = {0x00, 0x02, 0x00};
    require(!pixels::format::RunLengthIntDecoder::decode(
                    pixels::format::ByteSpan(
                            trailing, sizeof(trailing)),
                    true, 3, values, 4, consumed, error)
            && error.code
               == pixels::format::ErrorCode::MALFORMED_PROTOBUF,
            "trailing RLE bytes were not rejected");

    require(!pixels::format::RunLengthIntDecoder::decode(
                    pixels::format::ByteSpan(),
                    true, 1, nullptr, 0, consumed, error)
            && error.code
               == pixels::format::ErrorCode::INVALID_ARGUMENT,
            "null RLE destination was not rejected");
}

} // namespace

int main()
{
    try
    {
        testEncodingKinds();
        testMultipleRuns();
        testInt64Boundaries();
        testMalformedStreams();
        std::cout << "pixels-inspector RLE conformance: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << "pixels-inspector RLE conformance: FAIL: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
