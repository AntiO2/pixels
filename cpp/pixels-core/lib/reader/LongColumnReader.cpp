/*
 * Copyright 2025 PixelsDB.
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
 * @author whz
 * @create 2025-04-01
 */

#include "reader/LongColumnReader.h"
#include "format/PlainLongDecoder.h"

#include <cstdint>
#include <limits>
#include <vector>

LongColumnReader::LongColumnReader(std::shared_ptr<TypeDescription> type)
    : ColumnReader(type)
{
  // TODO: implement
}

void LongColumnReader::close()
{
  // TODO: implement
}

void LongColumnReader::read(std::shared_ptr<ByteBuffer> input,
                            pixels::proto::ColumnEncoding &encoding, int offset,
                            int size, int pixelStride, int vectorIndex,
                            std::shared_ptr<ColumnVector> vector,
                            pixels::proto::ColumnChunkIndex &chunkIndex,
                            std::shared_ptr<PixelsBitMask> filterMask)
{
  (void) filterMask;
  if (size < 0 || offset < 0 || vectorIndex < 0 || pixelStride <= 0)
  {
    throw InvalidArgumentException(
        "LongColumnReader::read: invalid range, vector index, or pixel stride");
  }
  if (size == 0)
  {
    return;
  }

  std::shared_ptr<LongColumnVector> columnVector =
      std::static_pointer_cast<LongColumnVector>(vector);
  if (columnVector == nullptr
      || static_cast<std::uint64_t>(vectorIndex)
         > columnVector->length
      || static_cast<std::uint64_t>(size)
         > columnVector->length
           - static_cast<std::uint64_t>(vectorIndex))
  {
    throw InvalidArgumentException(
        "LongColumnReader::read: destination vector is too small");
  }
  if (encoding.kind() != pixels::proto::ColumnEncoding_Kind_RUNLENGTH
      && encoding.kind() != pixels::proto::ColumnEncoding_Kind_NONE)
  {
    throw InvalidArgumentException(
        "LongColumnReader::read: unsupported LONG encoding");
  }

  // Make sure [offset, offset + size) is in the same pixels.
  assert(offset / pixelStride == (offset + size - 1) / pixelStride);

  // if read from start, init the stream and decoder
  if (offset == 0)
  {
    if (encoding.kind() == pixels::proto::ColumnEncoding_Kind_RUNLENGTH)
    {
      decoder = std::make_shared<RunLenIntDecoder>(input, true);
    }
    ColumnReader::elementIndex = 0;
    isNullOffset = chunkIndex.isnulloffset();
  }

  int pixelId = elementIndex / pixelStride;
  if (pixelId < 0 || pixelId >= chunkIndex.pixelstatistics_size()
      || !chunkIndex.pixelstatistics(pixelId).has_statistic())
  {
    throw InvalidArgumentException(
        "LongColumnReader::read: missing pixel statistics");
  }
  bool hasNull = chunkIndex.pixelstatistics(pixelId).statistic().hasnull();
  setValid(input, pixelStride, vector, pixelId, hasNull);

  if (encoding.kind() == pixels::proto::ColumnEncoding_Kind_RUNLENGTH)
  {
    for (int i = 0; i < size; i++)
    {
      columnVector->longVector[i + vectorIndex] = decoder->next();

      elementIndex++;
    }
  } else
  {
    if (static_cast<std::uint64_t>(size)
        > std::numeric_limits<std::uint32_t>::max() / sizeof(std::int64_t))
    {
      throw InvalidArgumentException(
          "LongColumnReader::read: plain LONG byte count overflows");
    }

    const std::uint32_t readPosition = input->getReadPos();
    pixels::format::ByteSpan remaining(
        input->getPointer() + readPosition, input->bytesRemaining());
    std::vector<std::int64_t> decoded(static_cast<std::size_t>(size));
    pixels::format::FormatError error;
    if (!pixels::format::PlainLongDecoder::decode(
            remaining, chunkIndex.has_littleendian()
                       && chunkIndex.littleendian(),
            0, decoded.size(), decoded.data(), decoded.size(), error))
    {
      throw InvalidArgumentException(
          "LongColumnReader::read: " + error.message);
    }

    for (int index = 0; index < size; ++index)
    {
      columnVector->longVector[index + vectorIndex] =
          static_cast<long>(decoded[static_cast<std::size_t>(index)]);
    }
    input->skipBytes(
        static_cast<std::uint32_t>(size * sizeof(std::int64_t)));
    elementIndex += size;
  }
}
