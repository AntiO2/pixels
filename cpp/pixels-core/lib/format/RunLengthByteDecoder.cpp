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

#include "format/RunLengthByteDecoder.h"

#include <cstring>
#include <string>

namespace pixels
{
namespace format
{

bool RunLengthByteDecoder::decode(
        const ByteSpan &bytes, std::size_t expectedCount,
        std::int8_t *destination, std::size_t destinationSize,
        std::size_t &consumedBytes, FormatError &error)
{
    error.clear();
    consumedBytes = 0;
    if (!bytes.isValid())
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "RLE byte input span is invalid");
    }
    if (expectedCount == 0)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "RLE byte expected count must be positive");
    }
    if (destination == nullptr)
    {
        return fail(error, ErrorCode::INVALID_ARGUMENT,
                    "RLE byte destination is null");
    }
    if (expectedCount > destinationSize)
    {
        return fail(error, ErrorCode::BUFFER_TOO_SMALL,
                    "RLE byte destination is too small");
    }

    std::size_t input = 0;
    std::size_t output = 0;
    while (output < expectedCount)
    {
        if (input >= bytes.size())
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "RLE byte stream ends before expected values");
        }
        std::int8_t control = 0;
        std::memcpy(&control, bytes.data() + input, sizeof(control));
        ++input;
        if (control >= 0)
        {
            const std::size_t length =
                    static_cast<std::size_t>(control) + 3U;
            if (input >= bytes.size())
            {
                return fail(error, ErrorCode::OUT_OF_BOUNDS,
                            "RLE byte repeat value is truncated");
            }
            if (length > expectedCount - output)
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "RLE byte repeat exceeds the expected count");
            }
            std::int8_t value = 0;
            std::memcpy(
                    &value, bytes.data() + input, sizeof(value));
            ++input;
            for (std::size_t index = 0; index < length; ++index)
            {
                destination[output++] = value;
            }
        }
        else
        {
            const std::size_t length =
                    static_cast<std::size_t>(
                            -static_cast<std::int16_t>(control));
            if (length > bytes.size() - input)
            {
                return fail(error, ErrorCode::OUT_OF_BOUNDS,
                            "RLE byte literals are truncated");
            }
            if (length > expectedCount - output)
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "RLE byte literals exceed the expected count");
            }
            for (std::size_t index = 0; index < length; ++index)
            {
                std::memcpy(
                        destination + output,
                        bytes.data() + input, sizeof(std::int8_t));
                ++input;
                ++output;
            }
        }
    }
    consumedBytes = input;
    if (input != bytes.size())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "RLE byte stream has trailing bytes");
    }
    return true;
}

} // namespace format
} // namespace pixels
