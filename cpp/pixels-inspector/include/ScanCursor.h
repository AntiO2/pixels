/*
 * Copyright 2026 PixelsDB.
 *
 * Opaque, plan/source-bound scan-v2 cursor.
 */

#ifndef PIXELS_INSPECTOR_SCANCURSOR_H
#define PIXELS_INSPECTOR_SCANCURSOR_H

#include "ScanPlan.h"
#include "format/FormatError.h"

#include <cstdint>
#include <string>

namespace pixels
{
namespace inspector
{

struct ScanCursor
{
    bool ordered = false;
    std::uint64_t planFingerprint = 0;
    std::uint64_t sourceSignature = 0;
    std::uint64_t anchorAbsoluteRow = 0;
};

[[nodiscard]] std::uint64_t scanPlanFingerprint(const ScanPlan &plan);
[[nodiscard]] std::uint64_t scanSourceSignature(
        std::uint64_t fileSize, const std::string &sourceIdentity);
[[nodiscard]] std::string encodeScanCursor(const ScanCursor &cursor);
[[nodiscard]] bool decodeScanCursor(
        const std::string &encoded, ScanCursor &cursor,
        format::FormatError &error);

} // namespace inspector
} // namespace pixels

#endif
