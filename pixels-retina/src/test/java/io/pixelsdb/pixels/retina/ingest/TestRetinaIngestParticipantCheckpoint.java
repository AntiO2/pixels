/*
 * Copyright 2026 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the Affero GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */
package io.pixelsdb.pixels.retina.ingest;

import com.google.protobuf.ByteString;
import io.pixelsdb.pixels.common.ingest.MutationBatch;
import io.pixelsdb.pixels.common.ingest.MutationStreamId;
import io.pixelsdb.pixels.common.ingest.MutationStreamSeal;
import io.pixelsdb.pixels.common.ingest.wire.IngestWire;
import io.pixelsdb.pixels.ingest.IngestProto.PrepareToken;
import io.pixelsdb.pixels.ingest.IngestProto.Route;
import io.pixelsdb.pixels.ingest.IngestProto.TableSpec;
import io.pixelsdb.pixels.ingest.IngestProto.Transaction;
import io.pixelsdb.pixels.ingest.IngestProto.TransactionList;
import io.pixelsdb.pixels.ingest.IngestProto.TransactionState;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

public class TestRetinaIngestParticipantCheckpoint
{
    @Test
    public void testCheckpointThenWalReclaimSurvivesRestartWithoutReplay() throws Exception
    {
        Path directory = Files.createTempDirectory("pixels-participant-checkpoint-");
        String owner = "127.0.0.1:18889";
        MutationStreamId stream = new MutationStreamId(
                41, 7, 13, 0, MutationStreamId.Kind.APPEND_ROWS);
        MutationBatch batch = new MutationBatch(stream, 0, 3, 1, 2, new byte[] {4, 1});
        MutationStreamSeal seal = new MutationStreamSeal(
                stream, 1, 2, batch.getPayloadBytes(),
                MutationStreamSeal.extendDigest(
                        MutationStreamSeal.emptyDigest(), batch.getDigest()));
        TableSpec table = TableSpec.newBuilder()
                .setTableId(13)
                .setSchemaVersion(3)
                .setFingerprint(ByteString.copyFromUtf8("table-13-v3"))
                .addRoutes(Route.newBuilder().setShardId(0).setHost("127.0.0.1").setPort(18889))
                .build();
        Transaction unsigned = Transaction.newBuilder()
                .setTransactionId(41)
                .setCommitTimestamp(52)
                .setState(TransactionState.PUBLISHED)
                .setTable(table)
                .addStreams(IngestWire.encode(stream))
                .addSeals(IngestWire.encode(seal))
                .build();
        Transaction transaction = unsigned.toBuilder()
                .addTokens(PrepareToken.newBuilder()
                        .setOwner(owner)
                        .setDigest(ByteString.copyFrom(IngestWire.prepareDigest(unsigned, owner))))
                .build();
        Decisions decisions = new Decisions(transaction);
        AtomicBoolean durableCheckpoint = new AtomicBoolean();
        AtomicInteger firstInstalls = new AtomicInteger();

        try
        {
            try (LocalMutationJournal journal = open(directory))
            {
                journal.append(batch);
                journal.seal(seal);
                RetinaIngestParticipant participant = new RetinaIngestParticipant(
                        owner, journal, decisions,
                        new Installer(durableCheckpoint, firstInstalls), new IngestReadPins(10_000));
                participant.recover();
                assertEquals(1, firstInstalls.get());
                participant.checkpointPublishedTransactions();
                assertTrue(durableCheckpoint.get());
                expectIo(() -> journal.readSealedBatch(stream, 0));
                expectIo(() -> journal.append(batch));
                participant.close();
            }

            AtomicInteger restartInstalls = new AtomicInteger();
            try (LocalMutationJournal journal = open(directory))
            {
                RetinaIngestParticipant restarted = new RetinaIngestParticipant(
                        owner, journal, decisions,
                        new Installer(durableCheckpoint, restartInstalls), new IngestReadPins(10_000));
                restarted.recover();
                assertEquals("checkpointed transaction was replayed", 0, restartInstalls.get());
                expectIo(() -> journal.readSealedBatch(stream, 0));
                expectIo(() -> journal.append(batch));
                restarted.close();
            }
        }
        finally
        {
            try (java.util.stream.Stream<Path> paths = Files.walk(directory))
            {
                Path[] ordered = paths.sorted(java.util.Comparator.reverseOrder()).toArray(Path[]::new);
                for (Path path : ordered)
                {
                    Files.deleteIfExists(path);
                }
            }
        }
    }

    private static LocalMutationJournal open(Path directory) throws IOException
    {
        return new LocalMutationJournal(directory, 1024, 1_000_000, 1000);
    }

    private static void expectIo(Checked action) throws Exception
    {
        try
        {
            action.run();
            fail("expected IOException");
        }
        catch (IOException expected)
        {
            // Expected: the compacted generation retains only its durable late-request fence.
        }
    }

    private interface Checked
    {
        void run() throws Exception;
    }

    private static final class Decisions implements RetinaIngestParticipant.Decisions
    {
        private final Transaction transaction;

        private Decisions(Transaction transaction)
        {
            this.transaction = transaction;
        }

        @Override
        public Transaction get(long id)
        {
            return transaction;
        }

        @Override
        public Transaction abort(long id)
        {
            throw new AssertionError("published transaction must not abort");
        }

        @Override
        public TransactionList list(String owner)
        {
            return TransactionList.newBuilder()
                    .setPublishedTimestamp(transaction.getCommitTimestamp())
                    .addTransactions(transaction)
                    .build();
        }
    }

    private static final class Installer implements RetinaIngestParticipant.Installer
    {
        private final AtomicBoolean checkpoint;
        private final AtomicInteger installs;

        private Installer(AtomicBoolean checkpoint, AtomicInteger installs)
        {
            this.checkpoint = checkpoint;
            this.installs = installs;
        }

        @Override
        public void prepare(Transaction tx, Iterable<MutationBatch> batches) {}

        @Override
        public void install(Transaction tx, Iterable<MutationBatch> batches, boolean recovering)
        {
            for (MutationBatch ignored : batches)
            {
                // Force the recovery path to consume the WAL before it is checkpointed.
            }
            installs.incrementAndGet();
        }

        @Override
        public void release(long txId) {}

        @Override
        public void initializeRecovery(java.util.List<Transaction> transactions) {}

        @Override
        public boolean recoveredByCheckpoint(Transaction tx)
        {
            return checkpoint.get();
        }

        @Override
        public boolean checkpoint(Transaction tx, Iterable<MutationBatch> batches)
        {
            int count = 0;
            for (MutationBatch ignored : batches)
            {
                count++;
            }
            assertEquals(1, count);
            checkpoint.set(true);
            return true;
        }

        @Override
        public void close() {}
    }
}
