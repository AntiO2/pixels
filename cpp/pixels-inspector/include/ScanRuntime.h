/*
 * Copyright 2026 PixelsDB.
 *
 * Scan-v2 execution state and bounded candidate retention.
 */

#ifndef PIXELS_INSPECTOR_SCANRUNTIME_H
#define PIXELS_INSPECTOR_SCANRUNTIME_H

#include "ScanPlan.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pixels
{
namespace inspector
{

struct ScanProgress
{
    std::uint32_t phase = 0;
    std::uint32_t rowGroupsTotal = 0;
    std::uint32_t rowGroupsConsidered = 0;
    std::uint32_t scannedRowGroups = 0;
    std::uint32_t prunedRowGroups = 0;
    std::uint32_t currentRowGroup = static_cast<std::uint32_t>(-1);
    std::uint32_t retained = 0;
    std::uint64_t rowsTotal = 0;
    std::uint64_t scannedRows = 0;
    std::uint64_t prunedRows = 0;
    std::uint64_t matchedRows = 0;
};

struct ScanCandidate
{
    std::uint32_t rowGroup = 0;
    std::uint64_t localRow = 0;
    std::uint64_t absoluteRow = 0;
    std::size_t keyBytes = 0;
    std::vector<std::string> keys;
    std::vector<std::string> values;
};

struct ScanRuntime
{
    ScanPlan plan;
    ScanProgress progress;
    std::vector<std::uint32_t> inputColumns;
    std::vector<std::vector<std::string>> inputValues;
    std::size_t inputColumnIndex = 0;
    std::vector<std::uint32_t> matchingRows;
    std::uint32_t rowGroup = 0;
    std::uint64_t rowOffset = 0;
    std::uint32_t batchCount = 0;
    std::uint32_t countedRowGroup =
            static_cast<std::uint32_t>(-1);
    bool inputsReady = false;
    bool projecting = false;
    std::size_t projectionCandidate = 0;
    std::size_t projectionColumn = 0;
    std::uint64_t skippedMatches = 0;
    std::uint64_t eligibleMatches = 0;
    std::uint64_t planFingerprint = 0;
    std::uint64_t sourceSignature = 0;
    bool hasCursor = false;
    bool locatingOrderedAnchor = false;
    ScanCandidate anchor;
    std::vector<ScanCandidate> candidates;
    std::size_t retainedKeyBytes = 0;
};

using ScanCandidateComparator =
        std::function<bool(
                const ScanCandidate &, const ScanCandidate &, int &)>;

enum class ScanTopKResult
{
    OK,
    COMPARISON_FAILED,
    KEY_BUDGET_EXCEEDED
};

[[nodiscard]] ScanTopKResult insertTopK(
        std::vector<ScanCandidate> &candidates,
        ScanCandidate candidate, std::size_t retain,
        std::size_t maxKeyBytes, std::size_t &retainedKeyBytes,
        const ScanCandidateComparator &compare);

[[nodiscard]] bool sortAndRetainBest(
        std::vector<ScanCandidate> &candidates, std::size_t retain,
        const ScanCandidateComparator &compare);

} // namespace inspector
} // namespace pixels

#endif
