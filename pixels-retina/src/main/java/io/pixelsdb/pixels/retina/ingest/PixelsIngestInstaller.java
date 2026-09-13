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
package io.pixelsdb.pixels.retina.ingest;

import com.google.protobuf.ByteString;

import io.pixelsdb.pixels.common.index.IndexOption;
import io.pixelsdb.pixels.common.index.service.*;
import io.pixelsdb.pixels.common.ingest.MutationBatch;
import io.pixelsdb.pixels.common.ingest.durable.AtomicStateFile;
import io.pixelsdb.pixels.common.ingest.rpc.IngestOptions;
import io.pixelsdb.pixels.common.ingest.wire.*;
import io.pixelsdb.pixels.common.metadata.MetadataService;
import io.pixelsdb.pixels.common.metadata.domain.File;
import io.pixelsdb.pixels.common.utils.IndexUtils;
import io.pixelsdb.pixels.common.utils.RetinaUtils;
import io.pixelsdb.pixels.core.ingest.*;
import io.pixelsdb.pixels.index.IndexProto;
import io.pixelsdb.pixels.ingest.IngestProto.*;
import io.pixelsdb.pixels.retina.*;

import java.io.IOException;
import java.util.*;

/**
 * Installs into existing PixelsWriteBuffer and IndexService. The local plan file
 * records allocator results and replay placements; it is not a row-location index.
 * LOCAL v1 retains WAL/plans and disables rewriting GC until checkpoint compaction
 * of the ingestion log is implemented.
 */
public final class PixelsIngestInstaller implements RetinaIngestParticipant.Installer {
    private final AtomicStateFile state;
    private final IngestOptions options;
    private final RetinaResourceManager resources;
    private final IndexService indexes;
    private final MetadataService metadata;
    private final String owner;
    private final Map<String, BatchInstall> plans = new LinkedHashMap<>();
    private final Map<Long, TransactionCheckpoint> checkpoints = new LinkedHashMap<>();
    private final Map<String, Long> keyIntents = new HashMap<>();
    private final Map<Long, Set<String>> transactionKeys = new HashMap<>();
    private final Map<Long, Long> reservedBytes = new HashMap<>();
    private final Map<Long, File> catalogFiles = new HashMap<>();
    private final Set<Long> preparedTransactions = new HashSet<>();
    private long planBytes;
    private final Map<Long, Long> intentBytes = new HashMap<>();

    public PixelsIngestInstaller(
            AtomicStateFile state,
            IngestOptions options,
            String owner,
            RetinaResourceManager resources,
            IndexService indexes,
            MetadataService metadata)
            throws Exception {
        this.state = state;
        this.options = options;
        this.owner = owner;
        this.resources = resources;
        this.indexes = indexes;
        this.metadata = metadata;
        byte[] bytes = state.read();
        if (bytes.length > 0) {
            InstallationSnapshot snapshot = InstallationSnapshot.parseFrom(bytes);
            if (snapshot.getVersion() != 2) {
                throw new IOException("Unknown installation plan version");
            }
            for (BatchInstall plan : snapshot.getBatchesList()) {
                String key = key(plan);
                if (plans.put(key, plan) != null) {
                    throw new IOException("Duplicate installation plan");
                }
                long rows = 0;
                for (BufferSpan span : plan.getSpansList()) {
                    if (span.getRowCount() <= 0
                            || span.getRowIdStart() != plan.getRowIdStart() + rows) {
                        throw new IOException("Invalid recorded rowId span");
                    }
                    rows += span.getRowCount();
                }
                if (rows > plan.getRowCount()) {
                    throw new IOException("Plan exceeds its batch");
                }
            }
            for (TransactionCheckpoint checkpoint : snapshot.getCheckpointsList()) {
                if (checkpoint.getTransactionId() <= 0
                        || checkpoint.getCommitTimestamp() <= 0
                        || checkpoint.getTableId() <= 0
                        || checkpoint.getTableFingerprint().isEmpty()
                        || checkpoints.put(checkpoint.getTransactionId(), checkpoint) != null) {
                    throw new IOException("Invalid or duplicate installation checkpoint");
                }
                Set<Long> fileIds = new HashSet<>(checkpoint.getFileIdsList());
                if (fileIds.size() != checkpoint.getFileIdsCount() || fileIds.contains(0L)
                        || checkpoint.getRowRangesCount() == 0) {
                    throw new IOException("Invalid installation checkpoint coverage");
                }
                for (RowIdRange range : checkpoint.getRowRangesList()) {
                    if (range.getRowIdStart() < 0 || range.getRowCount() <= 0) {
                        throw new IOException("Invalid installation checkpoint row range");
                    }
                }
            }
            planBytes = bytes.length;
        }
    }

    private static String key(BatchInstall plan) {
        return IngestWire.batchKey(IngestWire.decode(plan.getStream()), plan.getSequence());
    }

    private void save(BatchInstall value) throws IOException {
        Map<String, BatchInstall> next = new LinkedHashMap<>(plans);
        next.put(key(value), value);
        byte[] bytes = snapshot(next, checkpoints);
        state.store(bytes);
        plans.clear();
        plans.putAll(next);
        planBytes = bytes.length;
    }

    private static byte[] snapshot(
            Map<String, BatchInstall> plans,
            Map<Long, TransactionCheckpoint> checkpoints) {
        return InstallationSnapshot.newBuilder()
                .setVersion(2)
                .addAllBatches(plans.values())
                .addAllCheckpoints(checkpoints.values())
                .build()
                .toByteArray();
    }

    private List<byte[][]> rows(Transaction tx, MutationBatch batch) throws IOException {
        if (batch.getSchemaVersion() != tx.getTable().getSchemaVersion()
                || batch.getPayloadFormat() != ColumnBatchCodec.FORMAT) {
            throw new IOException("Schema or codec mismatch");
        }
        List<byte[][]> result =
                ColumnBatchCodec.decode(
                        batch.getPayload(),
                        batch.getRowCount(),
                        tx.getTable().getColumnsCount(),
                        options.maxBatchRows,
                        options.maxBatchBytes);
        for (byte[][] row : result) {
            IngestRows.validate(tx.getTable(), row);
        }
        return result;
    }

    @Override
    public synchronized void prepare(Transaction tx, Iterable<MutationBatch> batches)
            throws Exception {
        if (preparedTransactions.contains(tx.getTransactionId())) {
            return;
        }
        IngestTables.validate(tx.getTable());
        long totalRows = 0, reservation = 4096;
        for (MutationBatch batch : batches) {
            totalRows = Math.addExact(totalRows, batch.getRowCount());
            reservation =
                    Math.addExact(
                            reservation, 2048L + 512L * ((batch.getRowCount() + 63L) / 64 + 2));
        }
        if (totalRows > options.maxPreparedRows) {
            throw new IOException("Prepared row limit exceeded");
        }
        long allReserved = reservedBytes.values().stream().mapToLong(Long::longValue).sum();
        if (planBytes + allReserved + reservation > options.maxStateBytes) {
            throw new IOException("Installation plan capacity exhausted");
        }
        Set<String> keys = new HashSet<>();
        long keyBytes = intentBytes.values().stream().mapToLong(Long::longValue).sum();
        TableIndex primary = IngestRows.primary(tx.getTable());
        for (MutationBatch batch : batches) {
            for (byte[][] row : rows(tx, batch)) {
                if (primary == null) {
                    continue;
                }
                ByteString encoded = IngestRows.indexKey(primary, row);
                int bucket = RetinaUtils.getBucketIdFromByteBuffer(encoded);
                if (bucket != batch.getStreamId().getShardId()) {
                    throw new IOException("Primary-key routing mismatch");
                }
                keyBytes = Math.addExact(keyBytes, 256L + 2L * encoded.size());
                if (keyBytes > options.maxStateBytes) {
                    throw new IOException("Prepared key-intent memory limit exceeded");
                }
                String identity =
                        tx.getTable().getTableId()
                                + ":"
                                + primary.getId()
                                + ":"
                                + Base64.getEncoder().encodeToString(encoded.toByteArray());
                if (!keys.add(identity)) {
                    throw new IOException("Duplicate primary key in INSERT");
                }
                Long holder = keyIntents.get(identity);
                if (holder != null && holder != tx.getTransactionId()) {
                    throw new IOException("Primary-key write conflict");
                }
                IndexProto.IndexKey indexKey =
                        IndexProto.IndexKey.newBuilder()
                                .setTableId(tx.getTable().getTableId())
                                .setIndexId(primary.getId())
                                .setKey(encoded)
                                .setTimestamp(Long.MAX_VALUE)
                                .build();
                IndexOption indexOption =
                        IndexOption.builder()
                                .vNodeId(IndexUtils.getBucketIdFromByteBuffer(encoded))
                                .build();
                if (indexes.lookupUniqueIndex(indexKey, indexOption) != null) {
                    throw new IOException("Primary key already exists");
                }
            }
        }
        // Mutation of the intent table is all-or-nothing after all validation succeeds.
        for (String identity : keys) {
            keyIntents.put(identity, tx.getTransactionId());
        }
        long existingKeyBytes = intentBytes.values().stream().mapToLong(Long::longValue).sum();
        intentBytes.put(tx.getTransactionId(), keyBytes - existingKeyBytes);
        transactionKeys.put(tx.getTransactionId(), keys);
        reservedBytes.put(tx.getTransactionId(), reservation);
        preparedTransactions.add(tx.getTransactionId());
    }

    @Override
    public synchronized void release(long txId) {
        Set<String> keys = transactionKeys.remove(txId);
        if (keys != null) {
            for (String key : keys) {
                keyIntents.remove(key, txId);
            }
        }
        reservedBytes.remove(txId);
        intentBytes.remove(txId);
        preparedTransactions.remove(txId);
    }

    @Override
    public synchronized void initializeRecovery(List<Transaction> transactions) throws Exception {
        Set<Long> committed = new HashSet<>();
        Map<Long, Long> timestamps = new HashMap<>();
        Map<Long, Transaction> authoritative = new HashMap<>();
        for (Transaction tx : transactions) {
            if (IngestWire.committed(tx)) {
                committed.add(tx.getTransactionId());
                timestamps.put(tx.getTransactionId(), tx.getCommitTimestamp());
                authoritative.put(tx.getTransactionId(), tx);
            }
        }
        Set<Long> retiredCheckpoints = new HashSet<>();
        for (TransactionCheckpoint checkpoint : checkpoints.values()) {
            Transaction tx = authoritative.get(checkpoint.getTransactionId());
            if (tx != null && (tx.getState() != TransactionState.PUBLISHED
                    || tx.getCommitTimestamp() != checkpoint.getCommitTimestamp()
                    || tx.getTable().getTableId() != checkpoint.getTableId()
                    || !tx.getTable().getFingerprint().equals(checkpoint.getTableFingerprint()))) {
                throw new IOException("Installation checkpoint lacks its authoritative PUBLISHED decision");
            }
            if (tx == null) {
                // The coordinator removes a full PUBLISHED decision only after this participant
                // acknowledged the checkpoint. Its absence is therefore the durable prune ack.
                // Do not re-open checkpointed file identities here: a later storage-GC
                // checkpoint may already have retired them and moved surviving stable rowIds.
                retiredCheckpoints.add(checkpoint.getTransactionId());
            } else {
                verifyCheckpointRows(checkpoint);
            }
        }
        if (!retiredCheckpoints.isEmpty()) {
            Map<Long, TransactionCheckpoint> retained = new LinkedHashMap<>(checkpoints);
            retiredCheckpoints.forEach(retained::remove);
            byte[] compacted = snapshot(plans, retained);
            state.store(compacted);
            checkpoints.clear();
            checkpoints.putAll(retained);
            planBytes = compacted.length;
        }
        Set<Long> managedFiles = new HashSet<>();
        for (BatchInstall plan : plans.values()) {
            if (!committed.contains(plan.getStream().getTransactionId())
                    || timestamps.get(plan.getStream().getTransactionId())
                            != plan.getCommitTimestamp()) {
                throw new IOException("Installation plan lacks its authoritative COMMIT decision");
            }
            for (BufferSpan span : plan.getSpansList()) {
                managedFiles.add(span.getFileId());
                File file = metadata.getFileById(span.getFileId());
                if (file == null
                        || file.getPathId() != span.getPathId()
                        || !file.getName().equals(span.getFileName())) {
                    throw new IOException(
                            "Acknowledged installation file identity is missing or changed");
                }
                if (file.getType() != File.Type.REGULAR
                        && file.getType() != File.Type.TEMPORARY_INGEST) {
                    throw new IOException(
                            "Ingestion replay does not support rewritten file identities");
                }
                catalogFiles.put(file.getId(), file);
                resources.addVisibility(file.getId(), 0, span.getFileCapacity(), 0L, null, false);
            }
        }
        resources.initializeIngestBaseline(managedFiles);
    }

    @Override
    public synchronized boolean recoveredByCheckpoint(Transaction tx) throws Exception {
        TransactionCheckpoint checkpoint = checkpoints.get(tx.getTransactionId());
        if (checkpoint == null) {
            return false;
        }
        if (tx.getState() != TransactionState.PUBLISHED
                || checkpoint.getCommitTimestamp() != tx.getCommitTimestamp()
                || checkpoint.getTableId() != tx.getTable().getTableId()
                || !checkpoint.getTableFingerprint().equals(tx.getTable().getFingerprint())) {
            throw new IOException("Recovered transaction differs from installation checkpoint");
        }
        return true;
    }

    @Override
    public synchronized boolean checkpoint(
            Transaction tx, Iterable<MutationBatch> batches) throws Exception {
        if (checkpoints.containsKey(tx.getTransactionId())) {
            return true;
        }
        if (tx.getState() != TransactionState.PUBLISHED) {
            return false;
        }
        List<BatchInstall> transactionPlans = new ArrayList<>();
        Set<Long> coveredFiles = new HashSet<>();
        for (MutationBatch batch : batches) {
            BatchInstall plan = plans.get(
                    IngestWire.batchKey(batch.getStreamId(), batch.getSequence()));
            if (plan == null || plan.getStream().getTransactionId() != tx.getTransactionId()) {
                throw new IOException("Published transaction is missing its installation plan");
            }
            if (!verifyMaterializedBatch(tx, batch, plan, coveredFiles)) {
                return false;
            }
            transactionPlans.add(plan);
        }
        if (!resources.isIngestRecoveryCheckpointDurable(
                tx.getCommitTimestamp(), coveredFiles)) {
            return false;
        }
        TransactionCheckpoint.Builder checkpoint = TransactionCheckpoint.newBuilder()
                .setTransactionId(tx.getTransactionId())
                .setCommitTimestamp(tx.getCommitTimestamp())
                .setTableId(tx.getTable().getTableId())
                .setTableFingerprint(tx.getTable().getFingerprint())
                .addAllFileIds(new TreeSet<>(coveredFiles));
        for (BatchInstall plan : transactionPlans) {
            checkpoint.addRowRanges(RowIdRange.newBuilder()
                    .setRowIdStart(plan.getRowIdStart())
                    .setRowCount(plan.getRowCount()));
        }
        Map<String, BatchInstall> nextPlans = new LinkedHashMap<>(plans);
        nextPlans.values().removeIf(
                plan -> plan.getStream().getTransactionId() == tx.getTransactionId());
        Map<Long, TransactionCheckpoint> nextCheckpoints = new LinkedHashMap<>(checkpoints);
        nextCheckpoints.put(tx.getTransactionId(), checkpoint.build());
        byte[] bytes = snapshot(nextPlans, nextCheckpoints);
        state.store(bytes);
        plans.clear();
        plans.putAll(nextPlans);
        checkpoints.clear();
        checkpoints.putAll(nextCheckpoints);
        planBytes = bytes.length;
        return true;
    }

    private boolean verifyMaterializedBatch(
            Transaction tx,
            MutationBatch batch,
            BatchInstall plan,
            Set<Long> coveredFiles) throws Exception {
        if (plan.getRowCount() != batch.getRowCount()
                || plan.getCommitTimestamp() != tx.getCommitTimestamp()
                || !plan.getDigest().equals(ByteString.copyFrom(batch.getDigest()))) {
            throw new IOException("Checkpoint batch identity mismatch");
        }
        List<byte[][]> decoded = rows(tx, batch);
        int rowOffset = 0;
        for (BufferSpan span : plan.getSpansList()) {
            if (span.getRowIdStart() != plan.getRowIdStart() + rowOffset
                    || rowOffset + span.getRowCount() > decoded.size()) {
                throw new IOException("Checkpoint span is incomplete or misaligned");
            }
            List<Long> rowIds = new ArrayList<>(span.getRowCount());
            for (int i = 0; i < span.getRowCount(); i++) {
                rowIds.add(span.getRowIdStart() + i);
            }
            List<IndexProto.RowLocation> locations =
                    indexes.lookupRowLocations(tx.getTable().getTableId(), rowIds);
            if (locations.size() != rowIds.size()) {
                throw new IOException("MainIndex checkpoint lookup lost positional alignment");
            }
            for (int i = 0; i < locations.size(); i++) {
                IndexProto.RowLocation location = locations.get(i);
                if (location == null) {
                    throw new IOException("MainIndex row is missing at checkpoint");
                }
                File file = metadata.getFileById(location.getFileId());
                if (file == null || file.getType() != File.Type.REGULAR) {
                    return false;
                }
                coveredFiles.add(location.getFileId());
                verifyBusinessIndexes(tx, decoded.get(rowOffset + i), rowIds.get(i), location);
            }
            rowOffset += span.getRowCount();
        }
        if (rowOffset != decoded.size()) {
            throw new IOException("Installation plan is not complete enough to checkpoint");
        }
        return true;
    }

    private void verifyBusinessIndexes(
            Transaction tx, byte[][] row, long rowId, IndexProto.RowLocation location)
            throws Exception {
        for (TableIndex index : tx.getTable().getIndexesList()) {
            ByteString encoded = IngestRows.indexKey(index, row);
            IndexProto.IndexKey key = IndexProto.IndexKey.newBuilder()
                    .setTableId(tx.getTable().getTableId())
                    .setIndexId(index.getId())
                    .setKey(encoded)
                    .setTimestamp(tx.getCommitTimestamp())
                    .build();
            IndexOption option = IndexOption.builder()
                    .vNodeId(IndexUtils.getBucketIdFromByteBuffer(encoded)).build();
            if (index.getPrimary()) {
                if (!location.equals(indexes.lookupUniqueIndex(key, option))) {
                    throw new IOException("Primary business index is not checkpoint-ready for row " + rowId);
                }
            } else {
                List<IndexProto.RowLocation> members = indexes.lookupNonUniqueIndex(key, option);
                if (members == null || !members.contains(location)) {
                    throw new IOException("Secondary business index is not checkpoint-ready for row " + rowId);
                }
            }
        }
    }

    private void verifyCheckpointRows(TransactionCheckpoint checkpoint) throws Exception {
        Set<Long> files = new HashSet<>();
        for (RowIdRange range : checkpoint.getRowRangesList()) {
            List<Long> rowIds = new ArrayList<>(range.getRowCount());
            for (int i = 0; i < range.getRowCount(); i++) {
                rowIds.add(range.getRowIdStart() + i);
            }
            List<IndexProto.RowLocation> locations =
                    indexes.lookupRowLocations(checkpoint.getTableId(), rowIds);
            if (locations.size() != rowIds.size() || locations.contains(null)) {
                throw new IOException("Checkpointed MainIndex rows are missing");
            }
            for (IndexProto.RowLocation location : locations) {
                File file = metadata.getFileById(location.getFileId());
                if (file == null || file.getType() != File.Type.REGULAR) {
                    throw new IOException("Checkpointed row points outside REGULAR storage");
                }
                files.add(location.getFileId());
            }
        }
        if (!resources.isIngestRecoveryCheckpointDurable(
                checkpoint.getCommitTimestamp(), files)) {
            throw new IOException("Published recovery checkpoint no longer covers installation checkpoint");
        }
    }

    @Override
    public synchronized void install(
            Transaction tx, Iterable<MutationBatch> batches, boolean recovering) throws Exception {
        for (MutationBatch batch : batches) {
            List<byte[][]> rows = rows(tx, batch);
            Route route = IngestWire.route(tx.getTable(), batch.getStreamId().getShardId());
            if (!IngestWire.owner(route).equals(owner)) {
                throw new IOException("Incorrect participant owner");
            }
            PixelsWriteBuffer buffer =
                    resources.getIngestBuffer(
                            tx.getTable().getSchemaName(),
                            tx.getTable().getTableName(),
                            route.getVirtualNodeId());
            buffer.beginInstallation();
            try {
                String batchKey = IngestWire.batchKey(batch.getStreamId(), batch.getSequence());
                BatchInstall plan = plans.get(batchKey);
                if (plan == null) {
                    IndexProto.RowIdBatch allocation =
                            indexes.allocateRowIdBatch(tx.getTable().getTableId(), batch.getRowCount());
                    if (allocation == null
                            || allocation.getLength() < batch.getRowCount()
                            || allocation.getRowIdStart() < 0) {
                        throw new IOException("Existing row allocator returned an insufficient range");
                    }
                    Math.addExact(allocation.getRowIdStart(), batch.getRowCount() - 1L);
                    plan =
                            BatchInstall.newBuilder()
                                    .setStream(IngestWire.encode(batch.getStreamId()))
                                    .setSequence(batch.getSequence())
                                    .setCommitTimestamp(tx.getCommitTimestamp())
                                    .setRowIdStart(allocation.getRowIdStart())
                                    .setRowCount(batch.getRowCount())
                                    .setDigest(ByteString.copyFrom(batch.getDigest()))
                                    .build();
                    save(plan);
                }
                if (plan.getCommitTimestamp() != tx.getCommitTimestamp()
                        || plan.getRowCount() != batch.getRowCount()
                        || !plan.getDigest().equals(ByteString.copyFrom(batch.getDigest()))) {
                    throw new IOException("Batch installation identity mismatch");
                }
                int offset = 0;
                for (BufferSpan span : plan.getSpansList()) {
                    installSpan(
                            tx,
                            route,
                            buffer,
                            span,
                            rows.subList(offset, offset + span.getRowCount()),
                            recovering);
                    offset += span.getRowCount();
                }
                while (offset < rows.size()) {
                    BufferSpan span =
                            buffer.planSpan(
                                    rows.size() - offset, Math.addExact(plan.getRowIdStart(), offset));
                    plan = plan.toBuilder().addSpans(span).build();
                    save(plan); // Assignments are durable before any corresponding shared row appears.
                    installSpan(
                            tx,
                            route,
                            buffer,
                            span,
                            rows.subList(offset, offset + span.getRowCount()),
                            false);
                    offset += span.getRowCount();
                }
            } finally {
                buffer.endInstallation();
            }
        }
    }

    private void installSpan(
            Transaction tx,
            Route route,
            PixelsWriteBuffer buffer,
            BufferSpan span,
            List<byte[][]> rows,
            boolean recovering)
            throws Exception {
        File file =
                recovering
                        ? catalogFiles.get(span.getFileId())
                        : metadata.getFileById(span.getFileId());
        if (file == null) {
            file = metadata.getFileById(span.getFileId());
        }
        if (file == null) {
            throw new IOException("Installation file was removed");
        }
        boolean published = file.getType() == File.Type.REGULAR;
        if (published) {
            if (recovering) {
                buffer.observePublishedSpan(span);
            }
            // MainIndex explicitly forbids putting a file again after flush.
        } else {
            buffer.installSpan(span, rows, tx.getCommitTimestamp());
            List<IndexProto.PrimaryIndexEntry> locations = new ArrayList<>();
            for (int i = 0; i < rows.size(); i++) {
                IndexProto.RowLocation location =
                        IndexProto.RowLocation.newBuilder()
                                .setFileId(span.getFileId())
                                .setRgId(0)
                                .setRgRowOffset(
                                        span.getBlockStartOffset() + span.getOffsetInBlock() + i)
                                .build();
                locations.add(
                        IndexProto.PrimaryIndexEntry.newBuilder()
                                .setRowId(span.getRowIdStart() + i)
                                .setRowLocation(location)
                                .build());
            }
            // A crash may occur after MainIndex's atomic per-file flush but before
            // catalog publication. Resolve before putting: rebuilding that flushed
            // file's cache would violate its flush-marker contract.
            List<Long> rowIds = new ArrayList<>(locations.size());
            for (IndexProto.PrimaryIndexEntry entry : locations) {
                rowIds.add(entry.getRowId());
            }
            List<IndexProto.RowLocation> existing =
                    indexes.lookupRowLocations(tx.getTable().getTableId(), rowIds);
            if (existing.size() != locations.size()) {
                throw new IOException("MainIndex lookup lost positional alignment");
            }
            List<IndexProto.PrimaryIndexEntry> missing = new ArrayList<>();
            for (int i = 0; i < locations.size(); i++) {
                if (existing.get(i) == null) {
                    missing.add(locations.get(i));
                } else if (!existing.get(i).equals(locations.get(i).getRowLocation())) {
                    throw new IOException("Recorded rowId resolves to another storage location");
                }
            }
            if (!missing.isEmpty()) {
                indexes.putMainIndexEntriesOnly(tx.getTable().getTableId(), missing);
            }
        }
        for (TableIndex index : tx.getTable().getIndexesList()) {
            Map<Integer, List<IndexProto.PrimaryIndexEntry>> primary = new HashMap<>();
            Map<Integer, List<IndexProto.SecondaryIndexEntry>> secondary = new HashMap<>();
            for (int i = 0; i < rows.size(); i++) {
                ByteString key = IngestRows.indexKey(index, rows.get(i));
                int bucket = IndexUtils.getBucketIdFromByteBuffer(key);
                IndexProto.IndexKey version =
                        IndexProto.IndexKey.newBuilder()
                                .setTableId(tx.getTable().getTableId())
                                .setIndexId(index.getId())
                                .setKey(key)
                                .setTimestamp(tx.getCommitTimestamp())
                                .build();
                if (index.getPrimary()) {
                    primary.computeIfAbsent(bucket, k -> new ArrayList<>())
                            .add(
                                    IndexProto.PrimaryIndexEntry.newBuilder()
                                            .setIndexKey(version)
                                            .setRowId(span.getRowIdStart() + i)
                                            .build());
                } else {
                    secondary
                            .computeIfAbsent(bucket, k -> new ArrayList<>())
                            .add(
                                    IndexProto.SecondaryIndexEntry.newBuilder()
                                            .setIndexKey(version)
                                            .setRowId(span.getRowIdStart() + i)
                                            .build());
                }
            }
            for (Map.Entry<Integer, List<IndexProto.PrimaryIndexEntry>> entry :
                    primary.entrySet()) {
                indexes.putPrimaryIndexEntriesOnly(
                        tx.getTable().getTableId(),
                        index.getId(),
                        entry.getValue(),
                        IndexOption.builder().vNodeId(entry.getKey()).build());
            }
            for (Map.Entry<Integer, List<IndexProto.SecondaryIndexEntry>> entry :
                    secondary.entrySet()) {
                indexes.putSecondaryIndexEntries(
                        tx.getTable().getTableId(),
                        index.getId(),
                        entry.getValue(),
                        IndexOption.builder().vNodeId(entry.getKey()).build());
            }
        }
    }

    @Override
    public synchronized void close() throws IOException {
        state.close();
    }
}
