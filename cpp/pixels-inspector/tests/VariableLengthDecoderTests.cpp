/*
 * Copyright 2026 PixelsDB.
 *
 * Native conformance tests for portable variable-width layouts.
 */

#include "format/VariableLengthDecoder.h"

#include <cstdlib>
#include <iostream>
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

void testPlainLayout()
{
    pixels::format::FormatError error;
    pixels::format::PlainVariableLayout layout;
    const std::uint8_t littleTrailer[] = {5, 0, 0, 0};
    require(pixels::format::VariableLengthDecoder::parsePlainLayout(
                    pixels::format::ByteSpan(
                            littleTrailer, sizeof(littleTrailer)),
                    true, 25, layout, error)
            && layout.contentLength == 5
            && layout.startsOffset == 5
            && layout.startsLength == 16
            && layout.physicalValueCount == 3,
            "little-endian plain layout differs");

    const std::uint8_t bigTrailer[] = {0, 0, 0, 5};
    require(pixels::format::VariableLengthDecoder::parsePlainLayout(
                    pixels::format::ByteSpan(
                            bigTrailer, sizeof(bigTrailer)),
                    false, 25, layout, error)
            && layout.contentLength == 5,
            "big-endian plain layout differs");

    const std::uint8_t badTrailer[] = {22, 0, 0, 0};
    require(!pixels::format::VariableLengthDecoder::parsePlainLayout(
                    pixels::format::ByteSpan(
                            badTrailer, sizeof(badTrailer)),
                    true, 25, layout, error)
            && error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
            "out-of-range starts offset was not rejected");
}

void testStartsWindow()
{
    const std::uint8_t starts[] = {
            0, 0, 0, 0,
            2, 0, 0, 0,
            2, 0, 0, 0,
            5, 0, 0, 0};
    pixels::format::FormatError error;
    std::vector<pixels::format::VariableValueRange> ranges;
    require(pixels::format::VariableLengthDecoder::decodeStartsWindow(
                    pixels::format::ByteSpan(starts, sizeof(starts)),
                    true, 3, 5, ranges, error)
            && ranges.size() == 3
            && ranges[0].offset == 0 && ranges[0].length == 2
            && ranges[1].offset == 2 && ranges[1].length == 0
            && ranges[2].offset == 2 && ranges[2].length == 3,
            "plain starts window differs");

    const std::uint8_t unordered[] = {
            0, 0, 0, 0,
            3, 0, 0, 0,
            2, 0, 0, 0};
    require(!pixels::format::VariableLengthDecoder::decodeStartsWindow(
                    pixels::format::ByteSpan(
                            unordered, sizeof(unordered)),
                    true, 2, 5, ranges, error)
            && error.code
               == pixels::format::ErrorCode::MALFORMED_PROTOBUF,
            "unordered starts were not rejected");
}

void testDictionaryLayout()
{
    const std::uint8_t trailer[] = {
            8, 0, 0, 0,
            13, 0, 0, 0};
    pixels::format::DictionaryVariableLayout layout;
    pixels::format::FormatError error;
    require(pixels::format::VariableLengthDecoder::parseDictionaryLayout(
                    pixels::format::ByteSpan(
                            trailer, sizeof(trailer)),
                    true, 33, layout, error)
            && layout.idsLength == 8
            && layout.dictionaryContentOffset == 8
            && layout.dictionaryContentLength == 5
            && layout.dictionaryStartsOffset == 13
            && layout.dictionaryStartsLength == 12,
            "dictionary layout differs");

    const std::uint8_t unordered[] = {
            14, 0, 0, 0,
            13, 0, 0, 0};
    require(!pixels::format::VariableLengthDecoder::parseDictionaryLayout(
                    pixels::format::ByteSpan(
                            unordered, sizeof(unordered)),
                    true, 33, layout, error)
            && error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
            "unordered dictionary offsets were not rejected");
}

void testLengthPrefixed()
{
    const std::uint8_t bytes[] = {
            2, 0x00, 0xFF,
            0,
            3, 1, 2, 3};
    pixels::format::FormatError error;
    std::vector<pixels::format::VariableValueRange> ranges;
    require(pixels::format::VariableLengthDecoder::decodeLengthPrefixed(
                    pixels::format::ByteSpan(bytes, sizeof(bytes)),
                    3, 3, ranges, error)
            && ranges.size() == 3
            && ranges[0].offset == 1 && ranges[0].length == 2
            && ranges[1].offset == 4 && ranges[1].length == 0
            && ranges[2].offset == 5 && ranges[2].length == 3,
            "length-prefixed binary ranges differ");

    require(!pixels::format::VariableLengthDecoder::decodeLengthPrefixed(
                    pixels::format::ByteSpan(bytes, sizeof(bytes) - 1),
                    3, 3, ranges, error)
            && error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
            "truncated binary value was not rejected");
}

void testUtf8()
{
    const std::uint8_t valid[] = {
            'a', 0xE4, 0xB8, 0xAD, 0xF0, 0x9F, 0x98, 0x80};
    const std::uint8_t overlong[] = {0xC0, 0x80};
    const std::uint8_t surrogate[] = {0xED, 0xA0, 0x80};
    const std::uint8_t aboveUnicode[] = {0xF4, 0x90, 0x80, 0x80};
    require(pixels::format::VariableLengthDecoder::isValidUtf8(
                    pixels::format::ByteSpan(valid, sizeof(valid))),
            "valid UTF-8 was rejected");
    require(!pixels::format::VariableLengthDecoder::isValidUtf8(
                    pixels::format::ByteSpan(
                            overlong, sizeof(overlong)))
            && !pixels::format::VariableLengthDecoder::isValidUtf8(
                    pixels::format::ByteSpan(
                            surrogate, sizeof(surrogate)))
            && !pixels::format::VariableLengthDecoder::isValidUtf8(
                    pixels::format::ByteSpan(
                            aboveUnicode, sizeof(aboveUnicode))),
            "invalid UTF-8 was accepted");
}

} // namespace

int main()
{
    try
    {
        testPlainLayout();
        testStartsWindow();
        testDictionaryLayout();
        testLengthPrefixed();
        testUtf8();
        std::cout << "pixels-inspector variable-width conformance: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << "pixels-inspector variable-width conformance: FAIL: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
