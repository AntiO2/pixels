/*
 * Copyright 2026 PixelsDB.
 *
 * Stable C boundary for the Pixels portable inspector.
 */

#ifndef PIXELS_INSPECTOR_C_API_H
#define PIXELS_INSPECTOR_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PIXELS_INSPECTOR_ABI_VERSION 4U
#define PIXELS_INSPECTOR_MAX_OPERATION_ROWS 500U
#define PIXELS_INSPECTOR_DEFAULT_FILTER_ROWS 100U
#define PIXELS_INSPECTOR_MAX_PROJECTION_COLUMNS 128U
#define PIXELS_INSPECTOR_MAX_FILTER_LITERAL_BYTES 4096U
#define PIXELS_INSPECTOR_MAX_FILTER_CURSOR_BYTES 64U
#define PIXELS_INSPECTOR_MAX_SCAN_PLAN_BYTES 131072U
#define PIXELS_INSPECTOR_SCAN_PROGRESS_V1_BYTES 64U

typedef uint32_t pixels_inspector_handle;

typedef enum pixels_inspector_status
{
    PIXELS_INSPECTOR_OK = 0,
    PIXELS_INSPECTOR_RANGE_READY = 1,
    PIXELS_INSPECTOR_RESULT_READY = 2,
    PIXELS_INSPECTOR_INVALID_ARGUMENT = 100,
    PIXELS_INSPECTOR_OUT_OF_BOUNDS = 101,
    PIXELS_INSPECTOR_MALFORMED_PROTOBUF = 102,
    PIXELS_INSPECTOR_UNSUPPORTED_VERSION = 103,
    PIXELS_INSPECTOR_INVALID_MAGIC = 104,
    PIXELS_INSPECTOR_INVALID_STATE = 105,
    PIXELS_INSPECTOR_UNSUPPORTED_ENCODING = 106,
    PIXELS_INSPECTOR_UNSUPPORTED_TYPE = 107,
    PIXELS_INSPECTOR_CANCELLED = 108,
    PIXELS_INSPECTOR_BUFFER_TOO_SMALL = 109,
    PIXELS_INSPECTOR_INVALID_HANDLE = 110,
    PIXELS_INSPECTOR_INTERNAL_ERROR = 111
} pixels_inspector_status;

typedef enum pixels_inspector_filter_operator
{
    PIXELS_INSPECTOR_FILTER_EQ = 0,
    PIXELS_INSPECTOR_FILTER_NE = 1,
    PIXELS_INSPECTOR_FILTER_LT = 2,
    PIXELS_INSPECTOR_FILTER_LE = 3,
    PIXELS_INSPECTOR_FILTER_GT = 4,
    PIXELS_INSPECTOR_FILTER_GE = 5,
    PIXELS_INSPECTOR_FILTER_IS_NULL = 6,
    PIXELS_INSPECTOR_FILTER_IS_NOT_NULL = 7,
    PIXELS_INSPECTOR_FILTER_CONTAINS = 8
} pixels_inspector_filter_operator;

uint32_t pixels_inspector_abi_version(void);

pixels_inspector_status pixels_inspector_capabilities_size(
        uint64_t *size);

pixels_inspector_status pixels_inspector_copy_capabilities(
        uint8_t *destination, uint64_t destination_size);

pixels_inspector_status pixels_inspector_create(
        uint64_t file_size, pixels_inspector_handle *handle);

pixels_inspector_status pixels_inspector_destroy(
        pixels_inspector_handle handle);

pixels_inspector_status pixels_inspector_begin_metadata(
        pixels_inspector_handle handle);

pixels_inspector_status pixels_inspector_begin_row_group(
        pixels_inspector_handle handle, uint32_t row_group);

pixels_inspector_status pixels_inspector_begin_plain_long_page(
        pixels_inspector_handle handle, uint32_t row_group, uint32_t column,
        uint64_t row_offset, uint32_t row_count);

pixels_inspector_status pixels_inspector_begin_page(
        pixels_inspector_handle handle, uint32_t row_group, uint32_t column,
        uint64_t row_offset, uint32_t row_count);

pixels_inspector_status pixels_inspector_begin_rows(
        pixels_inspector_handle handle, uint32_t row_group,
        const uint32_t *columns, uint32_t column_count,
        uint64_t row_offset, uint32_t row_count);

pixels_inspector_status pixels_inspector_begin_filter(
        pixels_inspector_handle handle, uint32_t predicate_column,
        pixels_inspector_filter_operator filter_operator,
        const uint8_t *literal, uint32_t literal_size,
        const uint32_t *columns, uint32_t column_count,
        const uint8_t *cursor, uint32_t cursor_size, uint32_t limit);

pixels_inspector_status pixels_inspector_begin_scan(
        pixels_inspector_handle handle,
        const uint8_t *plan, uint32_t plan_size);

pixels_inspector_status pixels_inspector_copy_scan_progress(
        pixels_inspector_handle handle,
        uint8_t *destination, uint32_t destination_size);

pixels_inspector_status pixels_inspector_next_range(
        pixels_inspector_handle handle, uint64_t *offset, uint64_t *length);

pixels_inspector_status pixels_inspector_supply_range(
        pixels_inspector_handle handle, uint64_t offset, uint64_t length,
        const uint8_t *bytes);

pixels_inspector_status pixels_inspector_result_size(
        pixels_inspector_handle handle, uint64_t *size);

pixels_inspector_status pixels_inspector_copy_result(
        pixels_inspector_handle handle, uint8_t *destination,
        uint64_t destination_size);

pixels_inspector_status pixels_inspector_error_size(
        pixels_inspector_handle handle, uint64_t *size);

pixels_inspector_status pixels_inspector_copy_error(
        pixels_inspector_handle handle, uint8_t *destination,
        uint64_t destination_size);

pixels_inspector_status pixels_inspector_cancel(
        pixels_inspector_handle handle);

#ifdef __cplusplus
}
#endif

#endif // PIXELS_INSPECTOR_C_API_H
