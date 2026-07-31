#include "pixels_inspector.h"
#include "pixels.pb.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::uint32_t kRowGroups = 3;
constexpr std::uint32_t kRowsPerGroup = 25;
constexpr std::uint32_t kPixelStride = 5;
constexpr std::uint32_t kColumns = 8;
constexpr std::uint32_t kChunkAlignment = 32;
constexpr std::uint32_t kNullAlignment = 8;

struct Customer
{
    std::int64_t key;
    std::string name;
    std::string address;
    std::int64_t nationKey;
    std::string phone;
    std::int64_t accountBalance;
    std::string segment;
    std::string comment;
};

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::size_t countOccurrences(const std::string &input,
                             std::string_view needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = input.find(needle, position)) != std::string::npos)
    {
        ++count;
        position += needle.size();
    }
    return count;
}

void appendLittleEndian32(std::vector<std::uint8_t> &output,
                          std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendLittleEndian64(std::vector<std::uint8_t> &output,
                          std::int64_t value)
{
    const auto bits = static_cast<std::uint64_t>(value);
    for (unsigned shift = 0; shift < 64; shift += 8)
    {
        output.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
}

void appendBigEndian64(std::vector<std::uint8_t> &output,
                       std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void align(std::vector<std::uint8_t> &output, std::uint32_t alignment)
{
    while (output.size() % alignment != 0)
    {
        output.push_back(0);
    }
}

Customer customer(std::uint32_t row)
{
    static constexpr std::array<std::string_view, 5> segments = {
        "AUTOMOBILE", "BUILDING", "FURNITURE", "HOUSEHOLD", "MACHINERY"};
    static constexpr std::array<std::string_view, 6> streets = {
        "Cedar Lane", "Harbor Road", "Maple Street", "Orchard Way",
        "Pine Avenue", "Willow Court"};
    const auto key = static_cast<std::int64_t>(1001 + row);
    const std::uint32_t nation = (row * 7U + 3U) % 25U;
    const std::int64_t balance = (static_cast<std::int64_t>(row) - 18) * 13725;
    return {
        key,
        "Customer#" + std::to_string(key),
        std::to_string(10 + row) + " " + std::string(streets[row % streets.size()]) +
            ", Suite " + std::to_string(100 + row),
        nation,
        std::to_string(10 + nation) + "-" + std::to_string(100 + row) + "-" +
            std::to_string(200 + row) + "-" + std::to_string(3000 + row),
        balance,
        std::string(segments[row % segments.size()]),
        "Customer " + std::to_string(key) +
            " prefers verified local analytics and prompt account reviews."};
}

void addType(pixels::proto::Footer &footer, pixels::proto::Type::Kind kind,
             const std::string &name, std::uint32_t maximumLength = 0,
             std::uint32_t precision = 0, std::uint32_t scale = 0)
{
    auto *type = footer.add_types();
    type->set_kind(kind);
    type->set_name(name);
    if (maximumLength != 0)
    {
        type->set_maximumlength(maximumLength);
    }
    if (precision != 0)
    {
        type->set_precision(precision);
        type->set_scale(scale);
    }
}

pixels::proto::ColumnStatistic statistic(std::uint64_t count)
{
    pixels::proto::ColumnStatistic result;
    result.set_numberofvalues(count);
    result.set_hasnull(false);
    return result;
}

std::vector<std::uint8_t> fixedChunk(
    const std::vector<std::int64_t> &values,
    pixels::proto::ColumnChunkIndex &index)
{
    std::vector<std::uint8_t> output;
    for (std::size_t start = 0; start < values.size(); start += kPixelStride)
    {
        index.add_pixelpositions(static_cast<std::uint32_t>(output.size()));
        auto *pixel = index.add_pixelstatistics()->mutable_statistic();
        pixel->CopyFrom(statistic(std::min<std::size_t>(kPixelStride,
                                                        values.size() - start)));
        auto *integer = pixel->mutable_intstatistics();
        const auto end = std::min(values.size(), start + kPixelStride);
        const auto bounds = std::minmax_element(values.begin() + start,
                                                values.begin() + end);
        integer->set_minimum(*bounds.first);
        integer->set_maximum(*bounds.second);
        std::int64_t sum = 0;
        for (auto position = start; position < end; ++position)
        {
            appendLittleEndian64(output, values[position]);
            sum += values[position];
        }
        integer->set_sum(sum);
    }
    align(output, kNullAlignment);
    index.set_isnulloffset(static_cast<std::uint32_t>(output.size()));
    return output;
}

std::vector<std::uint8_t> stringChunk(
    const std::vector<std::string> &values,
    pixels::proto::ColumnChunkIndex &index)
{
    std::vector<std::uint8_t> output;
    std::vector<std::uint32_t> starts;
    starts.reserve(values.size() + 1);
    for (std::size_t start = 0; start < values.size(); start += kPixelStride)
    {
        index.add_pixelpositions(static_cast<std::uint32_t>(output.size()));
        auto *pixel = index.add_pixelstatistics()->mutable_statistic();
        pixel->CopyFrom(statistic(std::min<std::size_t>(kPixelStride,
                                                        values.size() - start)));
        auto *strings = pixel->mutable_stringstatistics();
        const auto end = std::min(values.size(), start + kPixelStride);
        auto minimum = values[start];
        auto maximum = values[start];
        std::int64_t totalLength = 0;
        for (auto position = start; position < end; ++position)
        {
            starts.push_back(static_cast<std::uint32_t>(output.size()));
            output.insert(output.end(), values[position].begin(), values[position].end());
            minimum = std::min(minimum, values[position]);
            maximum = std::max(maximum, values[position]);
            totalLength += static_cast<std::int64_t>(values[position].size());
        }
        strings->set_minimum(minimum);
        strings->set_maximum(maximum);
        strings->set_sum(totalLength);
    }
    starts.push_back(static_cast<std::uint32_t>(output.size()));
    align(output, kNullAlignment);
    index.set_isnulloffset(static_cast<std::uint32_t>(output.size()));
    const auto startsOffset = static_cast<std::uint32_t>(output.size());
    for (const auto start : starts)
    {
        appendLittleEndian32(output, start);
    }
    appendLittleEndian32(output, startsOffset);
    return output;
}

std::vector<std::uint8_t> buildFixture()
{
    pixels::proto::Footer footer;
    addType(footer, pixels::proto::Type::LONG, "c_custkey");
    addType(footer, pixels::proto::Type::VARCHAR, "c_name", 25);
    addType(footer, pixels::proto::Type::VARCHAR, "c_address", 40);
    addType(footer, pixels::proto::Type::LONG, "c_nationkey");
    addType(footer, pixels::proto::Type::CHAR, "c_phone", 15);
    addType(footer, pixels::proto::Type::DECIMAL, "c_acctbal", 0, 15, 2);
    addType(footer, pixels::proto::Type::CHAR, "c_mktsegment", 10);
    addType(footer, pixels::proto::Type::VARCHAR, "c_comment", 117);
    for (std::uint32_t column = 0; column < kColumns; ++column)
    {
        footer.add_columnstats()->CopyFrom(
            statistic(kRowGroups * kRowsPerGroup));
    }

    std::vector<std::uint8_t> file;
    for (std::uint32_t group = 0; group < kRowGroups; ++group)
    {
        std::array<std::vector<std::int64_t>, 3> fixed;
        std::array<std::vector<std::string>, 5> strings;
        for (std::uint32_t local = 0; local < kRowsPerGroup; ++local)
        {
            const Customer row = customer(group * kRowsPerGroup + local);
            fixed[0].push_back(row.key);
            fixed[1].push_back(row.nationKey);
            fixed[2].push_back(row.accountBalance);
            strings[0].push_back(row.name);
            strings[1].push_back(row.address);
            strings[2].push_back(row.phone);
            strings[3].push_back(row.segment);
            strings[4].push_back(row.comment);
        }

        pixels::proto::RowGroupFooter groupFooter;
        std::array<std::vector<std::uint8_t>, kColumns> chunks;
        chunks[0] = fixedChunk(fixed[0], *groupFooter.mutable_rowgroupindexentry()->add_columnchunkindexentries());
        chunks[1] = stringChunk(strings[0], *groupFooter.mutable_rowgroupindexentry()->add_columnchunkindexentries());
        chunks[2] = stringChunk(strings[1], *groupFooter.mutable_rowgroupindexentry()->add_columnchunkindexentries());
        chunks[3] = fixedChunk(fixed[1], *groupFooter.mutable_rowgroupindexentry()->add_columnchunkindexentries());
        chunks[4] = stringChunk(strings[2], *groupFooter.mutable_rowgroupindexentry()->add_columnchunkindexentries());
        chunks[5] = fixedChunk(fixed[2], *groupFooter.mutable_rowgroupindexentry()->add_columnchunkindexentries());
        chunks[6] = stringChunk(strings[3], *groupFooter.mutable_rowgroupindexentry()->add_columnchunkindexentries());
        chunks[7] = stringChunk(strings[4], *groupFooter.mutable_rowgroupindexentry()->add_columnchunkindexentries());

        const auto groupStart = static_cast<std::uint64_t>(file.size());
        for (std::uint32_t column = 0; column < kColumns; ++column)
        {
            align(file, kChunkAlignment);
            auto *index = groupFooter.mutable_rowgroupindexentry()->mutable_columnchunkindexentries(column);
            index->set_chunkoffset(file.size());
            index->set_chunklength(static_cast<std::uint32_t>(chunks[column].size()));
            index->set_littleendian(true);
            index->set_nullspadding(false);
            index->set_isnullalignment(kNullAlignment);
            file.insert(file.end(), chunks[column].begin(), chunks[column].end());
            auto *encoding = groupFooter.mutable_rowgroupencoding()->add_columnchunkencodings();
            encoding->set_kind(pixels::proto::ColumnEncoding::NONE);
        }
        const auto footerOffset = static_cast<std::uint64_t>(file.size());
        const std::string serialized = groupFooter.SerializeAsString();
        file.insert(file.end(), serialized.begin(), serialized.end());
        auto *information = footer.add_rowgroupinfos();
        information->set_footeroffset(footerOffset);
        information->set_datalength(static_cast<std::uint32_t>(footerOffset - groupStart));
        information->set_footerlength(static_cast<std::uint32_t>(serialized.size()));
        information->set_numberofrows(kRowsPerGroup);
        auto *groupStatistics = footer.add_rowgroupstats();
        for (std::uint32_t column = 0; column < kColumns; ++column)
        {
            groupStatistics->add_columnchunkstats()->CopyFrom(statistic(kRowsPerGroup));
        }
    }

    pixels::proto::PostScript postScript;
    postScript.set_version(1);
    postScript.set_contentlength(file.size());
    postScript.set_numberofrows(kRowGroups * kRowsPerGroup);
    postScript.set_compression(pixels::proto::CompressionKind::NONE);
    postScript.set_compressionblocksize(1);
    postScript.set_pixelstride(kPixelStride);
    postScript.set_writertimezone("UTC");
    postScript.set_partitioned(false);
    postScript.set_columnchunkalignment(kChunkAlignment);
    postScript.set_hashiddencolumn(false);
    postScript.set_magic("PIXELS");

    pixels::proto::FileTail tail;
    tail.mutable_footer()->CopyFrom(footer);
    tail.mutable_postscript()->CopyFrom(postScript);
    tail.set_footerlength(footer.ByteSizeLong());
    tail.set_postscriptlength(postScript.ByteSizeLong());
    const auto tailOffset = static_cast<std::uint64_t>(file.size());
    const std::string serialized = tail.SerializeAsString();
    file.insert(file.end(), serialized.begin(), serialized.end());
    appendBigEndian64(file, tailOffset);
    return file;
}

std::string copyResult(pixels_inspector_handle handle)
{
    std::uint64_t size = 0;
    require(pixels_inspector_result_size(handle, &size) == PIXELS_INSPECTOR_OK,
            "unable to size inspector result");
    require(size <= std::numeric_limits<std::size_t>::max(),
            "inspector result is too large");
    std::string output(static_cast<std::size_t>(size), '\0');
    require(pixels_inspector_copy_result(
                handle, reinterpret_cast<std::uint8_t *>(output.data()), size) ==
                PIXELS_INSPECTOR_OK,
            "unable to copy inspector result");
    return output;
}

void driveRanges(pixels_inspector_handle handle,
                 const std::vector<std::uint8_t> &file,
                 pixels_inspector_status status,
                 const std::string &operation)
{
    require(status == PIXELS_INSPECTOR_RANGE_READY,
            operation + " did not request an initial range");
    while (status == PIXELS_INSPECTOR_RANGE_READY)
    {
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        require(pixels_inspector_next_range(handle, &offset, &length) ==
                    PIXELS_INSPECTOR_RANGE_READY &&
                    offset <= file.size() && length <= file.size() - offset,
                operation + " requested an invalid range");
        status = pixels_inspector_supply_range(
            handle, offset, length, file.data() + static_cast<std::size_t>(offset));
    }
    require(status == PIXELS_INSPECTOR_RESULT_READY,
            operation + " rejected generated fixture");
}

std::string inspect(const std::vector<std::uint8_t> &file,
                    pixels_inspector_status (*begin)(pixels_inspector_handle))
{
    pixels_inspector_handle handle = 0;
    require(pixels_inspector_create(file.size(), &handle) == PIXELS_INSPECTOR_OK,
            "unable to create inspector session");
    driveRanges(handle, file, begin(handle), "metadata");
    const auto result = copyResult(handle);
    require(pixels_inspector_destroy(handle) == PIXELS_INSPECTOR_OK,
            "unable to destroy inspector session");
    return result;
}

pixels_inspector_status beginMetadata(pixels_inspector_handle handle)
{
    return pixels_inspector_begin_metadata(handle);
}

std::string inspectRowGroup(const std::vector<std::uint8_t> &file,
                            std::uint32_t rowGroup)
{
    pixels_inspector_handle handle = 0;
    require(pixels_inspector_create(file.size(), &handle) == PIXELS_INSPECTOR_OK,
            "unable to create layout session");
    driveRanges(handle, file, pixels_inspector_begin_metadata(handle), "metadata");
    driveRanges(handle, file, pixels_inspector_begin_row_group(handle, rowGroup),
                "layout");
    const auto result = copyResult(handle);
    pixels_inspector_destroy(handle);
    return result;
}

std::string inspectPage(const std::vector<std::uint8_t> &file,
                        std::uint32_t column)
{
    pixels_inspector_handle handle = 0;
    require(pixels_inspector_create(file.size(), &handle) == PIXELS_INSPECTOR_OK,
            "unable to create page session");
    driveRanges(handle, file, pixels_inspector_begin_metadata(handle), "metadata");
    driveRanges(handle, file, pixels_inspector_begin_page(handle, 0, column, 0, 25),
                "page");
    const auto result = copyResult(handle);
    pixels_inspector_destroy(handle);
    return result;
}

void validate(const std::vector<std::uint8_t> &file)
{
    const auto metadata = inspect(file, beginMetadata);
    require(metadata.find("\"rows\":75") != std::string::npos &&
                metadata.find("\"pixelStride\":5") != std::string::npos &&
                metadata.find("\"schemaCount\":8") != std::string::npos &&
                metadata.find("\"rowGroupCount\":3") != std::string::npos,
            "generated metadata shape is incorrect");
    const std::array<std::string_view, kColumns> names = {
        "c_custkey", "c_name", "c_address", "c_nationkey", "c_phone",
        "c_acctbal", "c_mktsegment", "c_comment"};
    for (const auto name : names)
    {
        require(metadata.find(name) != std::string::npos,
                "generated schema is missing " + std::string(name));
    }
    for (std::uint32_t group = 0; group < kRowGroups; ++group)
    {
        const auto layout = inspectRowGroup(file, group);
        require(countOccurrences(layout, "\"column\":") == kColumns,
                "generated layout does not expose eight column chunks");
        require(countOccurrences(layout, "\"index\":") ==
                    kColumns * (kRowsPerGroup / kPixelStride),
                "generated chunks do not expose five pixels each");
    }
    require(inspectPage(file, 0).find("\"1001\"") != std::string::npos,
            "LONG representative value did not round-trip");
    require(inspectPage(file, 1).find("Customer#1001") != std::string::npos,
            "VARCHAR representative value did not round-trip");
    require(inspectPage(file, 5).find("\"-2470.50\"") != std::string::npos,
            "DECIMAL representative value did not round-trip");
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        require(argc == 2, "usage: pixels-customer-fixture-generator OUTPUT.pxl");
        const auto file = buildFixture();
        validate(file);
        std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
        require(output.good(), "unable to open fixture output");
        output.write(reinterpret_cast<const char *>(file.data()),
                     static_cast<std::streamsize>(file.size()));
        require(output.good(), "unable to write fixture output");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
