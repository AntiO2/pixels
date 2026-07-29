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

#ifndef PIXELS_FORMAT_BYTESPAN_H
#define PIXELS_FORMAT_BYTESPAN_H

#include <cstddef>
#include <cstdint>

namespace pixels
{
namespace format
{

/**
 * An immutable, non-owning view over bytes.
 *
 * ByteSpan deliberately has no file, allocator, or platform dependencies. The
 * owner must keep the referenced storage alive for the duration of a call.
 */
class ByteSpan
{
public:
    ByteSpan() noexcept = default;

    ByteSpan(const std::uint8_t *data, std::size_t size) noexcept
            : data_(data), size_(size)
    {
    }

    [[nodiscard]] const std::uint8_t *data() const noexcept
    {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }

    [[nodiscard]] bool isValid() const noexcept
    {
        return data_ != nullptr || size_ == 0;
    }

    [[nodiscard]] bool subspan(std::size_t offset, std::size_t length,
                               ByteSpan &result) const noexcept
    {
        if (!isValid() || offset > size_ || length > size_ - offset)
        {
            return false;
        }
        result = ByteSpan(data_ + offset, length);
        return true;
    }

private:
    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_BYTESPAN_H
