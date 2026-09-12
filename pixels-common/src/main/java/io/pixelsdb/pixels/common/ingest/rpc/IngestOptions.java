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

import java.nio.file.Path;
import java.nio.file.Paths;

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
    public final long terminalRetentionMillis =
            longNumber("retina.ingest.terminal.retention.ms", 24L * 60 * 60 * 1000);
    public final int maxTerminalTransactions =
            number("retina.ingest.terminal.max.transactions", 100000);
    public final String coordinatorStateDirectory =
            requiredPath("retina.ingest.coordinator.state.dir");
    public final String participantPlanDirectory =
            requiredPath("retina.ingest.participant.plan.dir");
    public final String participantWalDirectory =
            requiredPath("retina.ingest.participant.wal.dir");
    public final int walSegmentBytes = number("retina.ingest.wal.segment.bytes", 64 * 1024 * 1024);
    public final long walMaxBytes =
            longNumber("retina.ingest.wal.max.bytes", 4L * 1024 * 1024 * 1024);
    public final int walMaxRecords =
            number("retina.ingest.wal.max.records", 10_000_000);
    public final long cutoverBaselineTimestamp =
            nonNegative("retina.ingest.cutover.baseline.timestamp", 0L);

    public IngestOptions() {
        if (terminalRetentionMillis < transactionLeaseMillis) {
            throw new IllegalArgumentException(
                    "retina.ingest.terminal.retention.ms must not be shorter than the transaction lease");
        }
        if (enabled) {
            validateStateDirectories();
        }
    }

    public static String property(String key, String fallback) {
        String v = ConfigFactory.Instance().getProperty(key);
        return v == null ? fallback : v;
    }

    private static int number(String key, int fallback) {
        int n = Integer.parseInt(property(key, Integer.toString(fallback)));
        if (n <= 0) throw new IllegalArgumentException(key + " must be positive");
        return n;
    }

    private static long longNumber(String key, long fallback) {
        long n = Long.parseLong(property(key, Long.toString(fallback)));
        if (n <= 0) throw new IllegalArgumentException(key + " must be positive");
        return n;
    }

    private static long nonNegative(String key, long fallback) {
        long n = Long.parseLong(property(key, Long.toString(fallback)));
        if (n < 0 || n >= (1L << 48)) {
            throw new IllegalArgumentException(
                    key + " must fit Retina's non-negative 48-bit timestamp domain");
        }
        return n;
    }

    private String requiredPath(String key) {
        String value = property(key, "").trim();
        if (enabled && value.isEmpty()) {
            throw new IllegalArgumentException(
                    key + " is required when transactional ingestion is enabled");
        }
        return value;
    }

    private void validateStateDirectories() {
        Path coordinator = absoluteStatePath(
                "retina.ingest.coordinator.state.dir", coordinatorStateDirectory);
        Path plans = absoluteStatePath(
                "retina.ingest.participant.plan.dir", participantPlanDirectory);
        Path wal = absoluteStatePath(
                "retina.ingest.participant.wal.dir", participantWalDirectory);
        requireDisjoint(coordinator, plans);
        requireDisjoint(coordinator, wal);
        requireDisjoint(plans, wal);
    }

    private static Path absoluteStatePath(String key, String value) {
        Path path = Paths.get(value);
        if (!path.isAbsolute()) {
            throw new IllegalArgumentException(key + " must be an absolute path");
        }
        return path.normalize();
    }

    private static void requireDisjoint(Path first, Path second) {
        if (first.equals(second) || first.startsWith(second) || second.startsWith(first)) {
            throw new IllegalArgumentException(
                    "transactional ingestion state directories must not overlap: "
                            + first + " and " + second);
        }
    }
}
