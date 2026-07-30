/*
 * Copyright 2026 PixelsDB.
 *
 * Unit tests for the scan-v2 wire plan and three-valued expression engine.
 */

#include "ScanExpression.h"
#include "ScanCursor.h"
#include "ScanPlan.h"
#include "ScanRuntime.h"

#include <cstdint>
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

void write16(
        std::vector<std::uint8_t> &packet, std::size_t offset,
        std::uint16_t value)
{
    packet[offset] = static_cast<std::uint8_t>(value);
    packet[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void write32(
        std::vector<std::uint8_t> &packet, std::size_t offset,
        std::uint32_t value)
{
    for (std::size_t byte = 0; byte < 4; ++byte)
    {
        packet[offset + byte] = static_cast<std::uint8_t>(
                value >> (byte * 8U));
    }
}

void write64(
        std::vector<std::uint8_t> &packet, std::size_t offset,
        std::uint64_t value)
{
    for (std::size_t byte = 0; byte < 8; ++byte)
    {
        packet[offset + byte] = static_cast<std::uint8_t>(
                value >> (byte * 8U));
    }
}

std::vector<std::uint8_t> truePlan()
{
    std::vector<std::uint8_t> packet(68, 0);
    packet[0] = 'P';
    packet[1] = 'X';
    packet[2] = 'S';
    packet[3] = 'V';
    write16(packet, 4, 1);
    write16(packet, 6, 48);
    write32(packet, 8, packet.size());
    write32(packet, 12, 1);
    write16(packet, 18, 1);
    return packet;
}

pixels::inspector::ScanPlan parse(
        const std::vector<std::uint8_t> &packet,
        pixels::format::FormatError &error)
{
    pixels::inspector::ScanPlan plan;
    (void) pixels::inspector::parseScanPlan(
            pixels::format::ByteSpan(packet.data(), packet.size()),
            plan, error);
    return plan;
}

void testCanonicalDefault()
{
    pixels::format::FormatError error;
    const pixels::inspector::ScanPlan plan = parse(truePlan(), error);
    require(!error.hasError(), "canonical default plan was rejected");
    require(plan.projectionAll && plan.projection.empty(),
            "projection-all was not decoded");
    require(plan.expression.size() == 1
            && plan.expression[0].kind
               == pixels::inspector::ScanNodeKind::TRUE_VALUE,
            "TRUE expression was not decoded");
    require(plan.offset == 0 && plan.limit == 20
            && plan.order.empty() && plan.cursor.empty(),
            "default range/order/cursor normalization differs");
}

void testHeaderValidation()
{
    {
        std::vector<std::uint8_t> packet = truePlan();
        packet[0] = 'Q';
        pixels::format::FormatError error;
        (void) parse(packet, error);
        require(error.code == pixels::format::ErrorCode::INVALID_MAGIC,
                "wrong PXSV magic was accepted");
    }
    {
        std::vector<std::uint8_t> packet = truePlan();
        write16(packet, 4, 2);
        pixels::format::FormatError error;
        (void) parse(packet, error);
        require(error.code
                == pixels::format::ErrorCode::UNSUPPORTED_VERSION,
                "unknown plan version was accepted");
    }
    {
        std::vector<std::uint8_t> packet = truePlan();
        packet.push_back(0);
        pixels::format::FormatError error;
        (void) parse(packet, error);
        require(error.code
                == pixels::format::ErrorCode::INVALID_ARGUMENT,
                "trailing plan byte was accepted");
    }
    {
        std::vector<std::uint8_t> packet = truePlan();
        packet[44] = 1;
        pixels::format::FormatError error;
        (void) parse(packet, error);
        require(error.code
                == pixels::format::ErrorCode::INVALID_ARGUMENT,
                "reserved header field was accepted");
    }
}

void testBounds()
{
    {
        std::vector<std::uint8_t> packet = truePlan();
        write32(packet, 40, 501);
        pixels::format::FormatError error;
        (void) parse(packet, error);
        require(error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
                "scan limit 501 was accepted");
    }
    {
        std::vector<std::uint8_t> packet(68 + 9 * 8, 0);
        packet[0] = 'P';
        packet[1] = 'X';
        packet[2] = 'S';
        packet[3] = 'V';
        write16(packet, 4, 1);
        write16(packet, 6, 48);
        write32(packet, 8, packet.size());
        write32(packet, 12, 1);
        write16(packet, 18, 1);
        write16(packet, 20, 9);
        pixels::format::FormatError error;
        (void) parse(packet, error);
        require(error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
                "nine order keys were accepted");
    }
    {
        std::vector<std::uint8_t> packet(76, 0);
        packet[0] = 'P';
        packet[1] = 'X';
        packet[2] = 'S';
        packet[3] = 'V';
        write16(packet, 4, 1);
        write16(packet, 6, 48);
        write32(packet, 8, packet.size());
        write32(packet, 12, 1);
        write16(packet, 18, 1);
        write16(packet, 20, 1);
        write64(packet, 32, 4096);
        write32(packet, 40, 1);
        pixels::format::FormatError error;
        (void) parse(packet, error);
        require(error.code == pixels::format::ErrorCode::OUT_OF_BOUNDS,
                "ordered window 4097 was accepted");
    }
}

pixels::inspector::ScanPlan expressionPlan(
        pixels::inspector::ScanNodeKind branch)
{
    pixels::inspector::ScanPlan plan;
    plan.projectionAll = true;
    plan.expression.push_back({
            pixels::inspector::ScanNodeKind::PREDICATE,
            0, 0, 0, "", 1});
    if (branch != pixels::inspector::ScanNodeKind::NOT)
    {
        plan.expression.push_back({
                pixels::inspector::ScanNodeKind::PREDICATE,
                0, 0, 1, "", 1});
    }
    plan.expression.push_back({
            branch, 0,
            static_cast<std::uint16_t>(
                    branch == pixels::inspector::ScanNodeKind::NOT
                    ? 1 : 2),
            0, "", 2});
    return plan;
}

void testThreeValuedLogic()
{
    using pixels::inspector::ScanNodeKind;
    using pixels::inspector::ScanTruth;
    const struct
    {
        ScanNodeKind branch;
        ScanTruth left;
        ScanTruth right;
        ScanTruth expected;
    } cases[] = {
            {ScanNodeKind::AND, ScanTruth::TRUE_VALUE,
             ScanTruth::UNKNOWN, ScanTruth::UNKNOWN},
            {ScanNodeKind::AND, ScanTruth::FALSE_VALUE,
             ScanTruth::UNKNOWN, ScanTruth::FALSE_VALUE},
            {ScanNodeKind::OR, ScanTruth::FALSE_VALUE,
             ScanTruth::UNKNOWN, ScanTruth::UNKNOWN},
            {ScanNodeKind::OR, ScanTruth::TRUE_VALUE,
             ScanTruth::UNKNOWN, ScanTruth::TRUE_VALUE}};
    for (const auto &test : cases)
    {
        const pixels::inspector::ScanPlan plan =
                expressionPlan(test.branch);
        const ScanTruth values[] = {test.left, test.right};
        ScanTruth actual = ScanTruth::UNKNOWN;
        require(pixels::inspector::evaluateScanExpression(
                        plan,
                        [&values](std::size_t index, ScanTruth &truth)
                        {
                            truth = values[index];
                            return true;
                        },
                        actual)
                && actual == test.expected,
                "AND/OR three-valued truth table differs");
    }
    {
        const pixels::inspector::ScanPlan plan =
                expressionPlan(ScanNodeKind::NOT);
        ScanTruth actual = ScanTruth::FALSE_VALUE;
        require(pixels::inspector::evaluateScanExpression(
                        plan,
                        [](std::size_t, ScanTruth &truth)
                        {
                            truth = ScanTruth::UNKNOWN;
                            return true;
                        },
                        actual)
                && actual == ScanTruth::UNKNOWN,
                "NOT UNKNOWN did not remain UNKNOWN");
    }
}

void testCursorRoundTrip()
{
    const pixels::inspector::ScanCursor expected{
            true, 0x0123456789ABCDEFULL,
            0xFEDCBA9876543210ULL, 42};
    const std::string encoded =
            pixels::inspector::encodeScanCursor(expected);
    require(encoded.size() == 54
            && encoded.find('=') == std::string::npos,
            "PXC2 cursor is not canonical base64url");
    pixels::inspector::ScanCursor actual;
    pixels::format::FormatError error;
    require(pixels::inspector::decodeScanCursor(
                    encoded, actual, error)
            && actual.ordered == expected.ordered
            && actual.planFingerprint == expected.planFingerprint
            && actual.sourceSignature == expected.sourceSignature
            && actual.anchorAbsoluteRow
               == expected.anchorAbsoluteRow,
            "PXC2 cursor round trip differs");
    std::string corrupted = encoded;
    corrupted[0] = '!';
    require(!pixels::inspector::decodeScanCursor(
                    corrupted, actual, error),
            "malformed PXC2 cursor was accepted");
}

void testBoundedTopK()
{
    std::vector<pixels::inspector::ScanCandidate> candidates;
    std::size_t keyBytes = 0;
    const auto compare = [](
                                 const pixels::inspector::ScanCandidate &left,
                                 const pixels::inspector::ScanCandidate &right,
                                 int &comparison)
    {
        comparison = left.absoluteRow < right.absoluteRow ? -1
                     : left.absoluteRow > right.absoluteRow ? 1 : 0;
        return true;
    };
    for (std::uint64_t row : {3ULL, 1ULL, 2ULL})
    {
        pixels::inspector::ScanCandidate candidate;
        candidate.absoluteRow = row;
        candidate.keyBytes = 4;
        require(pixels::inspector::insertTopK(
                        candidates, std::move(candidate), 2, 8,
                        keyBytes, compare)
                == pixels::inspector::ScanTopKResult::OK,
                "bounded Top-K insertion failed");
    }
    require(candidates.size() == 2 && keyBytes == 8,
            "bounded Top-K retained too many candidates or bytes");
    require(pixels::inspector::sortAndRetainBest(
                    candidates, 2, compare)
            && candidates[0].absoluteRow == 1
            && candidates[1].absoluteRow == 2,
            "bounded Top-K retained the wrong candidates");
    pixels::inspector::ScanCandidate oversized;
    oversized.keyBytes = 9;
    require(pixels::inspector::insertTopK(
                    candidates, std::move(oversized), 3, 8,
                    keyBytes, compare)
            == pixels::inspector::ScanTopKResult::KEY_BUDGET_EXCEEDED,
            "Top-K key byte cap was not enforced");
}

} // namespace

int main()
{
    try
    {
        testCanonicalDefault();
        testHeaderValidation();
        testBounds();
        testThreeValuedLogic();
        testCursorRoundTrip();
        testBoundedTopK();
        std::cout << "pixels-inspector scan plan: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << "pixels-inspector scan plan: FAIL: "
                  << error.what() << "\n";
        return EXIT_FAILURE;
    }
}
