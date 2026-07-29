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

#ifndef PIXELS_FORMAT_FORMATERROR_H
#define PIXELS_FORMAT_FORMATERROR_H

#include <cstdint>
#include <string>

namespace pixels
{
namespace format
{

enum class ErrorCode : std::uint32_t
{
    NONE = 0,
    INVALID_ARGUMENT = 1,
    OUT_OF_BOUNDS = 2,
    MALFORMED_PROTOBUF = 3,
    UNSUPPORTED_VERSION = 4,
    INVALID_MAGIC = 5,
    INVALID_STATE = 6,
    UNSUPPORTED_ENCODING = 7,
    UNSUPPORTED_TYPE = 8,
    CANCELLED = 9,
    BUFFER_TOO_SMALL = 10
};

struct FormatError
{
    ErrorCode code = ErrorCode::NONE;
    std::string message;

    void clear()
    {
        code = ErrorCode::NONE;
        message.clear();
    }

    [[nodiscard]] bool hasError() const noexcept
    {
        return code != ErrorCode::NONE;
    }
};

inline bool fail(FormatError &error, ErrorCode code, const std::string &message)
{
    error.code = code;
    error.message = message;
    return false;
}

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_FORMATERROR_H
