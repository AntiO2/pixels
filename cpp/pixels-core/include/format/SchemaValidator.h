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

#ifndef PIXELS_FORMAT_SCHEMAVALIDATOR_H
#define PIXELS_FORMAT_SCHEMAVALIDATOR_H

#include "format/FormatError.h"
#include "pixels.pb.h"

#include <cstdint>

namespace pixels
{
namespace format
{

class SchemaValidator
{
public:
    static const std::uint32_t MAX_NESTING_DEPTH = 32;

    [[nodiscard]] static bool validate(
            const proto::Footer &footer, FormatError &error);
};

} // namespace format
} // namespace pixels

#endif // PIXELS_FORMAT_SCHEMAVALIDATOR_H
