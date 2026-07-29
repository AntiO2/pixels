/*
 * Copyright 2026 PixelsDB.
 *
 * Stable C boundary for the Pixels portable inspector.
 */

#include "pixels_inspector.h"

#include "InspectionSession.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

namespace
{

using pixels::format::ErrorCode;
using pixels::inspector::InspectionSession;

std::unordered_map<pixels_inspector_handle,
                   std::unique_ptr<InspectionSession>> sessions;
pixels_inspector_handle nextHandle = 1;

InspectionSession *findSession(pixels_inspector_handle handle)
{
    const auto iterator = sessions.find(handle);
    return iterator == sessions.end() ? nullptr : iterator->second.get();
}

pixels_inspector_status mapError(ErrorCode code)
{
    switch (code)
    {
        case ErrorCode::NONE:
            return PIXELS_INSPECTOR_OK;
        case ErrorCode::INVALID_ARGUMENT:
            return PIXELS_INSPECTOR_INVALID_ARGUMENT;
        case ErrorCode::OUT_OF_BOUNDS:
            return PIXELS_INSPECTOR_OUT_OF_BOUNDS;
        case ErrorCode::MALFORMED_PROTOBUF:
            return PIXELS_INSPECTOR_MALFORMED_PROTOBUF;
        case ErrorCode::UNSUPPORTED_VERSION:
            return PIXELS_INSPECTOR_UNSUPPORTED_VERSION;
        case ErrorCode::INVALID_MAGIC:
            return PIXELS_INSPECTOR_INVALID_MAGIC;
        case ErrorCode::INVALID_STATE:
            return PIXELS_INSPECTOR_INVALID_STATE;
        case ErrorCode::UNSUPPORTED_ENCODING:
            return PIXELS_INSPECTOR_UNSUPPORTED_ENCODING;
        case ErrorCode::UNSUPPORTED_TYPE:
            return PIXELS_INSPECTOR_UNSUPPORTED_TYPE;
        case ErrorCode::CANCELLED:
            return PIXELS_INSPECTOR_CANCELLED;
        case ErrorCode::BUFFER_TOO_SMALL:
            return PIXELS_INSPECTOR_BUFFER_TOO_SMALL;
    }
    return PIXELS_INSPECTOR_INTERNAL_ERROR;
}

pixels_inspector_status currentStatus(const InspectionSession &session)
{
    switch (session.state())
    {
        case InspectionSession::State::AWAITING_TAIL_POINTER:
        case InspectionSession::State::AWAITING_FILE_TAIL:
        case InspectionSession::State::AWAITING_ROW_GROUP_FOOTER:
        case InspectionSession::State::AWAITING_COLUMN_CHUNK:
            return PIXELS_INSPECTOR_RANGE_READY;
        case InspectionSession::State::METADATA_READY:
        case InspectionSession::State::PAGE_READY:
            return PIXELS_INSPECTOR_RESULT_READY;
        case InspectionSession::State::CANCELLED:
            return PIXELS_INSPECTOR_CANCELLED;
        case InspectionSession::State::FAILED:
            return mapError(session.error().code);
        case InspectionSession::State::IDLE:
            return PIXELS_INSPECTOR_OK;
    }
    return PIXELS_INSPECTOR_INTERNAL_ERROR;
}

pixels_inspector_status copyString(
        const std::string &source, std::uint8_t *destination,
        std::uint64_t destinationSize)
{
    if (destinationSize < source.size())
    {
        return PIXELS_INSPECTOR_BUFFER_TOO_SMALL;
    }
    if (!source.empty() && destination == nullptr)
    {
        return PIXELS_INSPECTOR_INVALID_ARGUMENT;
    }
    if (!source.empty())
    {
        std::memcpy(destination, source.data(), source.size());
    }
    return PIXELS_INSPECTOR_OK;
}

} // namespace

extern "C"
{

std::uint32_t pixels_inspector_abi_version()
{
    return PIXELS_INSPECTOR_ABI_VERSION;
}

pixels_inspector_status pixels_inspector_create(
        std::uint64_t fileSize, pixels_inspector_handle *handle)
{
    if (handle == nullptr)
    {
        return PIXELS_INSPECTOR_INVALID_ARGUMENT;
    }
    try
    {
        if (nextHandle == 0)
        {
            return PIXELS_INSPECTOR_INTERNAL_ERROR;
        }
        const pixels_inspector_handle candidate = nextHandle++;
        sessions.emplace(
                candidate,
                std::unique_ptr<InspectionSession>(
                        new InspectionSession(fileSize)));
        *handle = candidate;
        return PIXELS_INSPECTOR_OK;
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_destroy(
        pixels_inspector_handle handle)
{
    try
    {
        return sessions.erase(handle) == 1
               ? PIXELS_INSPECTOR_OK
               : PIXELS_INSPECTOR_INVALID_HANDLE;
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_begin_metadata(
        pixels_inspector_handle handle)
{
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        if (!session->beginMetadata())
        {
            return currentStatus(*session);
        }
        return currentStatus(*session);
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_begin_plain_long_page(
        pixels_inspector_handle handle, std::uint32_t rowGroup,
        std::uint32_t column, std::uint64_t rowOffset,
        std::uint32_t rowCount)
{
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        if (!session->beginPlainLongPage(
                    rowGroup, column, rowOffset, rowCount))
        {
            return currentStatus(*session);
        }
        return currentStatus(*session);
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_next_range(
        pixels_inspector_handle handle, std::uint64_t *offset,
        std::uint64_t *length)
{
    if (offset == nullptr || length == nullptr)
    {
        return PIXELS_INSPECTOR_INVALID_ARGUMENT;
    }
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        pixels::format::FileRange range;
        if (!session->nextRange(range))
        {
            return currentStatus(*session) == PIXELS_INSPECTOR_OK
                   ? PIXELS_INSPECTOR_INVALID_STATE
                   : currentStatus(*session);
        }
        *offset = range.offset;
        *length = range.length;
        return PIXELS_INSPECTOR_RANGE_READY;
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_supply_range(
        pixels_inspector_handle handle, std::uint64_t offset,
        std::uint64_t length, const std::uint8_t *bytes)
{
    if (length > std::numeric_limits<std::size_t>::max()
        || (length > 0 && bytes == nullptr))
    {
        return PIXELS_INSPECTOR_INVALID_ARGUMENT;
    }
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        if (!session->supplyRange(
                    pixels::format::FileRange{offset, length},
                    pixels::format::ByteSpan(
                            bytes, static_cast<std::size_t>(length))))
        {
            return currentStatus(*session);
        }
        return currentStatus(*session);
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_result_size(
        pixels_inspector_handle handle, std::uint64_t *size)
{
    if (size == nullptr)
    {
        return PIXELS_INSPECTOR_INVALID_ARGUMENT;
    }
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        const pixels_inspector_status status = currentStatus(*session);
        if (status != PIXELS_INSPECTOR_RESULT_READY)
        {
            return status == PIXELS_INSPECTOR_OK
                   ? PIXELS_INSPECTOR_INVALID_STATE : status;
        }
        *size = session->result().size();
        return PIXELS_INSPECTOR_OK;
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_copy_result(
        pixels_inspector_handle handle, std::uint8_t *destination,
        std::uint64_t destinationSize)
{
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        const pixels_inspector_status status = currentStatus(*session);
        if (status != PIXELS_INSPECTOR_RESULT_READY)
        {
            return status == PIXELS_INSPECTOR_OK
                   ? PIXELS_INSPECTOR_INVALID_STATE : status;
        }
        return copyString(
                session->result(), destination, destinationSize);
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_error_size(
        pixels_inspector_handle handle, std::uint64_t *size)
{
    if (size == nullptr)
    {
        return PIXELS_INSPECTOR_INVALID_ARGUMENT;
    }
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        if (!session->error().hasError())
        {
            return PIXELS_INSPECTOR_INVALID_STATE;
        }
        *size = session->error().message.size();
        return PIXELS_INSPECTOR_OK;
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_copy_error(
        pixels_inspector_handle handle, std::uint8_t *destination,
        std::uint64_t destinationSize)
{
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        if (!session->error().hasError())
        {
            return PIXELS_INSPECTOR_INVALID_STATE;
        }
        return copyString(
                session->error().message, destination, destinationSize);
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

pixels_inspector_status pixels_inspector_cancel(
        pixels_inspector_handle handle)
{
    try
    {
        InspectionSession *session = findSession(handle);
        if (session == nullptr)
        {
            return PIXELS_INSPECTOR_INVALID_HANDLE;
        }
        if (!session->cancel())
        {
            return currentStatus(*session);
        }
        return currentStatus(*session);
    }
    catch (...)
    {
        return PIXELS_INSPECTOR_INTERNAL_ERROR;
    }
}

} // extern "C"
