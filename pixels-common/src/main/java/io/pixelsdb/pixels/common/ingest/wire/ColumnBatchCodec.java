/*
 * Copyright 2026 PixelsDB.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * Affero GNU General Public License for more details.
 *
 * You should have received a copy of the Affero GNU General Public
 * License along with Pixels. If not, see <https://www.gnu.org/licenses/>.
 */
package io.pixelsdb.pixels.common.ingest.wire;

import java.io.*;
import java.util.*;

/** Bounded column-major scalar batches. A length of -1 encodes SQL NULL. */
public final class ColumnBatchCodec {
    public static final int FORMAT = 1;
    private static final int MAGIC = 0x50494231;

    private ColumnBatchCodec() {}

    public static byte[] encode(List<byte[][]> rows, int columns, int maxBytes) throws IOException {
        long size = 12L + 4L * columns * rows.size();
        for (byte[][] row : rows) {
            if (row.length != columns) throw new IOException("Column count mismatch");
            for (byte[] v : row) if (v != null) size += v.length;
        }
        if (columns <= 0 || rows.isEmpty() || size > maxBytes)
            throw new IOException("Invalid or oversized column batch");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream((int) size);
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeInt(MAGIC);
        out.writeInt(rows.size());
        out.writeInt(columns);
        for (int column = 0; column < columns; column++)
            for (byte[][] row : rows) {
                byte[] v = row[column];
                out.writeInt(v == null ? -1 : v.length);
                if (v != null) out.write(v);
            }
        out.flush();
        return bytes.toByteArray();
    }

    public static List<byte[][]> decode(
            byte[] payload, int expectedRows, int expectedColumns, int maxRows, int maxBytes)
            throws IOException {
        if (payload.length > maxBytes
                || expectedRows <= 0
                || expectedRows > maxRows
                || expectedColumns <= 0) throw new IOException("Batch limits exceeded");
        DataInputStream in = new DataInputStream(new ByteArrayInputStream(payload));
        if (in.readInt() != MAGIC
                || in.readInt() != expectedRows
                || in.readInt() != expectedColumns)
            throw new IOException("Batch metadata mismatch");
        if (4L * expectedRows * expectedColumns > in.available())
            throw new IOException("Truncated batch");
        List<byte[][]> rows = new ArrayList<>(expectedRows);
        for (int i = 0; i < expectedRows; i++) rows.add(new byte[expectedColumns][]);
        for (int column = 0; column < expectedColumns; column++)
            for (byte[][] row : rows) {
                int n = in.readInt();
                if (n < -1 || n > in.available()) throw new IOException("Invalid cell length");
                if (n >= 0) {
                    row[column] = new byte[n];
                    in.readFully(row[column]);
                }
            }
        if (in.available() != 0) throw new IOException("Trailing batch bytes");
        return rows;
    }
}
