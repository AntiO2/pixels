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

#ifndef PIXELS_INSPECTOR_INSPECTIONSESSION_H
#define PIXELS_INSPECTOR_INSPECTIONSESSION_H

#include "format/ByteSpan.h"
#include "format/ByteReader.h"
#include "format/FormatError.h"
#include "pixels.pb.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pixels
{
namespace inspector
{

class InspectionSession
{
public:
    enum class State : std::uint32_t
    {
        IDLE = 0,
        AWAITING_TAIL_POINTER = 1,
        AWAITING_FILE_TAIL = 2,
        METADATA_READY = 3,
        AWAITING_ROW_GROUP_FOOTER = 4,
        AWAITING_COLUMN_CHUNK = 5,
        PAGE_READY = 6,
        CANCELLED = 7,
        FAILED = 8
    };

    explicit InspectionSession(std::uint64_t fileSize);

    [[nodiscard]] bool beginMetadata();

    [[nodiscard]] bool beginPlainLongPage(
            std::uint32_t rowGroup, std::uint32_t column,
            std::uint64_t rowOffset, std::uint32_t rowCount);

    [[nodiscard]] bool nextRange(format::FileRange &range) const;

    [[nodiscard]] bool supplyRange(
            const format::FileRange &range, const format::ByteSpan &bytes);

    [[nodiscard]] bool cancel();

    [[nodiscard]] State state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] const std::string &result() const noexcept
    {
        return result_;
    }

    [[nodiscard]] const format::FormatError &error() const noexcept
    {
        return error_;
    }

private:
    struct PageRequest
    {
        std::uint32_t rowGroup = 0;
        std::uint32_t column = 0;
        std::uint64_t rowOffset = 0;
        std::uint32_t rowCount = 0;
    };

    [[nodiscard]] bool consumeTailPointer(const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeFileTail(const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeRowGroupFooter(const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeColumnChunk(const format::ByteSpan &bytes);
    [[nodiscard]] bool validatePageRequest();
    [[nodiscard]] bool transitionFailure(
            format::ErrorCode code, const std::string &message);

    void setPendingRange(const format::FileRange &range, State state);
    void buildMetadataResult();
    void buildPageResult(const std::vector<std::int64_t> &values);

    std::uint64_t fileSize_;
    State state_ = State::IDLE;
    format::FileRange pendingRange_;
    bool hasPendingRange_ = false;
    proto::FileTail fileTail_;
    proto::RowGroupFooter rowGroupFooter_;
    PageRequest pageRequest_;
    proto::ColumnChunkIndex pageChunk_;
    std::string result_;
    format::FormatError error_;
};

} // namespace inspector
} // namespace pixels

#endif // PIXELS_INSPECTOR_INSPECTIONSESSION_H
