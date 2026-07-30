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

#include "format/SchemaValidator.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pixels
{
namespace format
{

namespace
{

bool validateShape(
        const proto::Type &type, FormatError &error)
{
    if (!type.has_kind())
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "schema type kind is missing");
    }
    const int children = type.subtypes_size();
    switch (type.kind())
    {
        case proto::Type_Kind_ARRAY:
            if (children != 1)
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "ARRAY must have exactly one subtype");
            }
            return true;
        case proto::Type_Kind_MAP:
            if (children != 2)
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "MAP must have exactly two subtypes");
            }
            return true;
        case proto::Type_Kind_STRUCT:
            return true;
        case proto::Type_Kind_DECIMAL:
            if (children != 0 || !type.has_precision()
                || !type.has_scale() || type.precision() == 0
                || type.precision() > 38
                || type.scale() > type.precision())
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "DECIMAL schema parameters are invalid");
            }
            return true;
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_VARBINARY:
        case proto::Type_Kind_VARCHAR:
        case proto::Type_Kind_CHAR:
            if (children != 0 || !type.has_maximumlength()
                || type.maximumlength() == 0)
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "bounded variable schema length is invalid");
            }
            return true;
        case proto::Type_Kind_VECTOR:
            if (children != 0 || !type.has_dimension()
                || type.dimension() == 0 || type.dimension() > 4096)
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "VECTOR schema dimension is invalid");
            }
            return true;
        case proto::Type_Kind_BOOLEAN:
        case proto::Type_Kind_BYTE:
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_FLOAT:
        case proto::Type_Kind_DOUBLE:
        case proto::Type_Kind_STRING:
        case proto::Type_Kind_TIMESTAMP:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
            if (children != 0)
            {
                return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                            "scalar schema type has subtypes");
            }
            return true;
    }
    return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                "schema type kind is unknown");
}

} // namespace

bool SchemaValidator::validate(
        const proto::Footer &footer, FormatError &error)
{
    error.clear();
    const std::size_t count =
            static_cast<std::size_t>(footer.types_size());
    if (count == 0)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "schema has no types");
    }
    std::vector<std::uint32_t> parents(count, 0);
    for (std::size_t index = 0; index < count; ++index)
    {
        const proto::Type &type =
                footer.types(static_cast<int>(index));
        if (!validateShape(type, error))
        {
            return false;
        }
        std::unordered_set<std::uint32_t> children;
        std::unordered_set<std::string> names;
        for (int childIndex = 0;
             childIndex < type.subtypes_size(); ++childIndex)
        {
            const std::uint32_t child = type.subtypes(childIndex);
            if (child >= count || child == index
                || !children.insert(child).second
                || ++parents[child] != 1)
            {
                return fail(
                        error, ErrorCode::MALFORMED_PROTOBUF,
                        "schema subtype is duplicated, shared, or out of bounds");
            }
            if (type.kind() == proto::Type_Kind_STRUCT)
            {
                const proto::Type &childType =
                        footer.types(static_cast<int>(child));
                if (!childType.has_name() || childType.name().empty()
                    || !names.insert(childType.name()).second)
                {
                    return fail(
                            error, ErrorCode::MALFORMED_PROTOBUF,
                            "STRUCT field names must be present and unique");
                }
            }
        }
    }

    std::vector<std::uint8_t> colors(count, 0);
    std::function<bool(std::uint32_t, std::uint32_t)> visit =
            [&](std::uint32_t typeId, std::uint32_t depth) -> bool
    {
        if (depth > MAX_NESTING_DEPTH)
        {
            return fail(error, ErrorCode::OUT_OF_BOUNDS,
                        "schema nesting exceeds the bounded depth");
        }
        if (colors[typeId] == 1)
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "schema subtype graph contains a cycle");
        }
        if (colors[typeId] == 2)
        {
            return true;
        }
        colors[typeId] = 1;
        const proto::Type &type =
                footer.types(static_cast<int>(typeId));
        for (int index = 0; index < type.subtypes_size(); ++index)
        {
            if (!visit(type.subtypes(index), depth + 1U))
            {
                return false;
            }
        }
        colors[typeId] = 2;
        return true;
    };
    bool hasRoot = false;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (parents[index] == 0)
        {
            hasRoot = true;
            if (!visit(static_cast<std::uint32_t>(index), 1))
            {
                return false;
            }
        }
    }
    if (!hasRoot)
    {
        return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                    "schema subtype graph has no root");
    }
    for (const std::uint8_t color : colors)
    {
        if (color != 2)
        {
            return fail(error, ErrorCode::MALFORMED_PROTOBUF,
                        "schema subtype graph contains an unreachable cycle");
        }
    }
    return true;
}

} // namespace format
} // namespace pixels
