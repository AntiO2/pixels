/*
 * Copyright 2023 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the Affero GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Pixels is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Affero GNU General Public License for more details.
 *
 * You should have received a copy of the Affero GNU General Public
 * License along with Pixels.  If not, see
 * <https://www.gnu.org/licenses/>.
 */

/*
 * @author liyu
 * @create 2023-03-06
 */
#include "PixelsReaderBuilder.h"
#include "format/PixelsFormatReader.h"

namespace
{

void throwFormatError(
        const pixels::proto::FileTail &fileTail,
        const pixels::format::FormatError &error)
{
    if (error.code == pixels::format::ErrorCode::UNSUPPORTED_VERSION
        && fileTail.has_postscript())
    {
        throw PixelsFileVersionInvalidException(
                fileTail.postscript().version());
    }
    if (error.code == pixels::format::ErrorCode::INVALID_MAGIC
        && fileTail.has_postscript())
    {
        throw PixelsFileMagicInvalidException(
                fileTail.postscript().magic());
    }
    throw InvalidArgumentException(error.message);
}

} // namespace

PixelsReaderBuilder::PixelsReaderBuilder()
{
    builderPath = "";
    builderPixelsFooterCache = nullptr;
}

PixelsReaderBuilder *PixelsReaderBuilder::setStorage(std::shared_ptr<Storage> storage)
{
    builderStorage = storage;
    return this;
}

PixelsReaderBuilder *PixelsReaderBuilder::setPath(const std::string &path)
{
    builderPath = path;
    return this;
}

PixelsReaderBuilder *PixelsReaderBuilder::setPixelsFooterCache(std::shared_ptr<PixelsFooterCache> pixelsFooterCache)
{
    builderPixelsFooterCache = pixelsFooterCache;
    return this;
}

std::shared_ptr<PixelsReader> PixelsReaderBuilder::build()
{
    if (builderStorage.get () == nullptr || builderPath.empty ())
    {
        throw std::runtime_error ("Missing argument to build PixelsReader");
    }
    // get PhysicalReader
    std::shared_ptr<PhysicalReader> fsReader =
            PhysicalReaderUtil::newPhysicalReader (builderStorage, builderPath);
    if (fsReader == nullptr)
    {
        throw PixelsReaderException (
                "Failed to create PixelsReader due to error of creating PhysicalReader");
    }

    const long fileLen = fsReader->getFileLength ();
    if (fileLen < 0)
    {
        throw InvalidArgumentException (
                "PixelsReaderBuilder::build: negative file length");
    }
    const auto portableFileLen = static_cast<std::uint64_t> (fileLen);

    // try to get file tail from cache
    std::string fileName = fsReader->getName ();
    std::shared_ptr<pixels::proto::FileTail> fileTail;
    if (builderPixelsFooterCache != nullptr && builderPixelsFooterCache->containsFileTail (fileName))
    {
        fileTail = builderPixelsFooterCache->getFileTail (fileName);
    } else
    {
        if (portableFileLen
            < pixels::format::PixelsFormatReader::TAIL_POINTER_SIZE)
        {
            throw InvalidArgumentException (
                    "PixelsReaderBuilder::build: file is shorter than the tail pointer");
        }

        fsReader->seek (
                fileLen
                - static_cast<long> (
                        pixels::format::PixelsFormatReader::TAIL_POINTER_SIZE));
        std::shared_ptr<ByteBuffer> tailPointerBuffer = fsReader->readFully (
                static_cast<int> (
                        pixels::format::PixelsFormatReader::TAIL_POINTER_SIZE));

        pixels::format::FileRange fileTailRange;
        pixels::format::FormatError formatError;
        if (!pixels::format::PixelsFormatReader::parseTailPointer (
                    portableFileLen,
                    pixels::format::ByteSpan (
                            tailPointerBuffer->getPointer (),
                            tailPointerBuffer->size ()),
                    fileTailRange, formatError))
        {
            throw InvalidArgumentException (formatError.message);
        }

        fsReader->seek (static_cast<long> (fileTailRange.offset));
        std::shared_ptr<ByteBuffer> fileTailBuffer = fsReader->readFully (
                static_cast<int> (fileTailRange.length));
        fileTail = std::make_shared<pixels::proto::FileTail> ();
        if (!pixels::format::PixelsFormatReader::parseFileTail (
                    portableFileLen, fileTailRange,
                    pixels::format::ByteSpan (
                            fileTailBuffer->getPointer (),
                            fileTailBuffer->size ()),
                    *fileTail, formatError))
        {
            throwFormatError (*fileTail, formatError);
        }
        if (builderPixelsFooterCache != nullptr)
        {
            builderPixelsFooterCache->putFileTail (fileName, fileTail);
        }
    }

    pixels::format::FormatError formatError;
    if (!pixels::format::PixelsFormatReader::validateFileTail (
                portableFileLen, *fileTail, formatError))
    {
        throwFormatError (*fileTail, formatError);
    }

    auto fileColTypes = std::vector<std::shared_ptr<pixels::proto::Type >>{};
    for (const auto &type: fileTail->footer ().types ())
    {
        fileColTypes.emplace_back (std::make_shared<pixels::proto::Type> (type));
    }
    builderSchema = TypeDescription::createSchema (fileColTypes);

    // TODO: the remaining things, such as builderSchema, coreCOnfig, metric

    return std::make_shared<PixelsReaderImpl> (builderSchema, fsReader, fileTail,
                                               builderPixelsFooterCache);
}

