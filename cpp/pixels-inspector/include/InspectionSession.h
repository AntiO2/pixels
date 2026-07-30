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
#include "format/PlainPixelPlanner.h"
#include "format/VariableLengthDecoder.h"
#include "pixels.pb.h"
#include "ScanPlan.h"
#include "ScanRuntime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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
        AWAITING_NULL_BITMAP = 5,
        AWAITING_COLUMN_CHUNK = 6,
        PAGE_READY = 7,
        CANCELLED = 8,
        FAILED = 9,
        AWAITING_PREFIX_NULL_BITMAP = 10,
        AWAITING_VARIABLE_TRAILER = 11,
        AWAITING_VARIABLE_STARTS = 12,
        AWAITING_VARIABLE_CONTENT = 13,
        AWAITING_DICTIONARY_TRAILER = 14,
        AWAITING_DICTIONARY_STARTS = 15,
        AWAITING_DICTIONARY_CONTENT = 16,
        AWAITING_NESTED_CHILD = 17,
        AWAITING_OPERATION_CHILD = 18
    };

    explicit InspectionSession(std::uint64_t fileSize);

    [[nodiscard]] bool beginMetadata();

    [[nodiscard]] bool beginPlainLongPage(
            std::uint32_t rowGroup, std::uint32_t column,
            std::uint64_t rowOffset, std::uint32_t rowCount);

    [[nodiscard]] bool beginPage(
            std::uint32_t rowGroup, std::uint32_t column,
            std::uint64_t rowOffset, std::uint32_t rowCount);

    [[nodiscard]] bool beginRows(
            std::uint32_t rowGroup,
            const std::vector<std::uint32_t> &columns,
            std::uint64_t rowOffset, std::uint32_t rowCount);

    [[nodiscard]] bool beginFilter(
            std::uint32_t predicateColumn, std::uint32_t filterOperator,
            const std::string &literal,
            const std::vector<std::uint32_t> &columns,
            const std::string &cursor, std::uint32_t limit);

    [[nodiscard]] bool beginScan(const format::ByteSpan &packet);
    [[nodiscard]] bool copyScanProgress(
            std::uint8_t *destination, std::uint32_t size) const;

    [[nodiscard]] bool beginRowGroup(std::uint32_t rowGroup);

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
        bool legacyLongResult = false;
        std::uint32_t bitOffset = 0;
        std::uint32_t pixel = 0;
        std::uint32_t pixelRowOffset = 0;
        std::uint32_t pixelRowCount = 0;
        std::uint32_t resultOffset = 0;
        std::uint64_t nullBitmapByteOffset = 0;
        std::uint64_t pixelPhysicalBase = 0;
        std::uint64_t variableContentBase = 0;
    };

    enum class Operation : std::uint32_t
    {
        NONE = 0,
        ROWS = 1,
        FILTER = 2,
        SCAN = 3
    };

    struct FilterRequest
    {
        std::uint32_t predicateColumn = 0;
        std::uint32_t filterOperator = 0;
        std::string literal;
        std::uint32_t rowGroup = 0;
        std::uint64_t rowOffset = 0;
        std::uint32_t limit = 100;
        std::uint64_t scannedRows = 0;
        std::uint64_t prunedRows = 0;
        std::uint32_t scannedRowGroups = 0;
        std::uint32_t prunedRowGroups = 0;
        std::uint32_t countedRowGroup =
                static_cast<std::uint32_t>(-1);
        bool predicateReady = false;
        std::uint32_t batchCount = 0;
        std::vector<std::string> predicateValues;
        std::vector<std::uint32_t> matchingRows;
    };

    [[nodiscard]] bool consumeTailPointer(const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeFileTail(const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeRowGroupFooter(const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeNullBitmap(const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeColumnChunk(const format::ByteSpan &bytes);
    [[nodiscard]] bool consumePrefixNullBitmap(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeVariableTrailer(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeVariableStarts(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeVariableContent(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeDictionaryTrailer(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeDictionaryStarts(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeDictionaryContent(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeNestedChild(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool consumeOperationChild(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool startOperationPage(
            std::uint32_t rowGroup, std::uint32_t column,
            std::uint64_t rowOffset, std::uint32_t rowCount);
    [[nodiscard]] bool continueRows();
    [[nodiscard]] bool continueFilter();
    [[nodiscard]] bool finishRows();
    [[nodiscard]] bool finishFilter(bool completed);
    [[nodiscard]] bool consumeScanChild(
            const std::vector<std::string> &values);
    [[nodiscard]] bool continueScan();
    [[nodiscard]] bool finishScan(bool complete);
    [[nodiscard]] bool validateScanPlan();
    [[nodiscard]] bool evaluateScanRow(
            std::uint32_t row, bool &matches);
    [[nodiscard]] bool scanRowGroupCanBePruned(
            std::uint32_t rowGroup, bool &pruned);
    [[nodiscard]] bool scanValueTruth(
            const ScanExpressionNode &node,
            const std::string &value, std::uint8_t &truth);
    [[nodiscard]] bool compareScanCandidates(
            const ScanCandidate &left, const ScanCandidate &right,
            int &comparison);
    [[nodiscard]] bool validateProjection(
            const std::vector<std::uint32_t> &columns);
    [[nodiscard]] bool isRootColumn(std::uint32_t column) const;
    [[nodiscard]] bool filterValueMatches(
            const proto::Type &type, const std::string &value,
            bool &matches);
    [[nodiscard]] bool rowGroupCanBePruned(
            std::uint32_t rowGroup, bool &pruned) const;
    [[nodiscard]] bool parseFilterCursor(
            const std::string &cursor,
            std::uint32_t &rowGroup, std::uint64_t &rowOffset) const;
    [[nodiscard]] std::uint64_t absoluteRow(
            std::uint32_t rowGroup, std::uint64_t localRow) const;
    void resetOperation();
    [[nodiscard]] bool validatePageRequest();
    [[nodiscard]] bool preparePageLayout();
    [[nodiscard]] bool requestCurrentPixel();
    [[nodiscard]] bool requestPixelValues();
    [[nodiscard]] bool requestVariableValues();
    [[nodiscard]] bool finishVariableValues(
            const format::ByteSpan &bytes);
    [[nodiscard]] bool finishCollectionPixel(
            const std::vector<format::VariableValueRange> &ranges);
    [[nodiscard]] bool beginNestedChildren();
    [[nodiscard]] bool startNestedChild();
    [[nodiscard]] bool finishNestedPage();
    [[nodiscard]] bool logicalRowsForColumn(
            std::uint32_t column, std::uint32_t &rows);
    void initializeNestedChild(
            const proto::FileTail &fileTail,
            std::uint32_t rowGroup, std::uint32_t logicalRows);
    [[nodiscard]] bool computeVariablePhysicalBase(
            const format::ByteSpan &prefixNullBitmaps);
    [[nodiscard]] bool usesRunLengthEncoding() const;
    [[nodiscard]] bool usesCascadeRunLengthEncoding() const;
    [[nodiscard]] bool usesNullPadding() const;
    [[nodiscard]] bool isPlainVariablePage() const;
    [[nodiscard]] bool isDictionaryPage() const;
    [[nodiscard]] bool isNestedPage() const;
    [[nodiscard]] bool finishCurrentPixel(
            const std::vector<std::string> &physicalValues);
    [[nodiscard]] bool advancePixel();
    [[nodiscard]] bool currentPixelRows(std::uint32_t &rows) const;
    [[nodiscard]] bool currentPixelDataRange(
            std::uint64_t &offset, std::uint64_t &length) const;
    [[nodiscard]] bool currentNullBitmapRange(
            format::FileRange &range) const;
    [[nodiscard]] bool beginPageRequest(
            std::uint32_t rowGroup, std::uint32_t column,
            std::uint64_t rowOffset, std::uint32_t rowCount,
            bool legacyLongResult);
    [[nodiscard]] bool transitionFailure(
            format::ErrorCode code, const std::string &message);

    void setPendingRange(const format::FileRange &range, State state);
    [[nodiscard]] bool buildMetadataResult();
    [[nodiscard]] bool buildRowGroupResult();
    [[nodiscard]] bool buildPageResult(
            const std::vector<std::string> &values);

    std::uint64_t fileSize_;
    State state_ = State::IDLE;
    format::FileRange pendingRange_;
    bool hasPendingRange_ = false;
    proto::FileTail fileTail_;
    proto::RowGroupFooter rowGroupFooter_;
    PageRequest pageRequest_;
    proto::ColumnChunkIndex pageChunk_;
    format::PlainPixelPlan pagePlan_;
    format::PlainVariableLayout variableLayout_;
    format::DictionaryVariableLayout dictionaryLayout_;
    std::vector<format::VariableValueRange> variableRanges_;
    std::vector<format::VariableValueRange> dictionaryRanges_;
    std::vector<std::uint8_t> dictionaryContent_;
    std::vector<format::VariableValueRange> collectionRanges_;
    std::unique_ptr<InspectionSession> nestedChild_;
    std::vector<std::vector<std::string>> nestedChildValues_;
    std::size_t nestedChildIndex_ = 0;
    std::uint64_t nestedChildBase_ = 0;
    std::uint32_t nestedChildCount_ = 0;
    Operation operation_ = Operation::NONE;
    std::unique_ptr<InspectionSession> operationChild_;
    std::vector<std::uint32_t> operationColumns_;
    std::vector<std::vector<std::string>> operationColumnValues_;
    std::size_t operationColumnIndex_ = 0;
    std::uint32_t operationRowGroup_ = 0;
    std::uint64_t operationRowOffset_ = 0;
    std::uint32_t operationRowCount_ = 0;
    FilterRequest filterRequest_;
    ScanRuntime scanRequest_;
    std::vector<std::uint32_t> filterResultRowGroups_;
    std::vector<std::uint64_t> filterResultLocalRows_;
    std::vector<std::vector<std::string>> filterResultRows_;
    bool rowGroupRequest_ = false;
    std::uint32_t requestedRowGroup_ = 0;
    std::unique_ptr<bool[]> pageValidity_;
    std::vector<std::string> pageValues_;
    std::string result_;
    format::FormatError error_;
};

} // namespace inspector
} // namespace pixels

#endif // PIXELS_INSPECTOR_INSPECTIONSESSION_H
