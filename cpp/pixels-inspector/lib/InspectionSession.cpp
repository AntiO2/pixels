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

#include "InspectionSession.h"

#include "pixels_inspector.h"
#include "format/PixelsFormatReader.h"
#include "format/PlainLongDecoder.h"
#include "format/PlainScalarDecoder.h"
#include "format/RunLengthByteDecoder.h"
#include "format/RunLengthIntDecoder.h"
#include "format/SchemaValidator.h"
#include "format/VariableLengthDecoder.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <set>
#include <sstream>

namespace pixels
{
namespace inspector
{

namespace
{

const std::uint32_t MAX_PAGE_ROWS = 65536;
const std::uint32_t MAX_PIXEL_ROWS = 1048576;
const std::uint32_t MAX_VECTOR_DIMENSION = 4096;
const std::uint64_t MAX_PAGE_SCALARS = 1048576;
const std::uint32_t MAX_DICTIONARY_ENTRIES = 1048576;
const std::uint64_t MAX_DICTIONARY_BYTES = 67108864;
const std::uint32_t MAX_NESTED_ELEMENTS = 65536;
const std::uint64_t MAX_VALUE_BYTES = 16777216;
const std::uint64_t MAX_RESULT_OUTPUT_BYTES = 67108864;
const std::uint32_t MAX_OPERATION_ROWS =
        PIXELS_INSPECTOR_MAX_OPERATION_ROWS;
const std::uint32_t DEFAULT_FILTER_ROWS =
        PIXELS_INSPECTOR_DEFAULT_FILTER_ROWS;
const std::uint32_t MAX_PROJECTION_COLUMNS =
        PIXELS_INSPECTOR_MAX_PROJECTION_COLUMNS;
const std::uint32_t MAX_FILTER_LITERAL_BYTES =
        PIXELS_INSPECTOR_MAX_FILTER_LITERAL_BYTES;

bool checkedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t &result) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return false;
    }
    result = left * right;
    return true;
}

std::string escapeJson(const std::string &value)
{
    std::ostringstream escaped;
    for (std::string::const_iterator iterator = value.begin();
         iterator != value.end(); ++iterator)
    {
        const unsigned char character =
                static_cast<unsigned char>(*iterator);
        switch (character)
        {
            case '"':
                escaped << "\\\"";
                break;
            case '\\':
                escaped << "\\\\";
                break;
            case '\b':
                escaped << "\\b";
                break;
            case '\f':
                escaped << "\\f";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if (character < 0x20U)
                {
                    static const char HEX[] = "0123456789abcdef";
                    escaped << "\\u00" << HEX[(character >> 4U) & 0x0FU]
                            << HEX[character & 0x0FU];
                }
                else
                {
                    escaped << static_cast<char>(character);
                }
        }
    }
    return escaped.str();
}

bool pixelHasNull(const proto::ColumnChunkIndex &chunk,
                  std::uint32_t pixel)
{
    if (pixel >= static_cast<std::uint32_t>(chunk.pixelstatistics_size()))
    {
        return false;
    }
    const proto::PixelStatistic &pixelStatistics =
            chunk.pixelstatistics(static_cast<int>(pixel));
    return pixelStatistics.has_statistic()
           && pixelStatistics.statistic().has_hasnull()
           && pixelStatistics.statistic().hasnull();
}

bool plainValueWidth(const proto::Type &type, std::uint64_t &width)
{
    switch (type.kind())
    {
        case proto::Type_Kind_BYTE:
            width = 1;
            return true;
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_FLOAT:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
            width = 4;
            return true;
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_DOUBLE:
        case proto::Type_Kind_TIMESTAMP:
            width = 8;
            return true;
        case proto::Type_Kind_DECIMAL:
            if (!type.has_precision() || !type.has_scale()
                || type.precision() == 0 || type.precision() > 38
                || type.scale() > type.precision())
            {
                return false;
            }
            width = type.precision() <= 18 ? 8 : 16;
            return true;
        case proto::Type_Kind_BOOLEAN:
            width = 0;
            return true;
        case proto::Type_Kind_STRING:
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_STRUCT:
        case proto::Type_Kind_VARBINARY:
        case proto::Type_Kind_VARCHAR:
        case proto::Type_Kind_CHAR:
            return false;
        case proto::Type_Kind_ARRAY:
        case proto::Type_Kind_MAP:
            width = 16;
            return true;
        case proto::Type_Kind_VECTOR:
            if (!type.has_dimension()
                || type.dimension() == 0
                || type.dimension() > MAX_VECTOR_DIMENSION)
            {
                return false;
            }
            width = static_cast<std::uint64_t>(type.dimension()) * 8U;
            return true;
    }
    return false;
}

bool isRunLengthInteger(const proto::Type &type)
{
    switch (type.kind())
    {
        case proto::Type_Kind_BYTE:
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
        case proto::Type_Kind_TIMESTAMP:
            return true;
        case proto::Type_Kind_BOOLEAN:
        case proto::Type_Kind_FLOAT:
        case proto::Type_Kind_DOUBLE:
        case proto::Type_Kind_STRING:
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_DECIMAL:
        case proto::Type_Kind_ARRAY:
        case proto::Type_Kind_MAP:
        case proto::Type_Kind_STRUCT:
        case proto::Type_Kind_VARBINARY:
        case proto::Type_Kind_VARCHAR:
        case proto::Type_Kind_CHAR:
        case proto::Type_Kind_VECTOR:
            return false;
    }
    return false;
}

bool isPlainVariableString(const proto::Type &type)
{
    switch (type.kind())
    {
        case proto::Type_Kind_STRING:
            return true;
        case proto::Type_Kind_VARCHAR:
        case proto::Type_Kind_CHAR:
            return type.has_maximumlength()
                   && type.maximumlength() > 0;
        case proto::Type_Kind_BOOLEAN:
        case proto::Type_Kind_BYTE:
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_FLOAT:
        case proto::Type_Kind_DOUBLE:
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_TIMESTAMP:
        case proto::Type_Kind_ARRAY:
        case proto::Type_Kind_MAP:
        case proto::Type_Kind_STRUCT:
        case proto::Type_Kind_VARBINARY:
        case proto::Type_Kind_DECIMAL:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
        case proto::Type_Kind_VECTOR:
            return false;
    }
    return false;
}

bool isLengthPrefixedBinary(const proto::Type &type)
{
    return (type.kind() == proto::Type_Kind_BINARY
            || type.kind() == proto::Type_Kind_VARBINARY)
           && type.has_maximumlength()
           && type.maximumlength() > 0;
}

bool isNestedType(const proto::Type &type)
{
    return type.kind() == proto::Type_Kind_ARRAY
           || type.kind() == proto::Type_Kind_MAP
           || type.kind() == proto::Type_Kind_STRUCT;
}

std::string encodeBase64(const format::ByteSpan &bytes)
{
    static const char ALPHABET[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2U) / 3U * 4U);
    std::size_t index = 0;
    while (index + 3U <= bytes.size())
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes.data()[index]) << 16U
                | static_cast<std::uint32_t>(
                        bytes.data()[index + 1U]) << 8U
                | bytes.data()[index + 2U];
        encoded.push_back(ALPHABET[(value >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 12U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 6U) & 0x3FU]);
        encoded.push_back(ALPHABET[value & 0x3FU]);
        index += 3U;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1U)
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes.data()[index]) << 16U;
        encoded.push_back(ALPHABET[(value >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 12U) & 0x3FU]);
        encoded.append("==");
    }
    else if (remaining == 2U)
    {
        const std::uint32_t value =
                static_cast<std::uint32_t>(bytes.data()[index]) << 16U
                | static_cast<std::uint32_t>(
                        bytes.data()[index + 1U]) << 8U;
        encoded.push_back(ALPHABET[(value >> 18U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 12U) & 0x3FU]);
        encoded.push_back(ALPHABET[(value >> 6U) & 0x3FU]);
        encoded.push_back('=');
    }
    return encoded;
}

std::string quoteInteger(std::int64_t value)
{
    return "\"" + std::to_string(value) + "\"";
}

bool parseJsonString(const std::string &token, std::string &value)
{
    if (token.size() < 2 || token.front() != '"' || token.back() != '"')
    {
        return false;
    }
    value.clear();
    for (std::size_t index = 1; index + 1 < token.size(); ++index)
    {
        const unsigned char character =
                static_cast<unsigned char>(token[index]);
        if (character != '\\')
        {
            if (character < 0x20U)
            {
                return false;
            }
            value.push_back(static_cast<char>(character));
            continue;
        }
        if (++index + 1 >= token.size())
        {
            return false;
        }
        switch (token[index])
        {
            case '"':
            case '\\':
            case '/':
                value.push_back(token[index]);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
            {
                if (index + 4 >= token.size() - 1)
                {
                    return false;
                }
                std::uint32_t codePoint = 0;
                for (std::size_t digit = 0; digit < 4; ++digit)
                {
                    const char hex = token[++index];
                    codePoint <<= 4U;
                    if (hex >= '0' && hex <= '9')
                    {
                        codePoint |= static_cast<std::uint32_t>(hex - '0');
                    }
                    else if (hex >= 'a' && hex <= 'f')
                    {
                        codePoint |= static_cast<std::uint32_t>(
                                hex - 'a' + 10);
                    }
                    else if (hex >= 'A' && hex <= 'F')
                    {
                        codePoint |= static_cast<std::uint32_t>(
                                hex - 'A' + 10);
                    }
                    else
                    {
                        return false;
                    }
                }
                if (codePoint <= 0x7FU)
                {
                    value.push_back(static_cast<char>(codePoint));
                }
                else if (codePoint <= 0x7FFU)
                {
                    value.push_back(static_cast<char>(
                            0xC0U | (codePoint >> 6U)));
                    value.push_back(static_cast<char>(
                            0x80U | (codePoint & 0x3FU)));
                }
                else if (codePoint < 0xD800U || codePoint > 0xDFFFU)
                {
                    value.push_back(static_cast<char>(
                            0xE0U | (codePoint >> 12U)));
                    value.push_back(static_cast<char>(
                            0x80U | ((codePoint >> 6U) & 0x3FU)));
                    value.push_back(static_cast<char>(
                            0x80U | (codePoint & 0x3FU)));
                }
                else
                {
                    return false;
                }
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

bool normalizeSignedInteger(
        const std::string &input, std::string &digits, bool &negative)
{
    if (input.empty())
    {
        return false;
    }
    std::size_t index = 0;
    negative = input[0] == '-';
    if (negative)
    {
        index = 1;
    }
    if (index == input.size())
    {
        return false;
    }
    if (input[index] == '0' && index + 1U < input.size())
    {
        return false;
    }
    for (std::size_t digit = index; digit < input.size(); ++digit)
    {
        if (input[digit] < '0' || input[digit] > '9')
        {
            return false;
        }
    }
    while (index + 1 < input.size() && input[index] == '0')
    {
        ++index;
    }
    digits = input.substr(index);
    if (digits == "0")
    {
        if (negative)
        {
            return false;
        }
        negative = false;
    }
    return true;
}

bool normalizeDecimal(
        const std::string &input, std::uint32_t precision,
        std::uint32_t scale, std::string &digits, bool &negative)
{
    if (input.empty())
    {
        return false;
    }
    std::size_t index = input[0] == '-' ? 1U : 0U;
    negative = index == 1U;
    if (index == input.size())
    {
        return false;
    }
    const std::size_t dot = input.find('.', index);
    if (dot != std::string::npos
        && input.find('.', dot + 1U) != std::string::npos)
    {
        return false;
    }
    const std::size_t integerEnd =
            dot == std::string::npos ? input.size() : dot;
    if (integerEnd == index)
    {
        return false;
    }
    for (std::size_t digit = index; digit < integerEnd; ++digit)
    {
        if (input[digit] < '0' || input[digit] > '9')
        {
            return false;
        }
    }
    const std::size_t fractionStart =
            dot == std::string::npos ? input.size() : dot + 1U;
    const std::size_t fractionLength = input.size() - fractionStart;
    if (fractionLength > scale
        || (dot != std::string::npos && fractionLength == 0))
    {
        return false;
    }
    for (std::size_t digit = fractionStart;
         digit < input.size(); ++digit)
    {
        if (input[digit] < '0' || input[digit] > '9')
        {
            return false;
        }
    }
    digits = input.substr(index, integerEnd - index);
    digits.append(input, fractionStart, fractionLength);
    digits.append(scale - fractionLength, '0');
    std::size_t first = 0;
    while (first + 1U < digits.size() && digits[first] == '0')
    {
        ++first;
    }
    digits.erase(0, first);
    if (digits.size() > precision)
    {
        return false;
    }
    if (digits == "0")
    {
        negative = false;
    }
    return true;
}

int compareSignedMagnitude(
        const std::string &leftDigits, bool leftNegative,
        const std::string &rightDigits, bool rightNegative)
{
    if (leftNegative != rightNegative)
    {
        return leftNegative ? -1 : 1;
    }
    int magnitude = 0;
    if (leftDigits.size() != rightDigits.size())
    {
        magnitude = leftDigits.size() < rightDigits.size() ? -1 : 1;
    }
    else if (leftDigits != rightDigits)
    {
        magnitude = leftDigits < rightDigits ? -1 : 1;
    }
    return leftNegative ? -magnitude : magnitude;
}

bool applyComparison(std::uint32_t filterOperator, int comparison)
{
    switch (filterOperator)
    {
        case PIXELS_INSPECTOR_FILTER_EQ:
            return comparison == 0;
        case PIXELS_INSPECTOR_FILTER_NE:
            return comparison != 0;
        case PIXELS_INSPECTOR_FILTER_LT:
            return comparison < 0;
        case PIXELS_INSPECTOR_FILTER_LE:
            return comparison <= 0;
        case PIXELS_INSPECTOR_FILTER_GT:
            return comparison > 0;
        case PIXELS_INSPECTOR_FILTER_GE:
            return comparison >= 0;
        default:
            return false;
    }
}

bool compareTypedLiteral(
        const proto::Type &type, const std::string &valueToken,
        const std::string &literal, int &comparison)
{
    comparison = 0;
    const bool stringLike =
            type.kind() == proto::Type_Kind_STRING
            || type.kind() == proto::Type_Kind_VARCHAR
            || type.kind() == proto::Type_Kind_CHAR;
    if (stringLike)
    {
        std::string value;
        if (!parseJsonString(valueToken, value))
        {
            return false;
        }
        comparison = value < literal ? -1 : (value > literal ? 1 : 0);
        return true;
    }
    if (type.kind() == proto::Type_Kind_BOOLEAN)
    {
        if ((literal != "true" && literal != "false")
            || (valueToken != "true" && valueToken != "false"))
        {
            return false;
        }
        comparison = valueToken == literal
                     ? 0 : (valueToken == "false" ? -1 : 1);
        return true;
    }
    if (type.kind() == proto::Type_Kind_FLOAT
        || type.kind() == proto::Type_Kind_DOUBLE)
    {
        std::istringstream leftInput(valueToken);
        leftInput.imbue(std::locale::classic());
        leftInput >> std::noskipws;
        double left = 0;
        if (!(leftInput >> left) || !leftInput.eof()
            || !std::isfinite(left))
        {
            return false;
        }
        std::istringstream rightInput(literal);
        rightInput.imbue(std::locale::classic());
        rightInput >> std::noskipws;
        double right = 0;
        if (!(rightInput >> right) || !rightInput.eof()
            || !std::isfinite(right))
        {
            return false;
        }
        comparison = left < right ? -1 : (left > right ? 1 : 0);
        return true;
    }

    std::string valueText;
    if (!parseJsonString(valueToken, valueText))
    {
        return false;
    }
    std::string leftDigits;
    std::string rightDigits;
    bool leftNegative = false;
    bool rightNegative = false;
    if (type.kind() == proto::Type_Kind_DECIMAL)
    {
        if (!type.has_precision() || !type.has_scale()
            || !normalizeDecimal(
                    valueText, type.precision(), type.scale(),
                    leftDigits, leftNegative)
            || !normalizeDecimal(
                    literal, type.precision(), type.scale(),
                    rightDigits, rightNegative))
        {
            return false;
        }
    }
    else
    {
        if (!normalizeSignedInteger(
                    valueText, leftDigits, leftNegative)
            || !normalizeSignedInteger(
                    literal, rightDigits, rightNegative))
        {
            return false;
        }
    }
    comparison = compareSignedMagnitude(
            leftDigits, leftNegative, rightDigits, rightNegative);
    return true;
}

std::string formatDecimal(std::int64_t value, std::uint32_t scale)
{
    const bool negative = value < 0;
    const std::uint64_t magnitude =
            negative
            ? static_cast<std::uint64_t>(-(value + 1)) + 1U
            : static_cast<std::uint64_t>(value);
    std::string digits = std::to_string(magnitude);
    if (scale > 0)
    {
        if (digits.size() <= scale)
        {
            digits.insert(
                    0, static_cast<std::size_t>(scale + 1U)
                       - digits.size(), '0');
        }
        digits.insert(digits.size() - scale, 1, '.');
    }
    if (negative)
    {
        digits.insert(0, 1, '-');
    }
    return "\"" + digits + "\"";
}

std::string formatDecimal128(
        format::Int128Words value, std::uint32_t scale)
{
    const bool negative = (value.high >> 63U) != 0;
    if (negative)
    {
        value.low = ~value.low + 1U;
        value.high = ~value.high
                     + (value.low == 0 ? 1U : 0U);
    }

    std::uint32_t limbs[] = {
            static_cast<std::uint32_t>(value.high >> 32U),
            static_cast<std::uint32_t>(value.high),
            static_cast<std::uint32_t>(value.low >> 32U),
            static_cast<std::uint32_t>(value.low)};
    std::string digits;
    bool any = false;
    do
    {
        std::uint64_t remainder = 0;
        any = false;
        for (std::uint32_t &limb : limbs)
        {
            const std::uint64_t dividend =
                    (remainder << 32U) | limb;
            limb = static_cast<std::uint32_t>(dividend / 10U);
            remainder = dividend % 10U;
            any = any || limb != 0;
        }
        digits.push_back(static_cast<char>('0' + remainder));
    }
    while (any);
    std::reverse(digits.begin(), digits.end());

    if (scale > 0)
    {
        if (digits.size() <= scale)
        {
            digits.insert(
                    0, static_cast<std::size_t>(scale + 1U)
                       - digits.size(), '0');
        }
        digits.insert(digits.size() - scale, 1, '.');
    }
    const bool zero =
            value.high == 0 && value.low == 0;
    if (negative && !zero)
    {
        digits.insert(0, 1, '-');
    }
    return "\"" + digits + "\"";
}

template<typename Floating>
std::string formatFloating(Floating value)
{
    if (std::isnan(value))
    {
        return "\"NaN\"";
    }
    if (std::isinf(value))
    {
        return value < 0 ? "\"-Infinity\"" : "\"Infinity\"";
    }
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<Floating>::max_digits10)
           << value;
    return output.str();
}

void appendStatisticJson(
        std::ostringstream &output,
        const proto::ColumnStatistic &statistic)
{
    output << "{\"numberOfValues\":\""
           << (statistic.has_numberofvalues()
               ? statistic.numberofvalues() : 0)
           << "\",\"containsNull\":"
           << (statistic.has_hasnull() && statistic.hasnull()
               ? "true" : "false");
    if (statistic.has_intstatistics())
    {
        const proto::IntegerStatistic &value =
                statistic.intstatistics();
        output << ",\"integer\":{";
        bool separator = false;
        if (value.has_minimum())
        {
            output << "\"minimum\":\"" << value.minimum() << "\"";
            separator = true;
        }
        if (value.has_maximum())
        {
            output << (separator ? "," : "")
                   << "\"maximum\":\"" << value.maximum() << "\"";
            separator = true;
        }
        if (value.has_sum())
        {
            output << (separator ? "," : "")
                   << "\"sum\":\"" << value.sum() << "\"";
        }
        output << "}";
    }
    if (statistic.has_doublestatistics())
    {
        const proto::DoubleStatistic &value =
                statistic.doublestatistics();
        output << ",\"double\":{";
        bool separator = false;
        if (value.has_minimum())
        {
            output << "\"minimum\":"
                   << formatFloating(value.minimum());
            separator = true;
        }
        if (value.has_maximum())
        {
            output << (separator ? "," : "")
                   << "\"maximum\":"
                   << formatFloating(value.maximum());
            separator = true;
        }
        if (value.has_sum())
        {
            output << (separator ? "," : "")
                   << "\"sum\":" << formatFloating(value.sum());
        }
        output << "}";
    }
    if (statistic.has_stringstatistics())
    {
        const proto::StringStatistic &value =
                statistic.stringstatistics();
        output << ",\"string\":{";
        bool separator = false;
        if (value.has_minimum())
        {
            output << "\"minimum\":\""
                   << escapeJson(value.minimum()) << "\"";
            separator = true;
        }
        if (value.has_maximum())
        {
            output << (separator ? "," : "")
                   << "\"maximum\":\""
                   << escapeJson(value.maximum()) << "\"";
            separator = true;
        }
        if (value.has_sum())
        {
            output << (separator ? "," : "")
                   << "\"sum\":\"" << value.sum() << "\"";
        }
        output << "}";
    }
    if (statistic.has_binarystatistics())
    {
        const proto::BinaryStatistic &value =
                statistic.binarystatistics();
        output << ",\"binary\":{";
        if (value.has_sum())
        {
            output << "\"sum\":\"" << value.sum() << "\"";
        }
        output << "}";
    }
    if (statistic.has_timestampstatistics())
    {
        const proto::TimestampStatistic &value =
                statistic.timestampstatistics();
        output << ",\"timestamp\":{";
        bool separator = false;
        if (value.has_minimum())
        {
            output << "\"minimum\":\"" << value.minimum() << "\"";
            separator = true;
        }
        if (value.has_maximum())
        {
            output << (separator ? "," : "")
                   << "\"maximum\":\"" << value.maximum() << "\"";
        }
        output << "}";
    }
    if (statistic.has_datestatistics())
    {
        const proto::DateStatistic &value =
                statistic.datestatistics();
        output << ",\"date\":{";
        bool separator = false;
        if (value.has_minimum())
        {
            output << "\"minimum\":\"" << value.minimum() << "\"";
            separator = true;
        }
        if (value.has_maximum())
        {
            output << (separator ? "," : "")
                   << "\"maximum\":\"" << value.maximum() << "\"";
        }
        output << "}";
    }
    if (statistic.has_timestatistics())
    {
        const proto::TimeStatistic &value =
                statistic.timestatistics();
        output << ",\"time\":{";
        bool separator = false;
        if (value.has_minimum())
        {
            output << "\"minimum\":\"" << value.minimum() << "\"";
            separator = true;
        }
        if (value.has_maximum())
        {
            output << (separator ? "," : "")
                   << "\"maximum\":\"" << value.maximum() << "\"";
        }
        output << "}";
    }
    if (statistic.has_bucketstatistics())
    {
        output << ",\"buckets\":[";
        const proto::BucketStatistic &value =
                statistic.bucketstatistics();
        for (int index = 0; index < value.count_size(); ++index)
        {
            if (index != 0)
            {
                output << ",";
            }
            output << "\"" << value.count(index) << "\"";
        }
        output << "]";
    }
    if (statistic.has_int128statistics())
    {
        const proto::Integer128Statistic &value =
                statistic.int128statistics();
        output << ",\"integer128\":{";
        bool separator = false;
        if (value.has_minimum_high())
        {
            output << "\"minimumHigh\":\""
                   << value.minimum_high() << "\"";
            separator = true;
        }
        if (value.has_minimum_low())
        {
            output << (separator ? "," : "")
                   << "\"minimumLow\":\""
                   << value.minimum_low() << "\"";
            separator = true;
        }
        if (value.has_maximum_high())
        {
            output << (separator ? "," : "")
                   << "\"maximumHigh\":\""
                   << value.maximum_high() << "\"";
            separator = true;
        }
        if (value.has_maximum_low())
        {
            output << (separator ? "," : "")
                   << "\"maximumLow\":\""
                   << value.maximum_low() << "\"";
        }
        output << "}";
    }
    output << "}";
}

} // namespace

InspectionSession::InspectionSession(std::uint64_t fileSize)
        : fileSize_(fileSize)
{
}

bool InspectionSession::beginMetadata()
{
    if (state_ != State::IDLE)
    {
        return transitionFailure(format::ErrorCode::INVALID_STATE,
                                 "metadata can only begin from the idle state");
    }
    if (fileSize_ < format::PixelsFormatReader::TAIL_POINTER_SIZE)
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "file is shorter than the eight-byte tail pointer");
    }

    result_.clear();
    error_.clear();
    setPendingRange(
            format::FileRange{
                    fileSize_
                    - format::PixelsFormatReader::TAIL_POINTER_SIZE,
                    format::PixelsFormatReader::TAIL_POINTER_SIZE},
            State::AWAITING_TAIL_POINTER);
    return true;
}

bool InspectionSession::beginPlainLongPage(
        std::uint32_t rowGroup, std::uint32_t column,
        std::uint64_t rowOffset, std::uint32_t rowCount)
{
    return beginPageRequest(
            rowGroup, column, rowOffset, rowCount, true);
}

bool InspectionSession::beginPage(
        std::uint32_t rowGroup, std::uint32_t column,
        std::uint64_t rowOffset, std::uint32_t rowCount)
{
    return beginPageRequest(
            rowGroup, column, rowOffset, rowCount, false);
}

bool InspectionSession::isRootColumn(std::uint32_t column) const
{
    const proto::Footer &footer = fileTail_.footer();
    if (column >= static_cast<std::uint32_t>(footer.types_size()))
    {
        return false;
    }
    for (int parent = 0; parent < footer.types_size(); ++parent)
    {
        const proto::Type &type = footer.types(parent);
        for (int child = 0; child < type.subtypes_size(); ++child)
        {
            if (type.subtypes(child) == column)
            {
                return false;
            }
        }
    }
    return true;
}

bool InspectionSession::validateProjection(
        const std::vector<std::uint32_t> &columns)
{
    if (columns.empty())
    {
        return format::fail(
                error_, format::ErrorCode::INVALID_ARGUMENT,
                "projection must contain at least one column");
    }
    if (columns.size() > MAX_PROJECTION_COLUMNS)
    {
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "projection exceeds the bounded column limit");
    }
    std::set<std::uint32_t> unique;
    for (std::uint32_t column : columns)
    {
        if (!isRootColumn(column))
        {
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "projection contains a non-root or missing column");
        }
        if (!unique.insert(column).second)
        {
            return format::fail(
                    error_, format::ErrorCode::INVALID_ARGUMENT,
                    "projection columns must be unique");
        }
    }
    return true;
}

void InspectionSession::resetOperation()
{
    operationChild_.reset();
    operationColumns_.clear();
    operationColumnValues_.clear();
    operationColumnIndex_ = 0;
    operationRowGroup_ = 0;
    operationRowOffset_ = 0;
    operationRowCount_ = 0;
    filterRequest_ = FilterRequest{};
    filterResultRowGroups_.clear();
    filterResultLocalRows_.clear();
    filterResultRows_.clear();
}

bool InspectionSession::beginRows(
        std::uint32_t rowGroup,
        const std::vector<std::uint32_t> &columns,
        std::uint64_t rowOffset, std::uint32_t rowCount)
{
    if (state_ != State::METADATA_READY
        && state_ != State::PAGE_READY)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_STATE,
                "row projection requires ready metadata");
    }
    if (rowCount == 0)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "row projection count must be positive");
    }
    if (rowCount > MAX_OPERATION_ROWS)
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "row projection count exceeds 500 rows");
    }
    if (rowGroup >= static_cast<std::uint32_t>(
                            fileTail_.footer().rowgroupinfos_size()))
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "row-group index is out of bounds");
    }
    error_.clear();
    if (!validateProjection(columns))
    {
        state_ = State::FAILED;
        return false;
    }
    const proto::RowGroupInformation &information =
            fileTail_.footer().rowgroupinfos(static_cast<int>(rowGroup));
    std::uint64_t end = 0;
    if (!information.has_numberofrows()
        || !format::checkedAdd(rowOffset, rowCount, end)
        || end > information.numberofrows())
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "row projection exceeds the row group");
    }

    resetOperation();
    operation_ = Operation::ROWS;
    operationColumns_ = columns;
    operationRowGroup_ = rowGroup;
    operationRowOffset_ = rowOffset;
    operationRowCount_ = rowCount;
    result_.clear();
    error_.clear();
    return continueRows();
}

bool InspectionSession::parseFilterCursor(
        const std::string &cursor, std::uint32_t &rowGroup,
        std::uint64_t &rowOffset) const
{
    rowGroup = 0;
    rowOffset = 0;
    if (cursor.empty())
    {
        return true;
    }
    if (cursor.size() > PIXELS_INSPECTOR_MAX_FILTER_CURSOR_BYTES)
    {
        return false;
    }
    if (cursor.compare(0, 3, "v1:") != 0)
    {
        return false;
    }
    const std::size_t separator = cursor.find(':', 3);
    if (separator == std::string::npos
        || separator == 3 || separator + 1 == cursor.size())
    {
        return false;
    }
    const std::string groupText = cursor.substr(3, separator - 3);
    const std::string rowText = cursor.substr(separator + 1);
    if ((groupText.size() > 1 && groupText[0] == '0')
        || (rowText.size() > 1 && rowText[0] == '0'))
    {
        return false;
    }
    std::string digits;
    bool negative = false;
    if (!normalizeSignedInteger(groupText, digits, negative)
        || negative || digits != groupText)
    {
        return false;
    }
    if (!normalizeSignedInteger(rowText, digits, negative)
        || negative || digits != rowText)
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long group =
            std::strtoull(groupText.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0'
        || group > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    errno = 0;
    const unsigned long long row =
            std::strtoull(rowText.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0')
    {
        return false;
    }
    rowGroup = static_cast<std::uint32_t>(group);
    rowOffset = static_cast<std::uint64_t>(row);
    return true;
}

bool InspectionSession::beginFilter(
        std::uint32_t predicateColumn, std::uint32_t filterOperator,
        const std::string &literal,
        const std::vector<std::uint32_t> &columns,
        const std::string &cursor, std::uint32_t limit)
{
    if (state_ != State::METADATA_READY
        && state_ != State::PAGE_READY)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_STATE,
                "filter requires ready metadata");
    }
    if (filterOperator > PIXELS_INSPECTOR_FILTER_CONTAINS)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "filter operator is invalid");
    }
    if (literal.size() > MAX_FILTER_LITERAL_BYTES)
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "filter literal exceeds the bounded byte limit");
    }
    const bool nullOperator =
            filterOperator == PIXELS_INSPECTOR_FILTER_IS_NULL
            || filterOperator == PIXELS_INSPECTOR_FILTER_IS_NOT_NULL;
    if (nullOperator && !literal.empty())
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "filter literal presence does not match the operator");
    }
    if (limit == 0)
    {
        limit = DEFAULT_FILTER_ROWS;
    }
    if (limit > MAX_OPERATION_ROWS)
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "filter result limit exceeds 500 rows");
    }

    error_.clear();
    if (!validateProjection(columns))
    {
        state_ = State::FAILED;
        return false;
    }
    if (!isRootColumn(predicateColumn))
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "predicate must select a root column");
    }
    const proto::Type &type =
            fileTail_.footer().types(static_cast<int>(predicateColumn));
    const bool stringLike =
            type.kind() == proto::Type_Kind_STRING
            || type.kind() == proto::Type_Kind_VARCHAR
            || type.kind() == proto::Type_Kind_CHAR;
    const bool comparable =
            stringLike
            || type.kind() == proto::Type_Kind_BOOLEAN
            || type.kind() == proto::Type_Kind_BYTE
            || type.kind() == proto::Type_Kind_SHORT
            || type.kind() == proto::Type_Kind_INT
            || type.kind() == proto::Type_Kind_LONG
            || type.kind() == proto::Type_Kind_FLOAT
            || type.kind() == proto::Type_Kind_DOUBLE
            || type.kind() == proto::Type_Kind_TIMESTAMP
            || type.kind() == proto::Type_Kind_DECIMAL
            || type.kind() == proto::Type_Kind_DATE
            || type.kind() == proto::Type_Kind_TIME;
    if (!comparable)
    {
        return transitionFailure(
                format::ErrorCode::UNSUPPORTED_TYPE,
                "predicate column type is not filterable");
    }
    if (filterOperator == PIXELS_INSPECTOR_FILTER_CONTAINS
        && !stringLike)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "contains requires a string-like predicate column");
    }
    if (type.kind() == proto::Type_Kind_BOOLEAN
        && !nullOperator
        && filterOperator != PIXELS_INSPECTOR_FILTER_EQ
        && filterOperator != PIXELS_INSPECTOR_FILTER_NE)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "BOOLEAN supports only equality and null operators");
    }
    if (!nullOperator)
    {
        if (stringLike)
        {
            if (!format::VariableLengthDecoder::isValidUtf8(
                        format::ByteSpan(
                                reinterpret_cast<const std::uint8_t *>(
                                        literal.data()),
                                literal.size())))
            {
                return transitionFailure(
                        format::ErrorCode::INVALID_ARGUMENT,
                        "string filter literal is not valid UTF-8");
            }
        }
        else
        {
            const std::string zero =
                    type.kind() == proto::Type_Kind_BOOLEAN
                    ? "false"
                    : type.kind() == proto::Type_Kind_FLOAT
                      || type.kind() == proto::Type_Kind_DOUBLE
                    ? "0" : "\"0\"";
            int comparison = 0;
            if (!compareTypedLiteral(
                        type, zero, literal, comparison))
            {
                return transitionFailure(
                        format::ErrorCode::INVALID_ARGUMENT,
                        "filter literal is not canonical for its column type");
            }
        }
    }

    std::uint32_t rowGroup = 0;
    std::uint64_t rowOffset = 0;
    if (!parseFilterCursor(cursor, rowGroup, rowOffset)
        || rowGroup > static_cast<std::uint32_t>(
                              fileTail_.footer().rowgroupinfos_size()))
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "filter cursor is malformed or outside the file");
    }
    if (rowGroup < static_cast<std::uint32_t>(
                           fileTail_.footer().rowgroupinfos_size()))
    {
        const proto::RowGroupInformation &information =
                fileTail_.footer().rowgroupinfos(
                        static_cast<int>(rowGroup));
        if (!information.has_numberofrows()
            || rowOffset > information.numberofrows())
        {
            return transitionFailure(
                    format::ErrorCode::INVALID_ARGUMENT,
                    "filter cursor row is outside its row group");
        }
    }
    else if (rowOffset != 0)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "completed filter cursor must have row zero");
    }

    resetOperation();
    operation_ = Operation::FILTER;
    operationColumns_ = columns;
    filterRequest_.predicateColumn = predicateColumn;
    filterRequest_.filterOperator = filterOperator;
    filterRequest_.literal = literal;
    filterRequest_.rowGroup = rowGroup;
    filterRequest_.rowOffset = rowOffset;
    filterRequest_.limit = limit;
    result_.clear();
    error_.clear();
    return continueFilter();
}

bool InspectionSession::beginRowGroup(std::uint32_t rowGroup)
{
    if (state_ != State::METADATA_READY
        && state_ != State::PAGE_READY)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_STATE,
                "row-group inspection requires ready metadata");
    }
    if (rowGroup >= static_cast<std::uint32_t>(
                            fileTail_.footer().rowgroupinfos_size()))
    {
        return transitionFailure(
                format::ErrorCode::OUT_OF_BOUNDS,
                "row-group index is out of bounds");
    }
    rowGroupRequest_ = true;
    requestedRowGroup_ = rowGroup;
    result_.clear();
    error_.clear();
    const proto::RowGroupInformation &information =
            fileTail_.footer().rowgroupinfos(
                    static_cast<int>(rowGroup));
    setPendingRange(
            format::FileRange{
                    information.footeroffset(),
                    information.footerlength()},
            State::AWAITING_ROW_GROUP_FOOTER);
    return true;
}

bool InspectionSession::beginPageRequest(
        std::uint32_t rowGroup, std::uint32_t column,
        std::uint64_t rowOffset, std::uint32_t rowCount,
        bool legacyLongResult)
{
    if (state_ != State::METADATA_READY
        && state_ != State::PAGE_READY)
    {
        return transitionFailure(format::ErrorCode::INVALID_STATE,
                                 "page decoding requires ready metadata");
    }
    if (rowCount == 0)
    {
        return transitionFailure(format::ErrorCode::INVALID_ARGUMENT,
                                 "page row count must be positive");
    }
    if (rowCount > MAX_PAGE_ROWS)
    {
        return transitionFailure(format::ErrorCode::OUT_OF_BOUNDS,
                                 "page row count exceeds the bounded limit");
    }
    if (rowGroup >= static_cast<std::uint32_t>(
                            fileTail_.footer().rowgroupinfos_size()))
    {
        return transitionFailure(format::ErrorCode::OUT_OF_BOUNDS,
                                 "row-group index is out of bounds");
    }
    if (column >= static_cast<std::uint32_t>(
                          fileTail_.footer().types_size()))
    {
        return transitionFailure(format::ErrorCode::OUT_OF_BOUNDS,
                                 "column index is out of bounds");
    }

    pageRequest_.rowGroup = rowGroup;
    pageRequest_.column = column;
    pageRequest_.rowOffset = rowOffset;
    pageRequest_.rowCount = rowCount;
    pageRequest_.legacyLongResult = legacyLongResult;
    pageRequest_.bitOffset = 0;
    pageRequest_.pixel = 0;
    pageRequest_.pixelRowOffset = 0;
    pageRequest_.pixelRowCount = 0;
    pageRequest_.resultOffset = 0;
    pageRequest_.nullBitmapByteOffset = 0;
    pageRequest_.pixelPhysicalBase = 0;
    pageRequest_.variableContentBase = 0;
    variableLayout_ = format::PlainVariableLayout{};
    dictionaryLayout_ = format::DictionaryVariableLayout{};
    variableRanges_.clear();
    dictionaryRanges_.clear();
    dictionaryContent_.clear();
    collectionRanges_.clear();
    nestedChild_.reset();
    nestedChildValues_.clear();
    nestedChildIndex_ = 0;
    nestedChildBase_ = 0;
    nestedChildCount_ = 0;
    rowGroupRequest_ = false;
    result_.clear();
    error_.clear();

    const proto::RowGroupInformation &rowGroupInformation =
            fileTail_.footer().rowgroupinfos(static_cast<int>(rowGroup));
    setPendingRange(
            format::FileRange{rowGroupInformation.footeroffset(),
                              rowGroupInformation.footerlength()},
            State::AWAITING_ROW_GROUP_FOOTER);
    return true;
}

bool InspectionSession::nextRange(format::FileRange &range) const
{
    if (!hasPendingRange_)
    {
        return false;
    }
    range = pendingRange_;
    return true;
}

bool InspectionSession::supplyRange(
        const format::FileRange &range, const format::ByteSpan &bytes)
{
    if (!hasPendingRange_)
    {
        return transitionFailure(format::ErrorCode::INVALID_STATE,
                                 "no byte range is currently pending");
    }
    if (!(range == pendingRange_))
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "supplied byte range does not match the pending request");
    }
    if (!bytes.isValid() || bytes.size() != range.length)
    {
        return transitionFailure(
                format::ErrorCode::INVALID_ARGUMENT,
                "supplied byte count does not match the pending request");
    }

    hasPendingRange_ = false;
    switch (state_)
    {
        case State::AWAITING_TAIL_POINTER:
            return consumeTailPointer(bytes);
        case State::AWAITING_FILE_TAIL:
            return consumeFileTail(bytes);
        case State::AWAITING_ROW_GROUP_FOOTER:
            return consumeRowGroupFooter(bytes);
        case State::AWAITING_NULL_BITMAP:
            return consumeNullBitmap(bytes);
        case State::AWAITING_COLUMN_CHUNK:
            return consumeColumnChunk(bytes);
        case State::AWAITING_PREFIX_NULL_BITMAP:
            return consumePrefixNullBitmap(bytes);
        case State::AWAITING_VARIABLE_TRAILER:
            return consumeVariableTrailer(bytes);
        case State::AWAITING_VARIABLE_STARTS:
            return consumeVariableStarts(bytes);
        case State::AWAITING_VARIABLE_CONTENT:
            return consumeVariableContent(bytes);
        case State::AWAITING_DICTIONARY_TRAILER:
            return consumeDictionaryTrailer(bytes);
        case State::AWAITING_DICTIONARY_STARTS:
            return consumeDictionaryStarts(bytes);
        case State::AWAITING_DICTIONARY_CONTENT:
            return consumeDictionaryContent(bytes);
        case State::AWAITING_NESTED_CHILD:
            return consumeNestedChild(bytes);
        case State::AWAITING_OPERATION_CHILD:
            return consumeOperationChild(bytes);
        case State::IDLE:
        case State::METADATA_READY:
        case State::PAGE_READY:
        case State::CANCELLED:
        case State::FAILED:
            return transitionFailure(format::ErrorCode::INVALID_STATE,
                                     "current state cannot consume a range");
    }
    return transitionFailure(format::ErrorCode::INVALID_STATE,
                             "unknown inspection state");
}

bool InspectionSession::cancel()
{
    switch (state_)
    {
        case State::AWAITING_TAIL_POINTER:
        case State::AWAITING_FILE_TAIL:
        case State::METADATA_READY:
        case State::AWAITING_ROW_GROUP_FOOTER:
        case State::AWAITING_NULL_BITMAP:
        case State::AWAITING_COLUMN_CHUNK:
        case State::AWAITING_PREFIX_NULL_BITMAP:
        case State::AWAITING_VARIABLE_TRAILER:
        case State::AWAITING_VARIABLE_STARTS:
        case State::AWAITING_VARIABLE_CONTENT:
        case State::AWAITING_DICTIONARY_TRAILER:
        case State::AWAITING_DICTIONARY_STARTS:
        case State::AWAITING_DICTIONARY_CONTENT:
        case State::AWAITING_NESTED_CHILD:
        case State::AWAITING_OPERATION_CHILD:
            if (state_ == State::AWAITING_NESTED_CHILD
                && nestedChild_)
            {
                (void) nestedChild_->cancel();
            }
            if (state_ == State::AWAITING_OPERATION_CHILD
                && operationChild_)
            {
                (void) operationChild_->cancel();
            }
            state_ = State::CANCELLED;
            hasPendingRange_ = false;
            result_.clear();
            error_.code = format::ErrorCode::CANCELLED;
            error_.message = "inspection was cancelled";
            return true;
        case State::IDLE:
        case State::PAGE_READY:
        case State::CANCELLED:
        case State::FAILED:
            return transitionFailure(format::ErrorCode::INVALID_STATE,
                                     "current state cannot be cancelled");
    }
    return transitionFailure(format::ErrorCode::INVALID_STATE,
                             "unknown inspection state");
}

bool InspectionSession::consumeTailPointer(const format::ByteSpan &bytes)
{
    format::FileRange fileTailRange;
    if (!format::PixelsFormatReader::parseTailPointer(
                fileSize_, bytes, fileTailRange, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    setPendingRange(fileTailRange, State::AWAITING_FILE_TAIL);
    return true;
}

bool InspectionSession::consumeFileTail(const format::ByteSpan &bytes)
{
    if (!format::PixelsFormatReader::parseFileTail(
                fileSize_, pendingRange_, bytes, fileTail_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (!format::SchemaValidator::validate(
                fileTail_.footer(), error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (!buildMetadataResult())
    {
        state_ = State::FAILED;
        return false;
    }
    state_ = State::METADATA_READY;
    return true;
}

bool InspectionSession::consumeRowGroupFooter(const format::ByteSpan &bytes)
{
    if (!format::PixelsFormatReader::parseRowGroupFooter(
                fileSize_, pendingRange_, bytes, rowGroupFooter_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (rowGroupRequest_)
    {
        if (!buildRowGroupResult())
        {
            state_ = State::FAILED;
            return false;
        }
        state_ = State::PAGE_READY;
        return true;
    }
    if (!validatePageRequest())
    {
        state_ = State::FAILED;
        return false;
    }

    pageValidity_.reset(new bool[pageRequest_.rowCount]);
    pageValues_.assign(pageRequest_.rowCount, std::string());
    collectionRanges_.assign(
            pageRequest_.rowCount,
            format::VariableValueRange{});
    const std::uint32_t pixelStride =
            fileTail_.postscript().pixelstride();
    pageRequest_.pixel = static_cast<std::uint32_t>(
            pageRequest_.rowOffset / pixelStride);
    pageRequest_.pixelRowOffset = static_cast<std::uint32_t>(
            pageRequest_.rowOffset % pixelStride);
    pageRequest_.resultOffset = 0;
    return preparePageLayout();
}

bool InspectionSession::preparePageLayout()
{
    if (isDictionaryPage())
    {
        if (pageChunk_.chunklength() < 8)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "dictionary chunk has no layout trailer");
        }
        std::uint64_t trailerOffset = 0;
        if (!format::checkedAdd(
                    pageChunk_.chunkoffset(),
                    pageChunk_.chunklength() - 8U, trailerOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "dictionary trailer offset overflows");
        }
        setPendingRange(
                format::FileRange{trailerOffset, 8},
                State::AWAITING_DICTIONARY_TRAILER);
        return true;
    }
    if (!isPlainVariablePage())
    {
        return requestCurrentPixel();
    }
    if (pageChunk_.chunklength() < 4)
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable chunk has no layout trailer");
    }
    std::uint64_t trailerOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(),
                pageChunk_.chunklength() - 4U, trailerOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable trailer offset overflows");
    }
    setPendingRange(
            format::FileRange{trailerOffset, 4},
            State::AWAITING_VARIABLE_TRAILER);
    return true;
}

bool InspectionSession::consumeDictionaryTrailer(
        const format::ByteSpan &bytes)
{
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    if (!format::VariableLengthDecoder::parseDictionaryLayout(
                bytes, littleEndian, pageChunk_.chunklength(),
                dictionaryLayout_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (!pageChunk_.has_isnulloffset()
        || pageChunk_.isnulloffset() > dictionaryLayout_.idsLength
        || dictionaryLayout_.dictionaryContentLength
           > MAX_DICTIONARY_BYTES)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "dictionary fields exceed their bounded layout");
    }
    std::uint64_t fileOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(),
                dictionaryLayout_.dictionaryStartsOffset,
                fileOffset))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "dictionary starts file offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    fileOffset,
                    dictionaryLayout_.dictionaryStartsLength},
            State::AWAITING_DICTIONARY_STARTS);
    return true;
}

bool InspectionSession::consumeDictionaryStarts(
        const format::ByteSpan &bytes)
{
    const proto::ColumnEncoding &encoding =
            rowGroupFooter_.rowgroupencoding()
                    .columnchunkencodings(
                            static_cast<int>(pageRequest_.column));
    const std::size_t dictionarySize = encoding.dictionarysize();
    std::vector<std::int64_t> starts(dictionarySize + 1U);
    bool decoded = false;
    if (usesCascadeRunLengthEncoding())
    {
        std::size_t consumed = 0;
        decoded = format::RunLengthIntDecoder::decode(
                bytes, false, starts.size(), starts.data(),
                starts.size(), consumed, error_);
    }
    else
    {
        if (starts.size()
            > std::numeric_limits<std::size_t>::max() / 4U
            || bytes.size() != starts.size() * 4U)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "dictionary starts field has an invalid length");
        }
        std::vector<std::int32_t> decodedStarts(starts.size());
        decoded = format::PlainScalarDecoder::decodeInt32(
                bytes,
                pageChunk_.has_littleendian()
                && pageChunk_.littleendian(),
                0, decodedStarts.size(), decodedStarts.data(),
                decodedStarts.size(), error_);
        if (decoded)
        {
            for (std::size_t index = 0;
                 index < starts.size(); ++index)
            {
                starts[index] = decodedStarts[index];
            }
        }
    }
    if (!decoded)
    {
        state_ = State::FAILED;
        return false;
    }
    dictionaryRanges_.clear();
    dictionaryRanges_.reserve(dictionarySize);
    for (std::size_t index = 0; index < starts.size(); ++index)
    {
        if (starts[index] < 0
            || static_cast<std::uint64_t>(starts[index])
               > dictionaryLayout_.dictionaryContentLength
            || (index != 0 && starts[index] < starts[index - 1U]))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "dictionary starts are unordered or out of bounds");
        }
    }
    if (starts.front() != 0
        || static_cast<std::uint64_t>(starts.back())
           != dictionaryLayout_.dictionaryContentLength)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "dictionary starts do not cover its content");
    }
    for (std::size_t index = 0; index < dictionarySize; ++index)
    {
        dictionaryRanges_.push_back(
                format::VariableValueRange{
                        static_cast<std::uint64_t>(starts[index]),
                        static_cast<std::uint64_t>(
                                starts[index + 1U] - starts[index])});
    }
    if (dictionaryLayout_.dictionaryContentLength == 0)
    {
        dictionaryContent_.clear();
        return requestCurrentPixel();
    }
    std::uint64_t fileOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(),
                dictionaryLayout_.dictionaryContentOffset,
                fileOffset))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "dictionary content file offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    fileOffset,
                    dictionaryLayout_.dictionaryContentLength},
            State::AWAITING_DICTIONARY_CONTENT);
    return true;
}

bool InspectionSession::consumeDictionaryContent(
        const format::ByteSpan &bytes)
{
    dictionaryContent_.assign(
            bytes.data(), bytes.data() + bytes.size());
    return requestCurrentPixel();
}

bool InspectionSession::consumeVariableTrailer(
        const format::ByteSpan &bytes)
{
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    if (!format::VariableLengthDecoder::parsePlainLayout(
                bytes, littleEndian, pageChunk_.chunklength(),
                variableLayout_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (!pageChunk_.has_isnulloffset()
        || pageChunk_.isnulloffset() > variableLayout_.startsOffset)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain variable null and starts offsets are inconsistent");
    }

    const std::uint64_t pixelStart =
            static_cast<std::uint64_t>(pageRequest_.pixel)
            * fileTail_.postscript().pixelstride();
    pageRequest_.pixelPhysicalBase = pixelStart;
    if (usesNullPadding() || pageRequest_.nullBitmapByteOffset == 0)
    {
        return requestCurrentPixel();
    }
    std::uint64_t prefixOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(), pageChunk_.isnulloffset(),
                prefixOffset))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable prefix bitmap offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    prefixOffset,
                    pageRequest_.nullBitmapByteOffset},
            State::AWAITING_PREFIX_NULL_BITMAP);
    return true;
}

bool InspectionSession::consumePrefixNullBitmap(
        const format::ByteSpan &bytes)
{
    if (!computeVariablePhysicalBase(bytes))
    {
        state_ = State::FAILED;
        return false;
    }
    return requestCurrentPixel();
}

bool InspectionSession::requestCurrentPixel()
{
    std::uint32_t pixelRows = 0;
    if (!currentPixelRows(pixelRows))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel is outside the row group");
    }
    const std::uint32_t rowsRemaining =
            pageRequest_.rowCount - pageRequest_.resultOffset;
    pageRequest_.pixelRowCount = std::min(
            rowsRemaining, pixelRows - pageRequest_.pixelRowOffset);

    if (pixelHasNull(pageChunk_, pageRequest_.pixel))
    {
        format::FileRange nullRange;
        if (!currentNullBitmapRange(nullRange))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "pixel null bitmap range is out of bounds");
        }
        setPendingRange(nullRange, State::AWAITING_NULL_BITMAP);
        return true;
    }

    if (!format::PlainPixelPlanner::plan(
                pixelRows,
                pageRequest_.pixelRowOffset,
                pageRequest_.pixelRowCount, false,
                usesNullPadding(),
                pageChunk_.has_littleendian()
                && pageChunk_.littleendian(),
                format::ByteSpan(),
                pageValidity_.get() + pageRequest_.resultOffset,
                pageRequest_.rowCount - pageRequest_.resultOffset,
                pagePlan_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    return requestPixelValues();
}

bool InspectionSession::consumeNullBitmap(const format::ByteSpan &bytes)
{
    std::uint32_t pixelRows = 0;
    if (!currentPixelRows(pixelRows))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel is outside the row group");
    }
    if (!format::PlainPixelPlanner::plan(
                pixelRows,
                pageRequest_.pixelRowOffset,
                pageRequest_.pixelRowCount, true,
                usesNullPadding(),
                pageChunk_.has_littleendian()
                && pageChunk_.littleendian(),
                bytes,
                pageValidity_.get() + pageRequest_.resultOffset,
                pageRequest_.rowCount - pageRequest_.resultOffset,
                pagePlan_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    return requestPixelValues();
}

bool InspectionSession::requestPixelValues()
{
    if (pagePlan_.physicalCount == 0)
    {
        return finishCurrentPixel(std::vector<std::string>());
    }

    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    if (type.kind() == proto::Type_Kind_STRUCT)
    {
        return finishCurrentPixel(
                std::vector<std::string>(
                        pagePlan_.physicalCount, "{}"));
    }
    if (isPlainVariablePage())
    {
        return requestVariableValues();
    }
    std::uint64_t pixelDataOffset = 0;
    std::uint64_t pixelDataLength = 0;
    if (!currentPixelDataRange(pixelDataOffset, pixelDataLength))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "pixel data range is invalid");
    }
    if (usesRunLengthEncoding())
    {
        if (pixelDataLength == 0)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "non-empty RLE pixel has no encoded content");
        }
        std::uint64_t pageFileOffset = 0;
        if (!format::checkedAdd(pageChunk_.chunkoffset(), pixelDataOffset,
                                pageFileOffset))
        {
            state_ = State::FAILED;
            return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                                "RLE pixel file offset overflows");
        }
        setPendingRange(
                format::FileRange{pageFileOffset, pixelDataLength},
                State::AWAITING_COLUMN_CHUNK);
        return true;
    }
    if (isDictionaryPage())
    {
        if (pixelDataLength == 0)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "non-empty dictionary pixel has no encoded IDs");
        }
        std::uint64_t pageFileOffset = 0;
        if (!format::checkedAdd(
                    pageChunk_.chunkoffset(), pixelDataOffset,
                    pageFileOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "dictionary ID file offset overflows");
        }
        setPendingRange(
                format::FileRange{pageFileOffset, pixelDataLength},
                State::AWAITING_COLUMN_CHUNK);
        return true;
    }
    if (isLengthPrefixedBinary(type))
    {
        if (pixelDataLength == 0)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "non-empty binary pixel has no encoded content");
        }
        std::uint64_t pageFileOffset = 0;
        if (!format::checkedAdd(
                    pageChunk_.chunkoffset(), pixelDataOffset,
                    pageFileOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "binary pixel file offset overflows");
        }
        setPendingRange(
                format::FileRange{pageFileOffset, pixelDataLength},
                State::AWAITING_COLUMN_CHUNK);
        return true;
    }

    std::uint64_t valueWidth = 0;
    if (!plainValueWidth(type, valueWidth))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::UNSUPPORTED_TYPE,
                            "column does not have a supported plain scalar layout");
    }
    std::uint64_t pageByteOffset = 0;
    std::uint64_t pageByteLength = 0;
    if (type.kind() == proto::Type_Kind_BOOLEAN)
    {
        pageRequest_.bitOffset = pagePlan_.physicalOffset % 8U;
        pageByteOffset = pagePlan_.physicalOffset / 8U;
        pageByteLength =
                (static_cast<std::uint64_t>(pageRequest_.bitOffset)
                 + pagePlan_.physicalCount + 7U) / 8U;
    }
    else
    {
        if (!checkedMultiply(pagePlan_.physicalOffset, valueWidth,
                             pageByteOffset)
            || !checkedMultiply(pagePlan_.physicalCount, valueWidth,
                                pageByteLength))
        {
            state_ = State::FAILED;
            return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                                "plain scalar page byte count overflows");
        }
    }
    std::uint64_t pageByteEnd = 0;
    if (!format::checkedAdd(
                pageByteOffset, pageByteLength, pageByteEnd)
        || pageByteEnd > pixelDataLength)
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain scalar page exceeds its pixel data");
    }
    std::uint64_t chunkByteOffset = 0;
    if (!format::checkedAdd(pixelDataOffset, pageByteOffset,
                            chunkByteOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain scalar chunk offset overflows");
    }
    std::uint64_t pageFileOffset = 0;
    if (!format::checkedAdd(pageChunk_.chunkoffset(), chunkByteOffset,
                            pageFileOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain scalar page file offset overflows");
    }
    setPendingRange(
            format::FileRange{
                    pageFileOffset,
                    pageByteLength},
            State::AWAITING_COLUMN_CHUNK);
    return true;
}

bool InspectionSession::requestVariableValues()
{
    std::uint64_t firstValue = 0;
    std::uint64_t valueEnd = 0;
    if (!format::checkedAdd(
                pageRequest_.pixelPhysicalBase,
                pagePlan_.physicalOffset, firstValue)
        || !format::checkedAdd(
                firstValue, pagePlan_.physicalCount, valueEnd)
        || valueEnd > variableLayout_.physicalValueCount)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable starts window exceeds its value count");
    }
    std::uint64_t startsByteOffset = 0;
    std::uint64_t startsByteLength = 0;
    if (!checkedMultiply(firstValue, 4U, startsByteOffset)
        || !format::checkedAdd(
                variableLayout_.startsOffset, startsByteOffset,
                startsByteOffset)
        || !checkedMultiply(
                static_cast<std::uint64_t>(pagePlan_.physicalCount) + 1U,
                4U, startsByteLength))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable starts range overflows");
    }
    std::uint64_t startsEnd = 0;
    std::uint64_t layoutEnd = 0;
    if (!format::checkedAdd(
                startsByteOffset, startsByteLength, startsEnd)
        || !format::checkedAdd(
                variableLayout_.startsOffset,
                variableLayout_.startsLength, layoutEnd)
        || startsEnd > layoutEnd)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable starts range exceeds its field");
    }
    std::uint64_t fileOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(), startsByteOffset, fileOffset))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable starts file offset overflows");
    }
    setPendingRange(
            format::FileRange{fileOffset, startsByteLength},
            State::AWAITING_VARIABLE_STARTS);
    return true;
}

bool InspectionSession::consumeVariableStarts(
        const format::ByteSpan &bytes)
{
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    if (!format::VariableLengthDecoder::decodeStartsWindow(
                bytes, littleEndian, pagePlan_.physicalCount,
                pageChunk_.isnulloffset(), variableRanges_, error_))
    {
        state_ = State::FAILED;
        return false;
    }
    if (variableRanges_.empty())
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain variable starts window is empty");
    }
    const std::uint64_t contentStart = variableRanges_.front().offset;
    const format::VariableValueRange &last = variableRanges_.back();
    std::uint64_t contentEnd = 0;
    std::uint64_t pixelOffset = 0;
    std::uint64_t pixelLength = 0;
    if (!format::checkedAdd(last.offset, last.length, contentEnd)
        || !currentPixelDataRange(pixelOffset, pixelLength))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "plain variable content range overflows");
    }
    std::uint64_t pixelEnd = 0;
    if (!format::checkedAdd(pixelOffset, pixelLength, pixelEnd)
        || contentStart < pixelOffset || contentEnd > pixelEnd)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain variable starts escape their pixel content");
    }
    pageRequest_.variableContentBase = contentStart;
    const std::uint64_t contentLength = contentEnd - contentStart;
    if (contentLength == 0)
    {
        return finishVariableValues(format::ByteSpan());
    }
    std::uint64_t fileOffset = 0;
    if (!format::checkedAdd(
                pageChunk_.chunkoffset(), contentStart, fileOffset))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable content file offset overflows");
    }
    setPendingRange(
            format::FileRange{fileOffset, contentLength},
            State::AWAITING_VARIABLE_CONTENT);
    return true;
}

bool InspectionSession::consumeVariableContent(
        const format::ByteSpan &bytes)
{
    return finishVariableValues(bytes);
}

bool InspectionSession::finishVariableValues(
        const format::ByteSpan &bytes)
{
    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    std::vector<std::string> physicalValues(variableRanges_.size());
    for (std::size_t index = 0;
         index < variableRanges_.size(); ++index)
    {
        const format::VariableValueRange &range =
                variableRanges_[index];
        if (range.length > MAX_VALUE_BYTES)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "plain variable value exceeds the bounded value limit");
        }
        if (range.offset < pageRequest_.variableContentBase)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "plain variable value precedes requested content");
        }
        const std::uint64_t relative =
                range.offset - pageRequest_.variableContentBase;
        if ((type.kind() == proto::Type_Kind_VARCHAR
             || type.kind() == proto::Type_Kind_CHAR)
            && range.length > type.maximumlength())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "plain variable value exceeds its schema length");
        }
        format::ByteSpan value;
        if (range.length == 0)
        {
            value = format::ByteSpan();
        }
        else if (relative > std::numeric_limits<std::size_t>::max()
                 || range.length
                    > std::numeric_limits<std::size_t>::max()
                 || !bytes.subspan(
                        static_cast<std::size_t>(relative),
                        static_cast<std::size_t>(range.length), value))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "plain variable value exceeds requested content");
        }
        if (!format::VariableLengthDecoder::isValidUtf8(value))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "string value is not valid UTF-8");
        }
        std::string text;
        if (!value.empty())
        {
            text.assign(
                    reinterpret_cast<const char *>(value.data()),
                    value.size());
        }
        physicalValues[index] = "\"" + escapeJson(text) + "\"";
    }
    return finishCurrentPixel(physicalValues);
}

bool InspectionSession::validatePageRequest()
{
    const proto::Footer &footer = fileTail_.footer();
    const proto::Type &type =
            footer.types(static_cast<int>(pageRequest_.column));
    if (!type.has_kind())
    {
        return format::fail(error_, format::ErrorCode::MALFORMED_PROTOBUF,
                            "column type kind is missing");
    }
    if (pageRequest_.legacyLongResult
        && type.kind() != proto::Type_Kind_LONG)
    {
        return format::fail(error_, format::ErrorCode::UNSUPPORTED_TYPE,
                            "validation page requires a LONG column");
    }
    const bool variableString = isPlainVariableString(type);
    const bool binary = isLengthPrefixedBinary(type);
    const bool nested = isNestedType(type);
    std::uint64_t valueWidth = 0;
    if (!variableString && !binary && !nested
        && !plainValueWidth(type, valueWidth))
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_TYPE,
                "plain scalar page does not support this column type");
    }
    if (type.kind() == proto::Type_Kind_VECTOR
        && (type.dimension() > MAX_PAGE_SCALARS
                               / pageRequest_.rowCount))
    {
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "VECTOR page exceeds the bounded scalar limit");
    }

    const proto::RowGroupIndex &index =
            rowGroupFooter_.rowgroupindexentry();
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                index.columnchunkindexentries_size())
        || pageRequest_.column
           >= static_cast<std::uint32_t>(
                   encodings.columnchunkencodings_size()))
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "column metadata is missing from the row group");
    }

    const proto::ColumnEncoding &encoding =
            encodings.columnchunkencodings(
                    static_cast<int>(pageRequest_.column));
    if (!encoding.has_kind())
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "column encoding kind is missing");
    }
    if (nested
        && encoding.kind() != proto::ColumnEncoding_Kind_NONE)
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                "nested parent columns require NONE encoding");
    }
    if (encoding.kind() == proto::ColumnEncoding_Kind_RUNLENGTH)
    {
        if (!isRunLengthInteger(type))
        {
            return format::fail(
                    error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                    "RLEv2 is not supported for this column type");
        }
    }
    else if (encoding.kind()
             == proto::ColumnEncoding_Kind_DICTIONARY)
    {
        if (!variableString
            || !encoding.has_dictionarysize()
            || encoding.dictionarysize() > MAX_DICTIONARY_ENTRIES
            || (encoding.has_cascadeencoding()
                && (!encoding.cascadeencoding().has_kind()
                    || encoding.cascadeencoding().kind()
                       != proto::ColumnEncoding_Kind_RUNLENGTH
                    || encoding.cascadeencoding()
                               .has_cascadeencoding())))
        {
            return format::fail(
                    error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                    "dictionary encoding metadata is unsupported");
        }
    }
    else if (encoding.kind() != proto::ColumnEncoding_Kind_NONE)
    {
        return format::fail(
                error_, format::ErrorCode::UNSUPPORTED_ENCODING,
                "column encoding is not supported");
    }

    const proto::RowGroupInformation &rowGroup =
            footer.rowgroupinfos(static_cast<int>(pageRequest_.rowGroup));
    std::uint64_t rowEnd = 0;
    if (!format::checkedAdd(pageRequest_.rowOffset,
                            pageRequest_.rowCount, rowEnd)
        || !rowGroup.has_numberofrows()
        || rowEnd > rowGroup.numberofrows())
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page rows exceed the row group");
    }

    const std::uint32_t pixelStride =
            fileTail_.postscript().pixelstride();
    if (pixelStride == 0 || pixelStride > MAX_PIXEL_ROWS)
    {
        return format::fail(error_, format::ErrorCode::MALFORMED_PROTOBUF,
                            "pixel stride is outside the bounded range");
    }

    pageChunk_ = index.columnchunkindexentries(
            static_cast<int>(pageRequest_.column));
    const std::uint64_t rowGroupRows = rowGroup.numberofrows();
    const std::uint64_t pixelCount =
            rowGroupRows == 0
            ? 0
            : (rowGroupRows - 1U) / pixelStride + 1U;
    if (pixelCount == 0 || pixelCount > INT_MAX
        || pageChunk_.pixelstatistics_size()
           != static_cast<int>(pixelCount)
        || pageChunk_.pixelpositions_size()
           != static_cast<int>(pixelCount))
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "pixel positions and statistics do not match row-group rows");
    }

    const std::uint64_t dataLength =
            pageChunk_.has_isnulloffset()
            ? pageChunk_.isnulloffset()
            : pageChunk_.chunklength();
    std::uint64_t nullBitmapBytes = 0;
    std::uint32_t previousPosition = 0;
    for (std::uint64_t pixel = 0; pixel < pixelCount; ++pixel)
    {
        const int pixelIndex = static_cast<int>(pixel);
        const proto::PixelStatistic &statistics =
                pageChunk_.pixelstatistics(pixelIndex);
        if (!statistics.has_statistic()
            || !statistics.statistic().has_hasnull())
        {
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "pixel is missing explicit null statistics");
        }
        const std::uint32_t position =
                pageChunk_.pixelpositions(pixelIndex);
        if ((pixel == 0 && position != 0)
            || (pixel != 0 && position < previousPosition)
            || position > dataLength)
        {
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "pixel positions are unordered or outside column data");
        }
        previousPosition = position;
        if (statistics.statistic().hasnull())
        {
            const std::uint64_t pixelStart = pixel * pixelStride;
            const std::uint64_t rows = std::min<std::uint64_t>(
                    pixelStride, rowGroupRows - pixelStart);
            const std::uint64_t bitmapBytes =
                    rows / 8U + (rows % 8U == 0 ? 0U : 1U);
            if (!format::checkedAdd(
                        nullBitmapBytes, bitmapBytes, nullBitmapBytes))
            {
                return format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "pixel null bitmap byte count overflows");
            }
        }
    }
    if (nullBitmapBytes != 0 && !pageChunk_.has_isnulloffset())
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "null-containing chunk has no null bitmap offset");
    }
    std::uint64_t nullBitmapEnd = 0;
    if (!format::checkedAdd(dataLength, nullBitmapBytes, nullBitmapEnd)
        || nullBitmapEnd > pageChunk_.chunklength())
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "pixel null bitmaps exceed the column chunk");
    }

    const std::uint64_t firstPixel =
            pageRequest_.rowOffset / pixelStride;
    if (firstPixel > std::numeric_limits<std::uint32_t>::max())
    {
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel index exceeds the supported range");
    }
    pageRequest_.nullBitmapByteOffset = 0;
    for (std::uint64_t pixel = 0; pixel < firstPixel; ++pixel)
    {
        if (!pixelHasNull(pageChunk_, static_cast<std::uint32_t>(pixel)))
        {
            continue;
        }
        const std::uint64_t pixelStart = pixel * pixelStride;
        const std::uint64_t rows = std::min<std::uint64_t>(
                pixelStride, rowGroupRows - pixelStart);
        const std::uint64_t bitmapBytes =
                rows / 8U + (rows % 8U == 0 ? 0U : 1U);
        pageRequest_.nullBitmapByteOffset += bitmapBytes;
    }
    return true;
}

bool InspectionSession::consumeColumnChunk(const format::ByteSpan &bytes)
{
    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    std::vector<std::string> physicalValues(pagePlan_.physicalCount);
    if (type.kind() == proto::Type_Kind_ARRAY
        || type.kind() == proto::Type_Kind_MAP)
    {
        std::uint64_t scalarCount64 = 0;
        if (!checkedMultiply(
                    pagePlan_.physicalCount, 2U, scalarCount64)
            || scalarCount64
               > std::numeric_limits<std::size_t>::max())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "nested collection range count overflows");
        }
        const std::size_t scalarCount =
                static_cast<std::size_t>(scalarCount64);
        std::vector<std::int64_t> decodedRanges(scalarCount);
        if (!format::PlainScalarDecoder::decodeInt64(
                    bytes, littleEndian, 0, scalarCount,
                    decodedRanges.data(), decodedRanges.size(), error_))
        {
            state_ = State::FAILED;
            return false;
        }
        std::vector<format::VariableValueRange> ranges(
                pagePlan_.physicalCount);
        for (std::size_t index = 0;
             index < ranges.size(); ++index)
        {
            const std::int64_t start = decodedRanges[index * 2U];
            const std::int64_t end = decodedRanges[index * 2U + 1U];
            if (start < 0 || end < start)
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "nested collection range is negative or reversed");
            }
            ranges[index].offset = static_cast<std::uint64_t>(start);
            ranges[index].length =
                    static_cast<std::uint64_t>(end - start);
        }
        return finishCollectionPixel(ranges);
    }
    if (isDictionaryPage())
    {
        std::vector<std::int64_t> pixelIds(
                pagePlan_.pixelPhysicalCount);
        bool decodedIds = false;
        if (usesCascadeRunLengthEncoding())
        {
            std::size_t consumed = 0;
            decodedIds = format::RunLengthIntDecoder::decode(
                    bytes, false, pixelIds.size(), pixelIds.data(),
                    pixelIds.size(), consumed, error_);
        }
        else
        {
            if (pixelIds.size()
                > std::numeric_limits<std::size_t>::max() / 4U
                || bytes.size() != pixelIds.size() * 4U)
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "dictionary ID field has an invalid pixel length");
            }
            std::vector<std::int32_t> decoded(pixelIds.size());
            decodedIds = format::PlainScalarDecoder::decodeInt32(
                    bytes, littleEndian, 0, decoded.size(),
                    decoded.data(), decoded.size(), error_);
            if (decodedIds)
            {
                for (std::size_t index = 0;
                     index < decoded.size(); ++index)
                {
                    pixelIds[index] = decoded[index];
                }
            }
        }
        if (!decodedIds)
        {
            state_ = State::FAILED;
            return false;
        }
        std::uint64_t physicalEnd = 0;
        if (!format::checkedAdd(
                    pagePlan_.physicalOffset, pagePlan_.physicalCount,
                    physicalEnd)
            || physicalEnd > pixelIds.size())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "dictionary page IDs exceed the decoded pixel");
        }
        format::ByteSpan dictionary(
                dictionaryContent_.data(),
                dictionaryContent_.size());
        for (std::size_t index = 0;
             index < physicalValues.size(); ++index)
        {
            const std::int64_t id =
                    pixelIds[pagePlan_.physicalOffset + index];
            if (id < 0
                || static_cast<std::uint64_t>(id)
                   >= dictionaryRanges_.size())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "dictionary ID is outside the dictionary");
            }
            const format::VariableValueRange &range =
                    dictionaryRanges_[static_cast<std::size_t>(id)];
            if (range.length > MAX_VALUE_BYTES)
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "dictionary value exceeds the bounded value limit");
            }
            if ((type.kind() == proto::Type_Kind_VARCHAR
                 || type.kind() == proto::Type_Kind_CHAR)
                && range.length > type.maximumlength())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "dictionary string exceeds its schema length");
            }
            format::ByteSpan value;
            if (range.length == 0)
            {
                value = format::ByteSpan();
            }
            else if (range.offset
                > std::numeric_limits<std::size_t>::max()
                || range.length
                   > std::numeric_limits<std::size_t>::max()
                || !dictionary.subspan(
                        static_cast<std::size_t>(range.offset),
                        static_cast<std::size_t>(range.length), value)
                || !format::VariableLengthDecoder::isValidUtf8(value))
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "dictionary string is invalid");
            }
            std::string text;
            if (!value.empty())
            {
                text.assign(
                        reinterpret_cast<const char *>(value.data()),
                        value.size());
            }
            physicalValues[index] =
                    "\"" + escapeJson(text) + "\"";
        }
        return finishCurrentPixel(physicalValues);
    }
    if (isLengthPrefixedBinary(type))
    {
        std::vector<format::VariableValueRange> pixelRanges;
        if (!format::VariableLengthDecoder::decodeLengthPrefixed(
                    bytes, pagePlan_.pixelPhysicalCount,
                    type.maximumlength(), pixelRanges, error_))
        {
            state_ = State::FAILED;
            return false;
        }
        std::uint64_t physicalEnd = 0;
        if (!format::checkedAdd(
                    pagePlan_.physicalOffset, pagePlan_.physicalCount,
                    physicalEnd)
            || physicalEnd > pixelRanges.size())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "binary page values exceed the decoded pixel");
        }
        for (std::size_t index = 0;
             index < physicalValues.size(); ++index)
        {
            const format::VariableValueRange &range =
                    pixelRanges[pagePlan_.physicalOffset + index];
            if (range.length > MAX_VALUE_BYTES)
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "binary value exceeds the bounded value limit");
            }
            format::ByteSpan value;
            if (range.offset
                > std::numeric_limits<std::size_t>::max()
                || range.length
                   > std::numeric_limits<std::size_t>::max()
                || !bytes.subspan(
                        static_cast<std::size_t>(range.offset),
                        static_cast<std::size_t>(range.length), value))
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "binary value exceeds the decoded pixel");
            }
            physicalValues[index] =
                    "\"" + encodeBase64(value) + "\"";
        }
        return finishCurrentPixel(physicalValues);
    }
    if (usesRunLengthEncoding())
    {
        if (type.kind() == proto::Type_Kind_BYTE)
        {
            std::vector<std::int8_t> pixelValues(
                    pagePlan_.pixelPhysicalCount);
            std::size_t consumedBytes = 0;
            if (!format::RunLengthByteDecoder::decode(
                        bytes, pixelValues.size(),
                        pixelValues.data(), pixelValues.size(),
                        consumedBytes, error_))
            {
                state_ = State::FAILED;
                return false;
            }
            std::uint64_t physicalEnd = 0;
            if (!format::checkedAdd(
                        pagePlan_.physicalOffset,
                        pagePlan_.physicalCount, physicalEnd)
                || physicalEnd > pixelValues.size())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "RLE byte page exceeds the decoded pixel");
            }
            for (std::size_t index = 0;
                 index < physicalValues.size(); ++index)
            {
                physicalValues[index] = quoteInteger(
                        pixelValues[
                                pagePlan_.physicalOffset + index]);
            }
            return finishCurrentPixel(physicalValues);
        }
        std::vector<std::int64_t> pixelValues(
                pagePlan_.pixelPhysicalCount);
        std::size_t consumedBytes = 0;
        if (!format::RunLengthIntDecoder::decode(
                    bytes, true, pixelValues.size(),
                    pixelValues.data(), pixelValues.size(),
                    consumedBytes, error_))
        {
            state_ = State::FAILED;
            return false;
        }
        std::uint64_t physicalEnd = 0;
        if (!format::checkedAdd(
                    pagePlan_.physicalOffset, pagePlan_.physicalCount,
                    physicalEnd)
            || physicalEnd > pixelValues.size())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "RLE page values exceed the decoded pixel");
        }
        for (std::size_t index = 0;
             index < physicalValues.size(); ++index)
        {
            physicalValues[index] = quoteInteger(
                    pixelValues[pagePlan_.physicalOffset + index]);
        }
        return finishCurrentPixel(physicalValues);
    }

    bool decoded = false;
    switch (type.kind())
    {
        case proto::Type_Kind_BOOLEAN:
        {
            std::unique_ptr<bool[]> decodedValues(
                    new bool[pagePlan_.physicalCount]);
            decoded = format::PlainScalarDecoder::decodeBoolean(
                    bytes, littleEndian, pageRequest_.bitOffset,
                    pagePlan_.physicalCount, decodedValues.get(),
                    pagePlan_.physicalCount, error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            decodedValues[index] ? "true" : "false";
                }
            }
            break;
        }
        case proto::Type_Kind_BYTE:
        {
            std::vector<std::int8_t> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeByte(
                    bytes, 0, decodedValues.size(), decodedValues.data(),
                    decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            quoteInteger(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_SHORT:
        case proto::Type_Kind_INT:
        case proto::Type_Kind_DATE:
        case proto::Type_Kind_TIME:
        {
            std::vector<std::int32_t> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeInt32(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            quoteInteger(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_LONG:
        case proto::Type_Kind_TIMESTAMP:
        {
            std::vector<std::int64_t> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeInt64(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            quoteInteger(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_FLOAT:
        {
            std::vector<float> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeFloat(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            formatFloating(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_DOUBLE:
        {
            std::vector<double> decodedValues(pagePlan_.physicalCount);
            decoded = format::PlainScalarDecoder::decodeDouble(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t index = 0;
                     index < physicalValues.size(); ++index)
                {
                    physicalValues[index] =
                            formatFloating(decodedValues[index]);
                }
            }
            break;
        }
        case proto::Type_Kind_DECIMAL:
        {
            if (type.precision() <= 18)
            {
                std::vector<std::int64_t> decodedValues(
                        pagePlan_.physicalCount);
                decoded = format::PlainScalarDecoder::decodeInt64(
                        bytes, littleEndian, 0, decodedValues.size(),
                        decodedValues.data(), decodedValues.size(), error_);
                if (decoded)
                {
                    for (std::size_t index = 0;
                         index < physicalValues.size(); ++index)
                    {
                        physicalValues[index] = formatDecimal(
                                decodedValues[index], type.scale());
                    }
                }
            }
            else
            {
                std::vector<format::Int128Words> decodedValues(
                        pagePlan_.physicalCount);
                decoded = format::PlainScalarDecoder::decodeInt128(
                        bytes, littleEndian, 0, decodedValues.size(),
                        decodedValues.data(), decodedValues.size(), error_);
                if (decoded)
                {
                    for (std::size_t index = 0;
                         index < physicalValues.size(); ++index)
                    {
                        physicalValues[index] = formatDecimal128(
                                decodedValues[index], type.scale());
                    }
                }
            }
            break;
        }
        case proto::Type_Kind_VECTOR:
        {
            const std::size_t dimension = type.dimension();
            if (pagePlan_.physicalCount
                > std::numeric_limits<std::size_t>::max() / dimension)
            {
                decoded = format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "VECTOR scalar count overflows");
                break;
            }
            const std::size_t scalarCount =
                    pagePlan_.physicalCount * dimension;
            std::vector<double> decodedValues(scalarCount);
            decoded = format::PlainScalarDecoder::decodeDouble(
                    bytes, littleEndian, 0, decodedValues.size(),
                    decodedValues.data(), decodedValues.size(), error_);
            if (decoded)
            {
                for (std::size_t value = 0;
                     value < physicalValues.size(); ++value)
                {
                    std::string formatted = "[";
                    for (std::size_t component = 0;
                         component < dimension; ++component)
                    {
                        if (component != 0)
                        {
                            formatted += ",";
                        }
                        formatted += formatFloating(
                                decodedValues[
                                        value * dimension + component]);
                    }
                    formatted += "]";
                    physicalValues[value] = formatted;
                }
            }
            break;
        }
        case proto::Type_Kind_STRING:
        case proto::Type_Kind_BINARY:
        case proto::Type_Kind_ARRAY:
        case proto::Type_Kind_MAP:
        case proto::Type_Kind_STRUCT:
        case proto::Type_Kind_VARBINARY:
        case proto::Type_Kind_VARCHAR:
        case proto::Type_Kind_CHAR:
            decoded = format::fail(
                    error_, format::ErrorCode::UNSUPPORTED_TYPE,
                    "column type is not a plain scalar");
            break;
    }
    if (!decoded)
    {
        state_ = State::FAILED;
        return false;
    }
    return finishCurrentPixel(physicalValues);
}

bool InspectionSession::finishCurrentPixel(
        const std::vector<std::string> &physicalValues)
{
    const bool nullsPadding = usesNullPadding();
    std::size_t physicalIndex = 0;
    for (std::uint32_t index = 0;
         index < pageRequest_.pixelRowCount; ++index)
    {
        const std::size_t resultIndex =
                static_cast<std::size_t>(pageRequest_.resultOffset) + index;
        if (pageValidity_[resultIndex])
        {
            if (physicalIndex >= physicalValues.size())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "plain scalar physical values end before the pixel");
            }
            pageValues_[resultIndex] = physicalValues[physicalIndex];
        }
        else
        {
            pageValues_[resultIndex] = "null";
        }
        if (nullsPadding || pageValidity_[resultIndex])
        {
            ++physicalIndex;
        }
    }
    if (physicalIndex != physicalValues.size())
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain scalar physical value count is inconsistent");
    }
    return advancePixel();
}

bool InspectionSession::finishCollectionPixel(
        const std::vector<format::VariableValueRange> &ranges)
{
    const bool nullsPadding = usesNullPadding();
    std::size_t physicalIndex = 0;
    for (std::uint32_t index = 0;
         index < pageRequest_.pixelRowCount; ++index)
    {
        const std::size_t resultIndex =
                static_cast<std::size_t>(pageRequest_.resultOffset) + index;
        if (pageValidity_[resultIndex])
        {
            if (physicalIndex >= ranges.size())
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "nested collection ranges end before the pixel");
            }
            collectionRanges_[resultIndex] = ranges[physicalIndex];
            pageValues_[resultIndex] = "[]";
        }
        else
        {
            pageValues_[resultIndex] = "null";
        }
        if (nullsPadding || pageValidity_[resultIndex])
        {
            ++physicalIndex;
        }
    }
    if (physicalIndex != ranges.size())
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "nested collection physical range count is inconsistent");
    }
    return advancePixel();
}

bool InspectionSession::usesRunLengthEncoding() const
{
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    return pageRequest_.column
           < static_cast<std::uint32_t>(
                   encodings.columnchunkencodings_size())
           && encodings.columnchunkencodings(
                      static_cast<int>(pageRequest_.column)).has_kind()
           && encodings.columnchunkencodings(
                      static_cast<int>(pageRequest_.column)).kind()
              == proto::ColumnEncoding_Kind_RUNLENGTH;
}

bool InspectionSession::usesCascadeRunLengthEncoding() const
{
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                encodings.columnchunkencodings_size()))
    {
        return false;
    }
    const proto::ColumnEncoding &encoding =
            encodings.columnchunkencodings(
                    static_cast<int>(pageRequest_.column));
    return encoding.has_kind()
           && encoding.kind()
              == proto::ColumnEncoding_Kind_DICTIONARY
           && encoding.has_cascadeencoding()
           && encoding.cascadeencoding().has_kind()
           && encoding.cascadeencoding().kind()
              == proto::ColumnEncoding_Kind_RUNLENGTH;
}

bool InspectionSession::usesNullPadding() const
{
    if (pageRequest_.column
        < static_cast<std::uint32_t>(
                fileTail_.footer().types_size()))
    {
        const proto::Type &type =
                fileTail_.footer().types(
                        static_cast<int>(pageRequest_.column));
        if (isLengthPrefixedBinary(type)
            || type.kind() == proto::Type_Kind_VECTOR)
        {
            return false;
        }
    }
    return !usesRunLengthEncoding()
           && !usesCascadeRunLengthEncoding()
           && pageChunk_.has_nullspadding()
           && pageChunk_.nullspadding();
}

bool InspectionSession::isPlainVariablePage() const
{
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                fileTail_.footer().types_size()))
    {
        return false;
    }
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                encodings.columnchunkencodings_size()))
    {
        return false;
    }
    const proto::ColumnEncoding &encoding =
            encodings.columnchunkencodings(
                    static_cast<int>(pageRequest_.column));
    return isPlainVariableString(
                   fileTail_.footer().types(
                           static_cast<int>(pageRequest_.column)))
           && encoding.has_kind()
           && encoding.kind() == proto::ColumnEncoding_Kind_NONE;
}

bool InspectionSession::isDictionaryPage() const
{
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                fileTail_.footer().types_size()))
    {
        return false;
    }
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    if (pageRequest_.column
        >= static_cast<std::uint32_t>(
                encodings.columnchunkencodings_size()))
    {
        return false;
    }
    const proto::ColumnEncoding &encoding =
            encodings.columnchunkencodings(
                    static_cast<int>(pageRequest_.column));
    return isPlainVariableString(
                   fileTail_.footer().types(
                           static_cast<int>(pageRequest_.column)))
           && encoding.has_kind()
           && encoding.kind()
              == proto::ColumnEncoding_Kind_DICTIONARY;
}

bool InspectionSession::isNestedPage() const
{
    return pageRequest_.column
           < static_cast<std::uint32_t>(
                   fileTail_.footer().types_size())
           && isNestedType(
                   fileTail_.footer().types(
                           static_cast<int>(pageRequest_.column)));
}

bool InspectionSession::computeVariablePhysicalBase(
        const format::ByteSpan &prefixNullBitmaps)
{
    const std::uint64_t pixelStride =
            fileTail_.postscript().pixelstride();
    const std::uint64_t rowGroupRows =
            fileTail_.footer().rowgroupinfos(
                    static_cast<int>(pageRequest_.rowGroup)).numberofrows();
    const bool littleEndian =
            pageChunk_.has_littleendian() && pageChunk_.littleendian();
    std::size_t bitmapOffset = 0;
    std::uint64_t physicalBase = 0;
    for (std::uint32_t pixel = 0;
         pixel < pageRequest_.pixel; ++pixel)
    {
        const std::uint64_t pixelStart =
                static_cast<std::uint64_t>(pixel) * pixelStride;
        const std::uint32_t rows = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                        pixelStride, rowGroupRows - pixelStart));
        if (!pixelHasNull(pageChunk_, pixel))
        {
            physicalBase += rows;
            continue;
        }
        const std::size_t bitmapBytes =
                rows / 8U + (rows % 8U == 0 ? 0U : 1U);
        format::ByteSpan bitmap;
        if (!prefixNullBitmaps.subspan(
                    bitmapOffset, bitmapBytes, bitmap))
        {
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "plain variable prefix null bitmap is truncated");
        }
        std::unique_ptr<bool[]> validity(new bool[rows]);
        format::PlainPixelPlan plan;
        if (!format::PlainPixelPlanner::plan(
                    rows, 0, rows, true, false, littleEndian,
                    bitmap, validity.get(), rows, plan, error_))
        {
            return false;
        }
        physicalBase += plan.pixelPhysicalCount;
        bitmapOffset += bitmapBytes;
    }
    if (bitmapOffset != prefixNullBitmaps.size())
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "plain variable prefix null bitmap has trailing bytes");
    }
    pageRequest_.pixelPhysicalBase = physicalBase;
    return true;
}

bool InspectionSession::beginNestedChildren()
{
    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    nestedChild_.reset();
    nestedChildValues_.clear();
    nestedChildIndex_ = 0;
    nestedChildBase_ = 0;
    nestedChildCount_ = 0;

    if (type.kind() == proto::Type_Kind_STRUCT)
    {
        nestedChildBase_ = pageRequest_.rowOffset;
        nestedChildCount_ = pageRequest_.rowCount;
        return startNestedChild();
    }

    bool hasRange = false;
    std::uint64_t expected = 0;
    for (std::size_t index = 0;
         index < collectionRanges_.size(); ++index)
    {
        if (!pageValidity_[index])
        {
            continue;
        }
        const format::VariableValueRange &range =
                collectionRanges_[index];
        std::uint64_t end = 0;
        if (!format::checkedAdd(range.offset, range.length, end))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "nested collection range end overflows");
        }
        if (!hasRange)
        {
            nestedChildBase_ = range.offset;
            expected = range.offset;
            hasRange = true;
        }
        if (range.offset != expected)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "nested collection ranges are not contiguous");
        }
        expected = end;
    }
    if (hasRange)
    {
        const std::uint64_t count = expected - nestedChildBase_;
        if (count > MAX_NESTED_ELEMENTS
            || count > std::numeric_limits<std::uint32_t>::max())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "nested collection page exceeds the element limit");
        }
        nestedChildCount_ = static_cast<std::uint32_t>(count);
    }
    return startNestedChild();
}

bool InspectionSession::logicalRowsForColumn(
        std::uint32_t column, std::uint32_t &rows)
{
    const proto::RowGroupIndex &index =
            rowGroupFooter_.rowgroupindexentry();
    if (column >= static_cast<std::uint32_t>(
                          index.columnchunkindexentries_size()))
    {
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "nested child column metadata is missing");
    }
    const proto::ColumnChunkIndex &chunk =
            index.columnchunkindexentries(static_cast<int>(column));
    std::uint64_t total = 0;
    for (int pixel = 0;
         pixel < chunk.pixelstatistics_size(); ++pixel)
    {
        const proto::PixelStatistic &pixelStatistic =
                chunk.pixelstatistics(pixel);
        if (!pixelStatistic.has_statistic()
            || !pixelStatistic.statistic().has_numberofvalues()
            || !format::checkedAdd(
                    total,
                    pixelStatistic.statistic().numberofvalues(),
                    total)
            || total > std::numeric_limits<std::uint32_t>::max())
        {
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "nested child logical row count is missing or invalid");
        }
    }
    rows = static_cast<std::uint32_t>(total);
    return true;
}

void InspectionSession::initializeNestedChild(
        const proto::FileTail &fileTail,
        std::uint32_t rowGroup, std::uint32_t logicalRows)
{
    fileTail_ = fileTail;
    fileTail_.mutable_footer()
            ->mutable_rowgroupinfos(static_cast<int>(rowGroup))
            ->set_numberofrows(logicalRows);
    result_.clear();
    error_.clear();
    state_ = State::METADATA_READY;
    hasPendingRange_ = false;
}

bool InspectionSession::startNestedChild()
{
    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    if (nestedChildIndex_
        >= static_cast<std::size_t>(type.subtypes_size()))
    {
        return finishNestedPage();
    }

    const std::uint32_t childColumn =
            type.subtypes(static_cast<int>(nestedChildIndex_));
    std::uint32_t logicalRows = 0;
    if (!logicalRowsForColumn(childColumn, logicalRows))
    {
        state_ = State::FAILED;
        return false;
    }
    std::uint64_t childEnd = 0;
    if (!format::checkedAdd(
                nestedChildBase_, nestedChildCount_, childEnd)
        || childEnd > logicalRows)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "nested child page exceeds its logical row count");
    }
    if (nestedChildCount_ == 0)
    {
        nestedChildValues_.push_back(std::vector<std::string>());
        ++nestedChildIndex_;
        return startNestedChild();
    }

    nestedChild_.reset(new InspectionSession(fileSize_));
    nestedChild_->initializeNestedChild(
            fileTail_, pageRequest_.rowGroup, logicalRows);
    if (!nestedChild_->beginPage(
                pageRequest_.rowGroup, childColumn,
                nestedChildBase_, nestedChildCount_))
    {
        error_ = nestedChild_->error();
        state_ = State::FAILED;
        return false;
    }
    format::FileRange range;
    if (!nestedChild_->nextRange(range))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::INVALID_STATE,
                "nested child did not request row-group metadata");
    }
    setPendingRange(range, State::AWAITING_NESTED_CHILD);
    return true;
}

bool InspectionSession::consumeNestedChild(
        const format::ByteSpan &bytes)
{
    if (!nestedChild_
        || !nestedChild_->supplyRange(pendingRange_, bytes))
    {
        if (nestedChild_)
        {
            error_ = nestedChild_->error();
        }
        else
        {
            (void) format::fail(
                    error_, format::ErrorCode::INVALID_STATE,
                    "nested child session is missing");
        }
        state_ = State::FAILED;
        return false;
    }

    format::FileRange range;
    if (nestedChild_->nextRange(range))
    {
        setPendingRange(range, State::AWAITING_NESTED_CHILD);
        return true;
    }
    if (nestedChild_->state() != State::PAGE_READY)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::INVALID_STATE,
                "nested child stopped before producing a page");
    }
    nestedChildValues_.push_back(nestedChild_->pageValues_);
    nestedChild_.reset();
    ++nestedChildIndex_;
    return startNestedChild();
}

bool InspectionSession::startOperationPage(
        std::uint32_t rowGroup, std::uint32_t column,
        std::uint64_t rowOffset, std::uint32_t rowCount)
{
    const proto::RowGroupInformation &information =
            fileTail_.footer().rowgroupinfos(static_cast<int>(rowGroup));
    operationChild_.reset(new InspectionSession(fileSize_));
    operationChild_->initializeNestedChild(
            fileTail_, rowGroup, information.numberofrows());
    if (!operationChild_->beginPage(
                rowGroup, column, rowOffset, rowCount))
    {
        error_ = operationChild_->error();
        state_ = State::FAILED;
        return false;
    }
    format::FileRange range;
    if (!operationChild_->nextRange(range))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::INVALID_STATE,
                "operation child did not request row-group metadata");
    }
    setPendingRange(range, State::AWAITING_OPERATION_CHILD);
    return true;
}

bool InspectionSession::consumeOperationChild(
        const format::ByteSpan &bytes)
{
    if (!operationChild_
        || !operationChild_->supplyRange(pendingRange_, bytes))
    {
        if (operationChild_)
        {
            error_ = operationChild_->error();
        }
        else
        {
            (void) format::fail(
                    error_, format::ErrorCode::INVALID_STATE,
                    "operation child session is missing");
        }
        state_ = State::FAILED;
        return false;
    }
    format::FileRange range;
    if (operationChild_->nextRange(range))
    {
        setPendingRange(range, State::AWAITING_OPERATION_CHILD);
        return true;
    }
    if (operationChild_->state() != State::PAGE_READY)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::INVALID_STATE,
                "operation child stopped before producing a page");
    }

    const std::vector<std::string> values =
            operationChild_->pageValues_;
    operationChild_.reset();
    if (operation_ == Operation::ROWS)
    {
        operationColumnValues_.push_back(values);
        ++operationColumnIndex_;
        return continueRows();
    }
    if (operation_ != Operation::FILTER)
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::INVALID_STATE,
                "operation child completed without an active operation");
    }

    if (!filterRequest_.predicateReady)
    {
        filterRequest_.predicateValues = values;
        filterRequest_.matchingRows.clear();
        filterRequest_.scannedRows += filterRequest_.batchCount;
        for (std::uint32_t index = 0;
             index < filterRequest_.batchCount; ++index)
        {
            bool matches = false;
            if (!filterValueMatches(
                        fileTail_.footer().types(
                                static_cast<int>(
                                        filterRequest_.predicateColumn)),
                        values[index], matches))
            {
                state_ = State::FAILED;
                return false;
            }
            if (matches)
            {
                filterRequest_.matchingRows.push_back(index);
            }
        }
        filterRequest_.predicateReady = true;
        operationColumnValues_.assign(
                operationColumns_.size(), std::vector<std::string>());
        operationColumnIndex_ = 0;
        return continueFilter();
    }

    if (operationColumnIndex_ >= operationColumnValues_.size())
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::INVALID_STATE,
                "filter projection child index is inconsistent");
    }
    operationColumnValues_[operationColumnIndex_] = values;
    ++operationColumnIndex_;
    return continueFilter();
}

bool InspectionSession::continueRows()
{
    if (operationColumnIndex_ < operationColumns_.size())
    {
        return startOperationPage(
                operationRowGroup_,
                operationColumns_[operationColumnIndex_],
                operationRowOffset_, operationRowCount_);
    }
    return finishRows();
}

std::uint64_t InspectionSession::absoluteRow(
        std::uint32_t rowGroup, std::uint64_t localRow) const
{
    std::uint64_t absolute = localRow;
    for (std::uint32_t group = 0; group < rowGroup; ++group)
    {
        const proto::RowGroupInformation &information =
                fileTail_.footer().rowgroupinfos(
                        static_cast<int>(group));
        if (!format::checkedAdd(
                    absolute, information.numberofrows(), absolute))
        {
            return std::numeric_limits<std::uint64_t>::max();
        }
    }
    return absolute;
}

bool InspectionSession::finishRows()
{
    if (operationColumnValues_.size() != operationColumns_.size())
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::INVALID_STATE,
                "row projection column count is inconsistent");
    }
    std::ostringstream output;
    output << "{\"operation\":\"rows-v1\",\"rowGroup\":"
           << operationRowGroup_
           << ",\"offset\":\"" << operationRowOffset_
           << "\",\"count\":" << operationRowCount_
           << ",\"columns\":[";
    for (std::size_t column = 0;
         column < operationColumns_.size(); ++column)
    {
        if (column != 0)
        {
            output << ",";
        }
        const std::uint32_t id = operationColumns_[column];
        const proto::Type &type =
                fileTail_.footer().types(static_cast<int>(id));
        output << "{\"id\":" << id
               << ",\"name\":\"" << escapeJson(type.name())
               << "\",\"kind\":" << static_cast<int>(type.kind())
               << "}";
    }
    output << "],\"rows\":[";
    for (std::uint32_t row = 0; row < operationRowCount_; ++row)
    {
        if (row != 0)
        {
            output << ",";
        }
        const std::uint64_t local = operationRowOffset_ + row;
        output << "{\"rowGroup\":" << operationRowGroup_
               << ",\"localRow\":\"" << local
               << "\",\"absoluteRow\":\""
               << absoluteRow(operationRowGroup_, local)
               << "\",\"values\":[";
        for (std::size_t column = 0;
             column < operationColumnValues_.size(); ++column)
        {
            if (column != 0)
            {
                output << ",";
            }
            if (operationColumnValues_[column].size()
                != operationRowCount_)
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::INVALID_STATE,
                        "row projection is not rectangular");
            }
            output << operationColumnValues_[column][row];
        }
        output << "]}";
    }
    output << "]}";
    result_ = output.str();
    if (result_.size() > MAX_RESULT_OUTPUT_BYTES)
    {
        result_.clear();
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "row projection exceeds the bounded output limit");
    }
    operation_ = Operation::NONE;
    resetOperation();
    state_ = State::PAGE_READY;
    return true;
}

bool InspectionSession::filterValueMatches(
        const proto::Type &type, const std::string &value,
        bool &matches)
{
    matches = false;
    if (value == "null")
    {
        matches =
                filterRequest_.filterOperator
                == PIXELS_INSPECTOR_FILTER_IS_NULL;
        return true;
    }
    if (filterRequest_.filterOperator
        == PIXELS_INSPECTOR_FILTER_IS_NULL)
    {
        return true;
    }
    if (filterRequest_.filterOperator
        == PIXELS_INSPECTOR_FILTER_IS_NOT_NULL)
    {
        matches = true;
        return true;
    }
    if (filterRequest_.filterOperator
        == PIXELS_INSPECTOR_FILTER_CONTAINS)
    {
        std::string text;
        if (!parseJsonString(value, text))
        {
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "decoded string predicate value is invalid JSON");
        }
        matches = text.find(filterRequest_.literal)
                  != std::string::npos;
        return true;
    }

    if ((type.kind() == proto::Type_Kind_FLOAT
         || type.kind() == proto::Type_Kind_DOUBLE)
        && !value.empty() && value.front() == '"')
    {
        std::string special;
        if (!parseJsonString(value, special)
            || (special != "NaN"
                && special != "Infinity"
                && special != "-Infinity"))
        {
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "decoded floating predicate value is invalid");
        }
        if (special == "NaN")
        {
            matches =
                    filterRequest_.filterOperator
                    == PIXELS_INSPECTOR_FILTER_NE;
            return true;
        }
        const int comparison =
                special == "-Infinity" ? -1 : 1;
        matches = applyComparison(
                filterRequest_.filterOperator, comparison);
        return true;
    }

    int comparison = 0;
    if (!compareTypedLiteral(
                type, value, filterRequest_.literal, comparison))
    {
        return format::fail(
                error_, format::ErrorCode::INVALID_ARGUMENT,
                "filter literal is not canonical for its column type");
    }
    matches = applyComparison(
            filterRequest_.filterOperator, comparison);
    return true;
}

bool InspectionSession::rowGroupCanBePruned(
        std::uint32_t rowGroup, bool &pruned) const
{
    pruned = false;
    const proto::Footer &footer = fileTail_.footer();
    if (rowGroup >= static_cast<std::uint32_t>(
                           footer.rowgroupstats_size()))
    {
        return true;
    }
    const proto::RowGroupStatistic &group =
            footer.rowgroupstats(static_cast<int>(rowGroup));
    if (filterRequest_.predicateColumn
        >= static_cast<std::uint32_t>(
                group.columnchunkstats_size()))
    {
        return true;
    }
    const proto::ColumnStatistic &statistic =
            group.columnchunkstats(
                    static_cast<int>(filterRequest_.predicateColumn));
    if (filterRequest_.filterOperator
        == PIXELS_INSPECTOR_FILTER_IS_NULL)
    {
        pruned = statistic.has_hasnull() && !statistic.hasnull();
        return true;
    }
    if (filterRequest_.filterOperator
        == PIXELS_INSPECTOR_FILTER_IS_NOT_NULL)
    {
        pruned = statistic.has_numberofvalues()
                 && statistic.numberofvalues() == 0;
        return true;
    }
    if (filterRequest_.filterOperator
        == PIXELS_INSPECTOR_FILTER_CONTAINS)
    {
        return true;
    }

    const proto::Type &type =
            footer.types(static_cast<int>(
                    filterRequest_.predicateColumn));
    std::string minimum;
    std::string maximum;
    if (type.kind() == proto::Type_Kind_STRING
        || type.kind() == proto::Type_Kind_VARCHAR
        || type.kind() == proto::Type_Kind_CHAR)
    {
        if (!statistic.has_stringstatistics()
            || !statistic.stringstatistics().has_minimum()
            || !statistic.stringstatistics().has_maximum())
        {
            return true;
        }
        minimum = "\"" + escapeJson(
                statistic.stringstatistics().minimum()) + "\"";
        maximum = "\"" + escapeJson(
                statistic.stringstatistics().maximum()) + "\"";
    }
    else if (type.kind() == proto::Type_Kind_BYTE
             || type.kind() == proto::Type_Kind_SHORT
             || type.kind() == proto::Type_Kind_INT
             || type.kind() == proto::Type_Kind_LONG)
    {
        if (!statistic.has_intstatistics()
            || !statistic.intstatistics().has_minimum()
            || !statistic.intstatistics().has_maximum())
        {
            return true;
        }
        minimum = quoteInteger(statistic.intstatistics().minimum());
        maximum = quoteInteger(statistic.intstatistics().maximum());
    }
    else if (type.kind() == proto::Type_Kind_DATE)
    {
        if (!statistic.has_datestatistics()
            || !statistic.datestatistics().has_minimum()
            || !statistic.datestatistics().has_maximum())
        {
            return true;
        }
        minimum = quoteInteger(statistic.datestatistics().minimum());
        maximum = quoteInteger(statistic.datestatistics().maximum());
    }
    else if (type.kind() == proto::Type_Kind_TIME)
    {
        if (!statistic.has_timestatistics()
            || !statistic.timestatistics().has_minimum()
            || !statistic.timestatistics().has_maximum())
        {
            return true;
        }
        minimum = quoteInteger(statistic.timestatistics().minimum());
        maximum = quoteInteger(statistic.timestatistics().maximum());
    }
    else if (type.kind() == proto::Type_Kind_FLOAT
             || type.kind() == proto::Type_Kind_DOUBLE)
    {
        if (!statistic.has_doublestatistics()
            || !statistic.doublestatistics().has_minimum()
            || !statistic.doublestatistics().has_maximum()
            || !std::isfinite(statistic.doublestatistics().minimum())
            || !std::isfinite(statistic.doublestatistics().maximum()))
        {
            return true;
        }
        std::ostringstream minStream;
        std::ostringstream maxStream;
        minStream << std::setprecision(17)
                  << statistic.doublestatistics().minimum();
        maxStream << std::setprecision(17)
                  << statistic.doublestatistics().maximum();
        minimum = minStream.str();
        maximum = maxStream.str();
    }
    else
    {
        // TIMESTAMP statistics are milliseconds while values are
        // microseconds, and long DECIMAL statistics use split 128-bit
        // words. Ambiguous statistics must scan to avoid false negatives.
        return true;
    }

    int minComparison = 0;
    int maxComparison = 0;
    if (!compareTypedLiteral(
                type, minimum, filterRequest_.literal, minComparison)
        || !compareTypedLiteral(
                type, maximum, filterRequest_.literal, maxComparison))
    {
        return true;
    }
    switch (filterRequest_.filterOperator)
    {
        case PIXELS_INSPECTOR_FILTER_EQ:
            pruned = minComparison > 0 || maxComparison < 0;
            break;
        case PIXELS_INSPECTOR_FILTER_NE:
            pruned = minComparison == 0 && maxComparison == 0;
            break;
        case PIXELS_INSPECTOR_FILTER_LT:
            pruned = minComparison >= 0;
            break;
        case PIXELS_INSPECTOR_FILTER_LE:
            pruned = minComparison > 0;
            break;
        case PIXELS_INSPECTOR_FILTER_GT:
            pruned = maxComparison <= 0;
            break;
        case PIXELS_INSPECTOR_FILTER_GE:
            pruned = maxComparison < 0;
            break;
        default:
            break;
    }
    return true;
}

bool InspectionSession::continueFilter()
{
    const proto::Footer &footer = fileTail_.footer();
    while (!filterRequest_.predicateReady)
    {
        if (filterRequest_.rowGroup
            >= static_cast<std::uint32_t>(
                    footer.rowgroupinfos_size()))
        {
            return finishFilter(true);
        }
        const proto::RowGroupInformation &information =
                footer.rowgroupinfos(
                        static_cast<int>(filterRequest_.rowGroup));
        if (!information.has_numberofrows())
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "filter row group is missing its row count");
        }
        if (filterRequest_.rowOffset >= information.numberofrows())
        {
            ++filterRequest_.rowGroup;
            filterRequest_.rowOffset = 0;
            continue;
        }
        if (filterRequest_.rowOffset == 0)
        {
            bool pruned = false;
            if (!rowGroupCanBePruned(
                        filterRequest_.rowGroup, pruned))
            {
                state_ = State::FAILED;
                return false;
            }
            if (pruned)
            {
                ++filterRequest_.prunedRowGroups;
                filterRequest_.prunedRows +=
                        information.numberofrows();
                ++filterRequest_.rowGroup;
                continue;
            }
        }
        if (filterRequest_.countedRowGroup
            != filterRequest_.rowGroup)
        {
            ++filterRequest_.scannedRowGroups;
            filterRequest_.countedRowGroup =
                    filterRequest_.rowGroup;
        }
        const std::uint64_t remaining =
                information.numberofrows()
                - filterRequest_.rowOffset;
        filterRequest_.batchCount =
                static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(
                                remaining, MAX_OPERATION_ROWS));
        return startOperationPage(
                filterRequest_.rowGroup,
                filterRequest_.predicateColumn,
                filterRequest_.rowOffset,
                filterRequest_.batchCount);
    }

    if (filterRequest_.matchingRows.empty())
    {
        filterRequest_.rowOffset += filterRequest_.batchCount;
        filterRequest_.predicateReady = false;
        filterRequest_.predicateValues.clear();
        return continueFilter();
    }

    while (operationColumnIndex_ < operationColumns_.size())
    {
        if (operationColumns_[operationColumnIndex_]
            == filterRequest_.predicateColumn)
        {
            operationColumnValues_[operationColumnIndex_] =
                    filterRequest_.predicateValues;
            ++operationColumnIndex_;
            continue;
        }
        return startOperationPage(
                filterRequest_.rowGroup,
                operationColumns_[operationColumnIndex_],
                filterRequest_.rowOffset,
                filterRequest_.batchCount);
    }

    for (std::uint32_t match : filterRequest_.matchingRows)
    {
        filterResultRowGroups_.push_back(filterRequest_.rowGroup);
        filterResultLocalRows_.push_back(
                filterRequest_.rowOffset + match);
        std::vector<std::string> row;
        row.reserve(operationColumns_.size());
        for (const std::vector<std::string> &column :
             operationColumnValues_)
        {
            if (column.size() != filterRequest_.batchCount)
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::INVALID_STATE,
                        "filter projection is not rectangular");
            }
            row.push_back(column[match]);
        }
        filterResultRows_.push_back(row);
        if (filterResultRows_.size() == filterRequest_.limit)
        {
            filterRequest_.rowOffset +=
                    static_cast<std::uint64_t>(match) + 1U;
            const proto::RowGroupInformation &information =
                    footer.rowgroupinfos(
                            static_cast<int>(filterRequest_.rowGroup));
            if (filterRequest_.rowOffset
                >= information.numberofrows())
            {
                ++filterRequest_.rowGroup;
                filterRequest_.rowOffset = 0;
            }
            if (filterRequest_.rowGroup
                >= static_cast<std::uint32_t>(
                        footer.rowgroupinfos_size()))
            {
                return finishFilter(true);
            }
            return finishFilter(false);
        }
    }

    filterRequest_.rowOffset += filterRequest_.batchCount;
    filterRequest_.predicateReady = false;
    filterRequest_.predicateValues.clear();
    filterRequest_.matchingRows.clear();
    operationColumnValues_.clear();
    operationColumnIndex_ = 0;
    return continueFilter();
}

bool InspectionSession::finishFilter(bool completed)
{
    std::ostringstream output;
    output << "{\"operation\":\"filter-v1\",\"columns\":[";
    for (std::size_t column = 0;
         column < operationColumns_.size(); ++column)
    {
        if (column != 0)
        {
            output << ",";
        }
        const std::uint32_t id = operationColumns_[column];
        const proto::Type &type =
                fileTail_.footer().types(static_cast<int>(id));
        output << "{\"id\":" << id
               << ",\"name\":\"" << escapeJson(type.name())
               << "\",\"kind\":" << static_cast<int>(type.kind())
               << "}";
    }
    output << "],\"rows\":[";
    for (std::size_t row = 0; row < filterResultRows_.size(); ++row)
    {
        if (row != 0)
        {
            output << ",";
        }
        output << "{\"rowGroup\":" << filterResultRowGroups_[row]
               << ",\"localRow\":\"" << filterResultLocalRows_[row]
               << "\",\"absoluteRow\":\""
               << absoluteRow(
                       filterResultRowGroups_[row],
                       filterResultLocalRows_[row])
               << "\",\"values\":[";
        for (std::size_t column = 0;
             column < filterResultRows_[row].size(); ++column)
        {
            if (column != 0)
            {
                output << ",";
            }
            output << filterResultRows_[row][column];
        }
        output << "]}";
    }
    output << "],\"progress\":{\"scannedRowGroups\":"
           << filterRequest_.scannedRowGroups
           << ",\"prunedRowGroups\":"
           << filterRequest_.prunedRowGroups
           << ",\"scannedRows\":\""
           << filterRequest_.scannedRows
           << "\",\"prunedRows\":\""
           << filterRequest_.prunedRows
           << "\"},\"matched\":" << filterResultRows_.size()
           << ",\"completed\":" << (completed ? "true" : "false")
           << ",\"truncated\":" << (completed ? "false" : "true")
           << ",\"cursor\":";
    if (completed)
    {
        output << "null";
    }
    else
    {
        output << "\"v1:" << filterRequest_.rowGroup
               << ":" << filterRequest_.rowOffset << "\"";
    }
    output << "}";
    result_ = output.str();
    if (result_.size() > MAX_RESULT_OUTPUT_BYTES)
    {
        result_.clear();
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "filter result exceeds the bounded output limit");
    }
    operation_ = Operation::NONE;
    resetOperation();
    state_ = State::PAGE_READY;
    return true;
}

bool InspectionSession::finishNestedPage()
{
    const proto::Type &type = fileTail_.footer().types(
            static_cast<int>(pageRequest_.column));
    if (nestedChildValues_.size()
        != static_cast<std::size_t>(type.subtypes_size()))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::INVALID_STATE,
                "nested child result count is inconsistent");
    }

    if (type.kind() == proto::Type_Kind_STRUCT)
    {
        for (std::size_t row = 0; row < pageValues_.size(); ++row)
        {
            if (!pageValidity_[row])
            {
                pageValues_[row] = "null";
                continue;
            }
            std::string value = "{";
            for (std::size_t child = 0;
                 child < nestedChildValues_.size(); ++child)
            {
                if (nestedChildValues_[child].size()
                    != pageValues_.size())
                {
                    state_ = State::FAILED;
                    return format::fail(
                            error_, format::ErrorCode::MALFORMED_PROTOBUF,
                            "STRUCT child row count is inconsistent");
                }
                if (child != 0)
                {
                    value += ",";
                }
                const std::uint32_t childColumn =
                        type.subtypes(static_cast<int>(child));
                const proto::Type &childType =
                        fileTail_.footer().types(
                                static_cast<int>(childColumn));
                value += "\"" + escapeJson(childType.name()) + "\":";
                value += nestedChildValues_[child][row];
            }
            value += "}";
            pageValues_[row] = value;
        }
    }
    else
    {
        const bool map = type.kind() == proto::Type_Kind_MAP;
        if (nestedChildValues_.empty()
            || (map && nestedChildValues_.size() != 2U))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::INVALID_STATE,
                    "collection child result count is inconsistent");
        }
        for (std::size_t row = 0; row < pageValues_.size(); ++row)
        {
            if (!pageValidity_[row])
            {
                pageValues_[row] = "null";
                continue;
            }
            const format::VariableValueRange &range =
                    collectionRanges_[row];
            if (range.offset < nestedChildBase_)
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::MALFORMED_PROTOBUF,
                        "collection range precedes the child page");
            }
            const std::uint64_t relative =
                    range.offset - nestedChildBase_;
            std::uint64_t end = 0;
            if (!format::checkedAdd(relative, range.length, end)
                || end > nestedChildValues_[0].size()
                || (map && end > nestedChildValues_[1].size()))
            {
                state_ = State::FAILED;
                return format::fail(
                        error_, format::ErrorCode::OUT_OF_BOUNDS,
                        "collection range exceeds the child page");
            }
            std::string value = "[";
            for (std::uint64_t index = relative;
                 index < end; ++index)
            {
                if (index != relative)
                {
                    value += ",";
                }
                if (map)
                {
                    value += "[";
                    value += nestedChildValues_[0][
                            static_cast<std::size_t>(index)];
                    value += ",";
                    value += nestedChildValues_[1][
                            static_cast<std::size_t>(index)];
                    value += "]";
                }
                else
                {
                    value += nestedChildValues_[0][
                            static_cast<std::size_t>(index)];
                }
            }
            value += "]";
            pageValues_[row] = value;
        }
    }

    if (!buildPageResult(pageValues_))
    {
        return false;
    }
    state_ = State::PAGE_READY;
    return true;
}

bool InspectionSession::advancePixel()
{
    std::uint32_t pixelRows = 0;
    if (!currentPixelRows(pixelRows))
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel is outside the row group");
    }
    if (pixelHasNull(pageChunk_, pageRequest_.pixel))
    {
        const std::uint64_t bitmapBytes =
                pixelRows / 8U + (pixelRows % 8U == 0 ? 0U : 1U);
        if (!format::checkedAdd(
                    pageRequest_.nullBitmapByteOffset, bitmapBytes,
                    pageRequest_.nullBitmapByteOffset))
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "pixel null bitmap offset overflows");
        }
    }
    if (isPlainVariablePage()
        && !format::checkedAdd(
                pageRequest_.pixelPhysicalBase,
                pagePlan_.pixelPhysicalCount,
                pageRequest_.pixelPhysicalBase))
    {
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "plain variable physical index overflows");
    }

    pageRequest_.resultOffset += pageRequest_.pixelRowCount;
    if (pageRequest_.resultOffset == pageRequest_.rowCount)
    {
        if (isNestedPage())
        {
            return beginNestedChildren();
        }
        if (!buildPageResult(pageValues_))
        {
            return false;
        }
        state_ = State::PAGE_READY;
        return true;
    }
    if (pageRequest_.pixel
        == std::numeric_limits<std::uint32_t>::max())
    {
        state_ = State::FAILED;
        return format::fail(error_, format::ErrorCode::OUT_OF_BOUNDS,
                            "page pixel index overflows");
    }
    ++pageRequest_.pixel;
    pageRequest_.pixelRowOffset = 0;
    return requestCurrentPixel();
}

bool InspectionSession::currentPixelRows(std::uint32_t &rows) const
{
    const std::uint64_t pixelStride =
            fileTail_.postscript().pixelstride();
    const std::uint64_t rowGroupRows =
            fileTail_.footer().rowgroupinfos(
                    static_cast<int>(pageRequest_.rowGroup)).numberofrows();
    std::uint64_t pixelStart = 0;
    if (pixelStride == 0
        || !checkedMultiply(pageRequest_.pixel, pixelStride, pixelStart)
        || pixelStart >= rowGroupRows)
    {
        rows = 0;
        return false;
    }
    rows = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                    pixelStride, rowGroupRows - pixelStart));
    return true;
}

bool InspectionSession::currentPixelDataRange(
        std::uint64_t &offset, std::uint64_t &length) const
{
    if (pageRequest_.pixel
        >= static_cast<std::uint32_t>(
                pageChunk_.pixelpositions_size()))
    {
        return false;
    }
    offset = pageChunk_.pixelpositions(
            static_cast<int>(pageRequest_.pixel));
    const std::uint64_t end =
            pageRequest_.pixel + 1U
                    < static_cast<std::uint32_t>(
                              pageChunk_.pixelpositions_size())
            ? pageChunk_.pixelpositions(
                    static_cast<int>(pageRequest_.pixel + 1U))
            : (pageChunk_.has_isnulloffset()
               ? pageChunk_.isnulloffset()
               : pageChunk_.chunklength());
    if (end < offset)
    {
        return false;
    }
    length = end - offset;
    return true;
}

bool InspectionSession::currentNullBitmapRange(
        format::FileRange &range) const
{
    if (!pageChunk_.has_isnulloffset())
    {
        return false;
    }
    std::uint32_t pixelRows = 0;
    if (!currentPixelRows(pixelRows))
    {
        return false;
    }
    const std::uint64_t bitmapBytes =
            pixelRows / 8U + (pixelRows % 8U == 0 ? 0U : 1U);
    std::uint64_t chunkOffset = 0;
    std::uint64_t chunkEnd = 0;
    if (!format::checkedAdd(
                pageChunk_.isnulloffset(),
                pageRequest_.nullBitmapByteOffset, chunkOffset)
        || !format::checkedAdd(chunkOffset, bitmapBytes, chunkEnd)
        || chunkEnd > pageChunk_.chunklength()
        || !format::checkedAdd(
                pageChunk_.chunkoffset(), chunkOffset, range.offset))
    {
        return false;
    }
    range.length = bitmapBytes;
    return format::isRangeWithinFile(range, fileSize_);
}

bool InspectionSession::transitionFailure(
        format::ErrorCode code, const std::string &message)
{
    state_ = State::FAILED;
    hasPendingRange_ = false;
    result_.clear();
    return format::fail(error_, code, message);
}

void InspectionSession::setPendingRange(
        const format::FileRange &range, State state)
{
    pendingRange_ = range;
    hasPendingRange_ = true;
    state_ = state;
}

bool InspectionSession::buildMetadataResult()
{
    const proto::PostScript &postScript = fileTail_.postscript();
    const proto::Footer &footer = fileTail_.footer();
    const proto::Type &firstType = footer.types(0);

    std::ostringstream output;
    output << "{\"abi\":" << PIXELS_INSPECTOR_ABI_VERSION
           << ",\"version\":" << postScript.version()
           << ",\"magic\":\"" << escapeJson(postScript.magic()) << "\""
           << ",\"rows\":" << postScript.numberofrows()
           << ",\"pixelStride\":" << postScript.pixelstride()
           << ",\"schemaCount\":" << footer.types_size()
           << ",\"rowGroupCount\":" << footer.rowgroupinfos_size()
           << ",\"firstColumn\":{\"name\":\""
           << escapeJson(firstType.name()) << "\",\"kind\":"
           << static_cast<int>(firstType.kind()) << "}"
           << ",\"postscript\":{"
           << "\"contentLength\":\""
           << (postScript.has_contentlength()
               ? postScript.contentlength() : 0)
           << "\",\"compression\":"
           << (postScript.has_compression()
               ? static_cast<int>(postScript.compression()) : 0)
           << ",\"compressionBlockSize\":"
           << (postScript.has_compressionblocksize()
               ? postScript.compressionblocksize() : 0)
           << ",\"writerTimezone\":\""
           << escapeJson(
                   postScript.has_writertimezone()
                   ? postScript.writertimezone() : "")
           << "\",\"partitioned\":"
           << (postScript.has_partitioned() && postScript.partitioned()
               ? "true" : "false")
           << ",\"columnChunkAlignment\":"
           << (postScript.has_columnchunkalignment()
               ? postScript.columnchunkalignment() : 0)
           << ",\"hasHiddenColumn\":"
           << (postScript.has_hashiddencolumn()
               && postScript.hashiddencolumn()
               ? "true" : "false")
           << "},\"schema\":[";
    for (int index = 0; index < footer.types_size(); ++index)
    {
        if (index != 0)
        {
            output << ",";
        }
        const proto::Type &type = footer.types(index);
        output << "{\"id\":" << index
               << ",\"name\":\"" << escapeJson(type.name()) << "\""
               << ",\"kind\":" << static_cast<int>(type.kind())
               << ",\"subtypes\":[";
        for (int child = 0; child < type.subtypes_size(); ++child)
        {
            if (child != 0)
            {
                output << ",";
            }
            output << type.subtypes(child);
        }
        output << "]";
        if (type.has_maximumlength())
        {
            output << ",\"maximumLength\":"
                   << type.maximumlength();
        }
        if (type.has_precision())
        {
            output << ",\"precision\":" << type.precision();
        }
        if (type.has_scale())
        {
            output << ",\"scale\":" << type.scale();
        }
        if (type.has_dimension())
        {
            output << ",\"dimension\":" << type.dimension();
        }
        output << "}";
    }
    output << "],\"fileStatistics\":[";
    for (int index = 0; index < footer.columnstats_size(); ++index)
    {
        if (index != 0)
        {
            output << ",";
        }
        appendStatisticJson(output, footer.columnstats(index));
    }
    output << "],\"rowGroups\":[";
    for (int index = 0;
         index < footer.rowgroupinfos_size(); ++index)
    {
        if (index != 0)
        {
            output << ",";
        }
        const proto::RowGroupInformation &rowGroup =
                footer.rowgroupinfos(index);
        output << "{\"index\":" << index
               << ",\"footerOffset\":\""
               << rowGroup.footeroffset() << "\""
               << ",\"footerLength\":"
               << rowGroup.footerlength()
               << ",\"dataLength\":"
               << (rowGroup.has_datalength()
                   ? rowGroup.datalength() : 0)
               << ",\"rows\":"
               << (rowGroup.has_numberofrows()
                   ? rowGroup.numberofrows() : 0);
        if (rowGroup.has_partitioninfo())
        {
            const proto::PartitionInformation &partition =
                    rowGroup.partitioninfo();
            output << ",\"partition\":{\"hash\":"
                   << (partition.has_hashvalue()
                       ? partition.hashvalue() : 0)
                   << ",\"columns\":[";
            for (int column = 0;
                 column < partition.columnids_size(); ++column)
            {
                if (column != 0)
                {
                    output << ",";
                }
                output << partition.columnids(column);
            }
            output << "]}";
        }
        output << "}";
    }
    output << "],\"rowGroupStatistics\":[";
    for (int rowGroup = 0;
         rowGroup < footer.rowgroupstats_size(); ++rowGroup)
    {
        if (rowGroup != 0)
        {
            output << ",";
        }
        const proto::RowGroupStatistic &statistics =
                footer.rowgroupstats(rowGroup);
        output << "[";
        for (int column = 0;
             column < statistics.columnchunkstats_size(); ++column)
        {
            if (column != 0)
            {
                output << ",";
            }
            appendStatisticJson(
                    output, statistics.columnchunkstats(column));
        }
        output << "]";
    }
    output << "]}";
    result_ = output.str();
    if (result_.size() > MAX_RESULT_OUTPUT_BYTES)
    {
        result_.clear();
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "metadata result exceeds the bounded output limit");
    }
    return true;
}

bool InspectionSession::buildRowGroupResult()
{
    const proto::RowGroupIndex &index =
            rowGroupFooter_.rowgroupindexentry();
    const proto::RowGroupEncoding &encodings =
            rowGroupFooter_.rowgroupencoding();
    const int schemaCount = fileTail_.footer().types_size();
    if (index.columnchunkindexentries_size() != schemaCount
        || encodings.columnchunkencodings_size() != schemaCount)
    {
        return format::fail(
                error_, format::ErrorCode::MALFORMED_PROTOBUF,
                "row-group column metadata does not match the schema");
    }

    std::ostringstream output;
    output << "{\"rowGroup\":" << requestedRowGroup_
           << ",\"columns\":[";
    for (int column = 0; column < schemaCount; ++column)
    {
        if (column != 0)
        {
            output << ",";
        }
        const proto::ColumnChunkIndex &chunk =
                index.columnchunkindexentries(column);
        const proto::ColumnEncoding &encoding =
                encodings.columnchunkencodings(column);
        output << "{\"column\":" << column
               << ",\"encoding\":{\"kind\":"
               << static_cast<int>(encoding.kind());
        if (encoding.has_dictionarysize())
        {
            output << ",\"dictionarySize\":"
                   << encoding.dictionarysize();
        }
        if (encoding.has_cascadeencoding()
            && encoding.cascadeencoding().has_kind())
        {
            output << ",\"cascadeKind\":"
                   << static_cast<int>(
                           encoding.cascadeencoding().kind());
        }
        output << "},\"chunk\":{\"offset\":\""
               << chunk.chunkoffset() << "\""
               << ",\"length\":" << chunk.chunklength()
               << ",\"nullOffset\":"
               << (chunk.has_isnulloffset()
                   ? chunk.isnulloffset() : chunk.chunklength())
               << ",\"littleEndian\":"
               << (chunk.has_littleendian() && chunk.littleendian()
                   ? "true" : "false")
               << ",\"nullsPadding\":"
               << (chunk.has_nullspadding() && chunk.nullspadding()
                   ? "true" : "false")
               << ",\"nullAlignment\":"
               << (chunk.has_isnullalignment()
                   ? chunk.isnullalignment() : 0)
               << ",\"pixels\":[";
        if (chunk.pixelpositions_size()
            != chunk.pixelstatistics_size())
        {
            return format::fail(
                    error_, format::ErrorCode::MALFORMED_PROTOBUF,
                    "row-group pixel positions and statistics differ");
        }
        for (int pixel = 0;
             pixel < chunk.pixelpositions_size(); ++pixel)
        {
            if (pixel != 0)
            {
                output << ",";
            }
            output << "{\"index\":" << pixel
                   << ",\"position\":"
                   << chunk.pixelpositions(pixel)
                   << ",\"statistics\":";
            const proto::PixelStatistic &pixelStatistic =
                    chunk.pixelstatistics(pixel);
            if (pixelStatistic.has_statistic())
            {
                appendStatisticJson(
                        output, pixelStatistic.statistic());
            }
            else
            {
                output << "null";
            }
            output << "}";
        }
        output << "]}}";
    }
    output << "]}";
    result_ = output.str();
    if (result_.size() > MAX_RESULT_OUTPUT_BYTES)
    {
        result_.clear();
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "row-group result exceeds the bounded output limit");
    }
    return true;
}

bool InspectionSession::buildPageResult(
        const std::vector<std::string> &values)
{
    std::uint64_t outputBytes = 128;
    for (const std::string &value : values)
    {
        if (!format::checkedAdd(
                    outputBytes, value.size() + 1U, outputBytes)
            || outputBytes > MAX_RESULT_OUTPUT_BYTES)
        {
            state_ = State::FAILED;
            return format::fail(
                    error_, format::ErrorCode::OUT_OF_BOUNDS,
                    "page result exceeds the bounded output limit");
        }
    }
    std::ostringstream output;
    output << "{\"rowGroup\":" << pageRequest_.rowGroup
           << ",\"column\":" << pageRequest_.column
           << ",\"offset\":" << pageRequest_.rowOffset
           << ",\"count\":" << pageRequest_.rowCount
           << ",\"values\":[";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            output << ',';
        }
        output << values[index];
    }
    output << "]}";
    result_ = output.str();
    if (result_.size() > MAX_RESULT_OUTPUT_BYTES)
    {
        result_.clear();
        state_ = State::FAILED;
        return format::fail(
                error_, format::ErrorCode::OUT_OF_BOUNDS,
                "page result exceeds the bounded output limit");
    }
    return true;
}

} // namespace inspector
} // namespace pixels
