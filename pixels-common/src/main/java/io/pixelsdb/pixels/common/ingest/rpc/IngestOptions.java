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
package io.pixelsdb.pixels.common.ingest.rpc;

import io.pixelsdb.pixels.common.utils.ConfigFactory;

/** Bounded admission settings shared by the connector and ingestion endpoints. */
public final class IngestOptions {
    public final boolean enabled = Boolean.parseBoolean(property("retina.ingest.enabled", "false"));
    public final int maxBatchRows = number("retina.ingest.max.batch.rows", 4096);
    public final int maxBatchBytes = number("retina.ingest.max.batch.bytes", 4 * 1024 * 1024);
    public final int maxStreams = number("retina.ingest.max.streams", 4096);
    public final int maxStateBytes = number("retina.ingest.max.state.bytes", 64 * 1024 * 1024);
    public final int maxPreparedRows = number("retina.ingest.max.prepared.rows", 1000000);
    public final long readLeaseMillis = number("retina.ingest.read.lease.ms", 120000);
    public final long transactionLeaseMillis = number("retina.ingest.transaction.lease.ms", 300000);

    public static String property(String key, String fallback) {
        String v = ConfigFactory.Instance().getProperty(key);
        return v == null ? fallback : v;
    }

    private static int number(String key, int fallback) {
        int n = Integer.parseInt(property(key, Integer.toString(fallback)));
        if (n <= 0) throw new IllegalArgumentException(key + " must be positive");
        return n;
    }
}
