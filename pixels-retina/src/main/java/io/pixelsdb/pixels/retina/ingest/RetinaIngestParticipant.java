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

import io.pixelsdb.pixels.common.ingest.*;
import io.pixelsdb.pixels.common.ingest.wire.IngestWire;
import io.pixelsdb.pixels.ingest.IngestProto.*;

import java.io.Closeable;
import java.io.IOException;
import java.util.*;

/** Exact stream receipts, authoritative decisions, and private preparation around existing Retina storage. */
public final class RetinaIngestParticipant implements Closeable {
    public interface Decisions {
        Transaction get(long id) throws Exception;

        Transaction abort(long id) throws Exception;

        TransactionList list(String owner) throws Exception;
    }

    public interface Installer extends Closeable {
        void prepare(Transaction tx, Iterable<MutationBatch> batches) throws Exception;

        void install(Transaction tx, Iterable<MutationBatch> batches, boolean recovering)
                throws Exception;

        void release(long txId) throws Exception;

        void initializeRecovery(List<Transaction> transactions) throws Exception;
    }

    private final String owner;
    private final LocalMutationJournal journal;
    private final Decisions decisions;
    private final Installer installer;
    private final IngestReadPins readPins;
    private final Map<Long, ByteString> prepared = new HashMap<>();
    private final Set<Long> installed = new HashSet<>();
    private long lastInstalledTimestamp;
    private boolean ready;

    public RetinaIngestParticipant(
            String owner,
            LocalMutationJournal journal,
            Decisions decisions,
            Installer installer,
            IngestReadPins readPins) {
        this.owner = owner;
        this.journal = journal;
        this.decisions = decisions;
        this.installer = installer;
        this.readPins = readPins;
    }

    private void serving() throws IOException {
        if (!ready) {
            throw new IOException("Retina ingest participant is not ready");
        }
    }

    private void owns(Transaction tx, MutationStreamId stream) throws IOException {
        if (stream.getTransactionId() != tx.getTransactionId()
                || stream.getTableId() != tx.getTable().getTableId()
                || !IngestWire.owner(IngestWire.route(tx.getTable(), stream.getShardId()))
                        .equals(owner)
                || !tx.getStreamsList().contains(IngestWire.encode(stream))) {
            throw new IOException("Unregistered or incorrectly routed stream");
        }
    }

    public synchronized void append(MutationBatch batch) throws Exception {
        serving();
        Transaction tx = decisions.get(batch.getStreamId().getTransactionId());
        owns(tx, batch.getStreamId());
        if (tx.getState() != TransactionState.OPEN) {
            throw new IOException("Transaction no longer accepts input");
        }
        if (batch.getSchemaVersion() != tx.getTable().getSchemaVersion()
                || batch.getPayloadFormat()
                        != io.pixelsdb.pixels.common.ingest.wire.ColumnBatchCodec.FORMAT
                || batch.getStreamId().getKind() != MutationStreamId.Kind.APPEND_ROWS) {
            throw new IOException("Unsupported batch kind, codec, or schema version");
        }
        journal.append(batch);
    }

    public synchronized MutationStreamSeal seal(MutationStreamSeal seal) throws Exception {
        serving();
        Transaction tx = decisions.get(seal.getStreamId().getTransactionId());
        owns(tx, seal.getStreamId());
        if (tx.getState() == TransactionState.ABORTED) {
            throw new IOException("Transaction aborted");
        }
        journal.seal(seal);
        return seal;
    }

    private Transaction authoritative(Transaction requested) throws Exception {
        Transaction current = decisions.get(requested.getTransactionId());
        if (!current.getTable().equals(requested.getTable())
                || !current.getSealsList().equals(requested.getSealsList())) {
            throw new IOException("Participant manifest differs from authoritative transaction");
        }
        if (!IngestWire.owners(current).contains(owner)) {
            throw new IOException("This node is not a transaction participant");
        }
        return current;
    }

    private Iterable<MutationBatch> batches(Transaction tx) throws Exception {
        List<StreamSeal> seals = IngestWire.localSeals(tx, owner);
        for (StreamSeal expected : seals) {
            MutationStreamSeal receipt =
                    journal.getSeal(IngestWire.decode(expected.getStream()))
                            .orElseThrow(() -> new IOException("Missing stream seal"));
            if (!IngestWire.decode(expected).equals(receipt)) {
                throw new IOException("Missing durable stream seal");
            }
        }
        // Re-iterable, bounded replay: retain descriptors, not the transaction's entire payload.
        return () ->
                new Iterator<MutationBatch>() {
                    private int stream;
                    private long sequence;

                    public boolean hasNext() {
                        while (stream < seals.size()
                                && sequence >= seals.get(stream).getBatchCount()) {
                            stream++;
                            sequence = 0;
                        }
                        return stream < seals.size();
                    }

                    public MutationBatch next() {
                        if (!hasNext()) {
                            throw new NoSuchElementException();
                        }
                        try {
                            return journal.readSealedBatch(
                                    IngestWire.decode(seals.get(stream).getStream()), sequence++);
                        } catch (IOException e) {
                            throw new java.io.UncheckedIOException(e);
                        }
                    }
                };
    }

    public synchronized PrepareToken prepare(Transaction request) throws Exception {
        serving();
        Transaction tx = authoritative(request);
        if (tx.getState() != TransactionState.SEALED
                && tx.getState() != TransactionState.PREPARED) {
            throw new IOException("Transaction is not in a preparable state");
        }
        ByteString digest = ByteString.copyFrom(IngestWire.prepareDigest(tx, owner));
        ByteString old = prepared.get(tx.getTransactionId());
        if (old != null && !old.equals(digest)) {
            throw new IOException("Prepared digest mismatch");
        }
        if (old == null) {
            Iterable<MutationBatch> batches = batches(tx);
            try {
                installer.prepare(tx, batches);
                prepared.put(tx.getTransactionId(), digest);
            } catch (Exception e) {
                installer.release(tx.getTransactionId());
                throw e;
            }
        }
        return PrepareToken.newBuilder().setOwner(owner).setDigest(digest).build();
    }

    public synchronized void install(Transaction request) throws Exception {
        serving();
        installAuthorized(authoritative(request), false);
    }

    private void installAuthorized(Transaction tx, boolean recovering) throws Exception {
        if (!IngestWire.committed(tx)) {
            throw new IOException("Installation requires authoritative COMMIT");
        }
        if (installed.contains(tx.getTransactionId())) {
            return;
        }
        if (tx.getCommitTimestamp() < lastInstalledTimestamp) {
            throw new IOException("Commit installation order violation");
        }
        boolean validToken = false;
        for (PrepareToken token : tx.getTokensList()) {
            if (token.getOwner().equals(owner)
                    && token.getDigest()
                            .equals(ByteString.copyFrom(IngestWire.prepareDigest(tx, owner)))) {
                validToken = true;
            }
        }
        if (!validToken) {
            throw new IOException("Committed transaction lacks the participant prepare token");
        }
        installer.install(tx, batches(tx), recovering);
        installed.add(tx.getTransactionId());
        lastInstalledTimestamp = tx.getCommitTimestamp();
    }

    public synchronized void discard(long id) throws Exception {
        if (!ready) {
            return;
        } // Recovery resolves decisions before accepting work.
        Transaction tx = decisions.get(id);
        if (tx.getState() == TransactionState.ABORTED) {
            journal.discardAbortedTransaction(id);
            installer.release(id);
            prepared.remove(id);
        } else if (tx.getState() == TransactionState.PUBLISHED) {
            installer.release(id);
            prepared.remove(id);
        }
        // COMMIT_DECIDED retains reservations until global publication.
    }

    public synchronized void recover() throws Exception {
        ready = false;
        List<Transaction> transactions =
                new ArrayList<>(decisions.list(owner).getTransactionsList());
        // No local timer may release a PREPARED transaction that has already committed.
        for (int i = 0; i < transactions.size(); i++) {
            Transaction tx = transactions.get(i);
            if (!IngestWire.committed(tx) && tx.getState() != TransactionState.ABORTED) {
                transactions.set(i, decisions.abort(tx.getTransactionId()));
            }
        }
        installer.initializeRecovery(transactions);
        transactions.sort(Comparator.comparingLong(Transaction::getCommitTimestamp));
        for (Transaction tx : transactions) {
            if (tx.getState() == TransactionState.ABORTED) {
                journal.discardAbortedTransaction(tx.getTransactionId());
            } else if (IngestWire.committed(tx)) {
                installAuthorized(tx, true);
            }
        }
        ready = true;
        readPins.ready();
    }

    public synchronized ReadPin pinRead(ReadPin request) throws Exception {
        serving();
        if (request.getReadTimestamp() > decisions.list(owner).getPublishedTimestamp()) {
            throw new IOException("Cannot pin an unpublished read timestamp");
        }
        return readPins.pin(request);
    }

    public IngestReadPins readPins() {
        return readPins;
    }

    public synchronized boolean isReady() {
        return ready;
    }

    @Override
    public synchronized void close() throws IOException {
        ready = false;
        try {
            installer.close();
        } finally {
            journal.close();
        }
    }
}
