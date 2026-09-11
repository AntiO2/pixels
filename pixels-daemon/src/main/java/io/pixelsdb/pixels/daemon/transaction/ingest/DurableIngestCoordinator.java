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
package io.pixelsdb.pixels.daemon.transaction.ingest;

import com.google.protobuf.ByteString;

import io.pixelsdb.pixels.common.ingest.durable.AtomicStateFile;
import io.pixelsdb.pixels.common.ingest.wire.IngestWire;
import io.pixelsdb.pixels.ingest.IngestProto.*;

import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.Closeable;
import java.io.IOException;
import java.time.Clock;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.LongSupplier;

/**
 * Single-owner LOCAL-durability transaction coordinator. The locked state volume is
 * authoritative. No memory update or remote installation precedes a durable decision.
 * Endpoint topology is frozen in the first publication snapshot and cannot migrate silently.
 */
public final class DurableIngestCoordinator implements Closeable {
    public interface Tables {
        TableSpec load(String schema, String table) throws Exception;

        List<Route> routes() throws Exception;
    }

    public interface Participants {
        PrepareToken prepare(String owner, Transaction transaction) throws Exception;

        void install(String owner, Transaction transaction) throws Exception;

        void discard(String owner, long transactionId) throws Exception;
    }

    private static final Logger LOG = LogManager.getLogger(DurableIngestCoordinator.class);
    private final AtomicStateFile store;
    private final Tables tables;
    private final Participants participants;
    private final LongSupplier ids;
    private final Clock clock;
    private final long leaseMillis;
    private final int maxTransactions;
    private final int maxStreams;
    private final Object publisher = new Object();
    private final ScheduledExecutorService recovery;
    private CoordinatorSnapshot snapshot;
    private volatile boolean closed;
    private final Set<String> cleaned = new HashSet<>();

    public DurableIngestCoordinator(
            AtomicStateFile store,
            Tables tables,
            Participants participants,
            LongSupplier ids,
            Clock clock,
            long baselineTimestamp,
            long leaseMillis,
            int maxTransactions,
            int maxStreams)
            throws IOException {
        this.store = store;
        this.tables = tables;
        this.participants = participants;
        this.ids = ids;
        this.clock = clock;
        this.leaseMillis = leaseMillis;
        this.maxTransactions = maxTransactions;
        this.maxStreams = maxStreams;
        byte[] bytes = store.read();
        if (bytes.length == 0) {
            snapshot =
                    CoordinatorSnapshot.newBuilder()
                            .setVersion(1)
                            .setPublishedTimestamp(baselineTimestamp)
                            .setLastCommitTimestamp(baselineTimestamp)
                            .build();
            save(snapshot);
        } else {
            snapshot = CoordinatorSnapshot.parseFrom(bytes);
            if (snapshot.getVersion() != 1
                    || snapshot.getPublishedTimestamp() > snapshot.getLastCommitTimestamp()) {
                throw new IOException("Invalid transaction checkpoint");
            }
            Set<Long> txIds = new HashSet<>();
            Set<String> requests = new HashSet<>();
            Set<Long> commitTimes = new HashSet<>();
            for (Transaction transaction : snapshot.getTransactionsList()) {
                if (!txIds.add(transaction.getTransactionId())
                        || !requests.add(transaction.getRequestId())) {
                    throw new IOException("Duplicate transaction identity in checkpoint");
                }
                if (IngestWire.committed(transaction)
                        && (!commitTimes.add(transaction.getCommitTimestamp())
                                || transaction.getCommitTimestamp() <= 0
                                || transaction.getCommitTimestamp()
                                        > snapshot.getLastCommitTimestamp())) {
                    throw new IOException("Invalid commit order in checkpoint");
                }
                if (transaction.getState() == TransactionState.COMMIT_DECIDED
                        && transaction.getCommitTimestamp() <= snapshot.getPublishedTimestamp()) {
                    throw new IOException("Published prefix crosses an uninstalled transaction");
                }
                if (transaction.getState() == TransactionState.PUBLISHED
                        && transaction.getCommitTimestamp() > snapshot.getPublishedTimestamp()) {
                    throw new IOException("Published transaction above publication point");
                }
                Set<Long> writerIds = new HashSet<>();
                Set<String> writerRequests = new HashSet<>();
                for (WriterAssignment writer : transaction.getWritersList()) {
                    if (writer.getWriterId() <= 0
                            || writer.getRequestId().isEmpty()
                            || writer.getRequestId().length() > 512
                            || !writerIds.add(writer.getWriterId())
                            || !writerRequests.add(writer.getRequestId())) {
                        throw new IOException("Invalid writer assignment in checkpoint");
                    }
                }
                if (transaction.getState() == TransactionState.UNRECOGNIZED) {
                    throw new IOException("Unknown transaction state");
                }
            }
        }
        recovery =
                Executors.newSingleThreadScheduledExecutor(
                        r -> {
                            Thread thread = new Thread(r, "pixels-ingest-decisions");
                            thread.setDaemon(true);
                            return thread;
                        });
    }

    public void start() {
        recovery.scheduleWithFixedDelay(
                () -> {
                    if (closed) {
                        return;
                    }
                    try {
                        expire();
                        drivePublication();
                        cleanupTerminalReservations();
                    } catch (Exception e) {
                        LOG.warn("Ingest reconciliation will retry: {}", e.toString());
                    }
                },
                0,
                1,
                TimeUnit.SECONDS);
    }

    private void checkOpen() throws IOException {
        if (closed) {
            throw new IOException("Coordinator closed");
        }
    }

    private void save(CoordinatorSnapshot value) throws IOException {
        store.store(value.toByteArray());
        snapshot = value;
    }

    private int position(long id) throws IOException {
        checkOpen();
        for (int i = 0; i < snapshot.getTransactionsCount(); i++) {
            if (snapshot.getTransactions(i).getTransactionId() == id) {
                return i;
            }
        }
        throw new IOException("Unknown ingest transaction " + id + "; absence is not ABORT");
    }

    private void replace(Transaction value) throws IOException {
        save(
                snapshot.toBuilder()
                        .setTransactions(position(value.getTransactionId()), value)
                        .build());
    }

    private long expiry() {
        return Math.addExact(clock.millis(), leaseMillis);
    }

    private void live(Transaction tx) throws IOException {
        if (tx.getExpiresAtMillis() <= clock.millis()) {
            throw new IOException("Transaction lease expired: " + tx.getTransactionId());
        }
    }

    public synchronized Transaction get(long id) throws IOException {
        return snapshot.getTransactions(position(id));
    }

    public synchronized long publishedTimestamp() {
        return snapshot.getPublishedTimestamp();
    }

    public Transaction begin(BeginWriteRequest request) throws Exception {
        if (request.getRequestId().isEmpty() || request.getRequestId().length() > 512) {
            throw new IllegalArgumentException("A bounded request id is required");
        }
        synchronized (this) {
            checkOpen();
            for (Transaction existing : snapshot.getTransactionsList()) {
                if (existing.getRequestId().equals(request.getRequestId())) {
                    if (!existing.getTable().getSchemaName().equals(request.getSchemaName())
                            || !existing.getTable().getTableName().equals(request.getTableName())
                            || existing.getReadTimestamp() != request.getReadTimestamp()) {
                        throw new IOException(
                                "Begin request identity reused with different arguments");
                    }
                    return existing;
                }
            }
            if (snapshot.getTransactionsCount() >= maxTransactions) {
                throw new IOException("Transaction metadata capacity reached");
            }
        }
        TableSpec descriptor = tables.load(request.getSchemaName(), request.getTableName());
        synchronized (this) {
            // Concurrent retransmissions must not allocate two transaction identities.
            for (Transaction existing : snapshot.getTransactionsList()) {
                if (existing.getRequestId().equals(request.getRequestId())) {
                    return begin(request);
                }
            }
            if (request.getReadTimestamp() > snapshot.getPublishedTimestamp()) {
                throw new IOException("Unpublished read timestamp");
            }
            pinRoutes(descriptor.getRoutesList());
            Transaction tx =
                    Transaction.newBuilder()
                            .setTransactionId(ids.getAsLong())
                            .setRequestId(request.getRequestId())
                            .setReadTimestamp(request.getReadTimestamp())
                            .setTable(descriptor)
                            .setState(TransactionState.OPEN)
                            .setExpiresAtMillis(expiry())
                            .build();
            for (Transaction old : snapshot.getTransactionsList()) {
                if (old.getTransactionId() == tx.getTransactionId()) {
                    throw new IOException("Allocator reused transaction identity");
                }
            }
            if (snapshot.getTransactionsCount() >= maxTransactions) {
                throw new IOException("Transaction metadata capacity reached");
            }
            save(snapshot.toBuilder().addTransactions(tx).build());
            return tx;
        }
    }

    private void pinRoutes(List<Route> proposed) throws IOException {
        if (proposed.isEmpty()) {
            throw new IOException("No Retina route available");
        }
        Set<Integer> ids = new HashSet<>();
        for (Route route : proposed) {
            if (!ids.add(route.getShardId())
                    || route.getHost().isEmpty()
                    || route.getPort() <= 0
                    || route.getPort() > 65535) {
                throw new IOException("Invalid or duplicate shard route");
            }
        }
        if (snapshot.getRoutesCount() == 0) {
            save(snapshot.toBuilder().addAllRoutes(proposed).build());
        } else if (!snapshot.getRoutesList().equals(proposed)) {
            throw new IOException(
                    "LOCAL ingest topology changed; recover the original owners and volumes before"
                        + " proceeding");
        }
    }

    public Publication publication() throws Exception {
        List<Route> current = tables.routes();
        synchronized (this) {
            checkOpen();
            pinRoutes(current);
            return Publication.newBuilder()
                    .setPublishedTimestamp(snapshot.getPublishedTimestamp())
                    .addAllRoutes(snapshot.getRoutesList())
                    .build();
        }
    }

    /** Allocate an idempotent, transaction-local identity for one physical writer. */
    public synchronized WriterAssignment allocateWriter(AllocateWriterRequest request)
            throws IOException {
        if (request.getRequestId().isEmpty() || request.getRequestId().length() > 512) {
            throw new IllegalArgumentException("A bounded writer request id is required");
        }
        Transaction tx = get(request.getTransactionId());
        if (tx.getState() == TransactionState.ABORTED) {
            throw new IOException("Transaction aborted");
        }
        long maximum = 0;
        for (WriterAssignment writer : tx.getWritersList()) {
            if (writer.getRequestId().equals(request.getRequestId())) {
                if (writer.getTaskId() != request.getTaskId()) {
                    throw new IOException("Writer request reused with a different task identity");
                }
                return writer;
            }
            maximum = Math.max(maximum, writer.getWriterId());
        }
        if (tx.getState() != TransactionState.OPEN) {
            throw new IOException("Transaction input is sealed");
        }
        live(tx);
        if (tx.getWritersCount() >= maxStreams) {
            throw new IOException("Transaction writer limit exceeded");
        }
        // Existing clients may already have registered a stream before using this API.
        for (StreamId stream : tx.getStreamsList()) {
            maximum = Math.max(maximum, stream.getWriterId());
        }
        if (maximum == Long.MAX_VALUE) {
            throw new IOException("Transaction writer identities exhausted");
        }
        WriterAssignment assignment =
                WriterAssignment.newBuilder()
                        .setRequestId(request.getRequestId())
                        .setTaskId(request.getTaskId())
                        .setWriterId(maximum + 1)
                        .build();
        replace(tx.toBuilder().addWriters(assignment).setExpiresAtMillis(expiry()).build());
        return assignment;
    }

    public synchronized Transaction register(StreamId stream) throws IOException {
        Transaction tx = get(stream.getTransactionId());
        IngestWire.decode(stream);
        if (stream.getTableId() != tx.getTable().getTableId()
                || stream.getKind() != MutationKind.APPEND_ROWS) {
            throw new IOException("Stream does not match the INSERT transaction");
        }
        IngestWire.route(tx.getTable(), stream.getShardId());
        if (tx.getStreamsList().contains(stream)) {
            if (tx.getState() == TransactionState.ABORTED) {
                throw new IOException("Transaction aborted");
            }
            return tx;
        }
        if (tx.getState() != TransactionState.OPEN) {
            throw new IOException("Transaction input is sealed");
        }
        live(tx);
        if (tx.getStreamsCount() >= maxStreams) {
            throw new IOException("Transaction stream limit exceeded");
        }
        tx = tx.toBuilder().addStreams(stream).setExpiresAtMillis(expiry()).build();
        replace(tx);
        return tx;
    }

    public synchronized Transaction touch(long id) throws IOException {
        Transaction tx = get(id);
        if (tx.getState() == TransactionState.ABORTED || IngestWire.committed(tx)) {
            return tx;
        }
        live(tx);
        tx = tx.toBuilder().setExpiresAtMillis(expiry()).build();
        replace(tx);
        return tx;
    }

    private static List<StreamSeal> canonical(List<StreamSeal> input) {
        List<StreamSeal> result = new ArrayList<>(input);
        result.sort(
                Comparator.comparingLong((StreamSeal s) -> s.getStream().getWriterId())
                        .thenComparingInt(s -> s.getStream().getShardId())
                        .thenComparingInt(s -> s.getStream().getKindValue()));
        return result;
    }

    public Transaction prepare(PrepareWriteRequest request) throws Exception {
        Transaction tx;
        List<StreamSeal> seals = canonical(request.getSealsList());
        synchronized (this) {
            tx = get(request.getTransactionId());
            if (tx.getState() == TransactionState.ABORTED) {
                throw new IOException("Transaction aborted");
            }
            if (tx.getState() != TransactionState.OPEN) {
                if (!tx.getSealsList().equals(seals)) {
                    throw new IOException("Sealed manifest mismatch");
                }
                if (tx.getState() == TransactionState.PREPARED || IngestWire.committed(tx)) {
                    return tx;
                }
            } else {
                live(tx);
                Set<StreamId> declared = new HashSet<>();
                for (StreamSeal seal : seals) {
                    IngestWire.decode(seal);
                    if (!declared.add(seal.getStream())
                            || seal.getStream().getTransactionId() != tx.getTransactionId()) {
                        throw new IOException("Duplicate or foreign manifest stream");
                    }
                }
                if (!declared.equals(new HashSet<>(tx.getStreamsList()))) {
                    throw new IOException("Manifest must exactly cover every registered stream");
                }
                tx =
                        tx.toBuilder()
                                .addAllSeals(seals)
                                .setState(TransactionState.SEALED)
                                .setExpiresAtMillis(expiry())
                                .build();
                replace(tx);
            }
        }
        List<PrepareToken> tokens = new ArrayList<>();
        for (String owner : IngestWire.owners(tx)) {
            PrepareToken token = participants.prepare(owner, tx);
            if (!token.getOwner().equals(owner)
                    || !token.getDigest()
                            .equals(ByteString.copyFrom(IngestWire.prepareDigest(tx, owner)))) {
                throw new IOException("Prepare token mismatch from participant");
            }
            tokens.add(token);
        }
        synchronized (this) {
            Transaction current = get(tx.getTransactionId());
            if (current.getState() == TransactionState.ABORTED) {
                throw new IOException("Transaction aborted during Prepare");
            }
            if (current.getState() == TransactionState.PREPARED || IngestWire.committed(current)) {
                return current;
            }
            live(current);
            tx =
                    current.toBuilder()
                            .clearTokens()
                            .addAllTokens(tokens)
                            .setState(TransactionState.PREPARED)
                            .setExpiresAtMillis(expiry())
                            .build();
            replace(tx);
            return tx;
        }
    }

    public Transaction commit(long id) throws Exception {
        synchronized (this) {
            Transaction tx = get(id);
            if (tx.getState() == TransactionState.PUBLISHED) {
                return tx;
            }
            if (tx.getState() == TransactionState.ABORTED) {
                throw new IOException("Transaction is ABORTED");
            }
            if (!IngestWire.committed(tx)) {
                if (tx.getState() != TransactionState.PREPARED) {
                    throw new IOException("Commit requires PREPARED");
                }
                live(tx);
                long timestamp = ids.getAsLong();
                if (timestamp <= snapshot.getLastCommitTimestamp()
                        || timestamp <= 0
                        || timestamp >= (1L << 48)) {
                    throw new IOException(
                            "Commit allocator is not monotonic or exceeds Retina timestamp width");
                }
                Transaction decided =
                        tx.toBuilder()
                                .setState(TransactionState.COMMIT_DECIDED)
                                .setCommitTimestamp(timestamp)
                                .build();
                save(
                        snapshot.toBuilder()
                                .setTransactions(position(id), decided)
                                .setLastCommitTimestamp(timestamp)
                                .build());
            }
        }
        drivePublication();
        return get(id);
    }

    public void drivePublication() throws Exception {
        synchronized (publisher) {
            while (!closed) {
                Transaction next;
                synchronized (this) {
                    next =
                            snapshot.getTransactionsList().stream()
                                    .filter(t -> t.getState() == TransactionState.COMMIT_DECIDED)
                                    .min(Comparator.comparingLong(Transaction::getCommitTimestamp))
                                    .orElse(null);
                }
                if (next == null) {
                    return;
                }
                for (String owner : IngestWire.owners(next)) {
                    participants.install(owner, next);
                }
                synchronized (this) {
                    Transaction current = get(next.getTransactionId());
                    if (current.getState() != TransactionState.COMMIT_DECIDED) {
                        throw new IOException("Invalid installation transition");
                    }
                    save(
                            snapshot.toBuilder()
                                    .setTransactions(
                                            position(current.getTransactionId()),
                                            current.toBuilder()
                                                    .setState(TransactionState.PUBLISHED))
                                    .setPublishedTimestamp(current.getCommitTimestamp())
                                    .build());
                }
                // Release reservations only after the common snapshot is published.
                for (String owner : IngestWire.owners(next)) {
                    try {
                        participants.discard(owner, next.getTransactionId());
                    } catch (Exception e) {
                        LOG.warn("Reservation release will retry: {}", e.toString());
                    }
                }
            }
        }
    }

    public Transaction abort(long id) throws Exception {
        Transaction tx;
        synchronized (this) {
            tx = get(id);
            if (IngestWire.committed(tx)) {
                return tx;
            }
            if (tx.getState() != TransactionState.ABORTED) {
                tx = tx.toBuilder().setState(TransactionState.ABORTED).build();
                replace(tx);
            }
        }
        // Cleanup is driven by reconciliation. Do not block a durable ABORT on an unavailable
        // participant.
        return tx;
    }

    public synchronized TransactionList list(String owner) throws IOException {
        TransactionList.Builder result =
                TransactionList.newBuilder()
                        .setPublishedTimestamp(snapshot.getPublishedTimestamp());
        for (Transaction tx : snapshot.getTransactionsList()) {
            if (IngestWire.owners(tx).contains(owner)) {
                result.addTransactions(tx);
            }
        }
        return result.build();
    }

    public void expire() throws Exception {
        List<Long> expired = new ArrayList<>();
        synchronized (this) {
            for (Transaction tx : snapshot.getTransactionsList()) {
                if (!IngestWire.committed(tx)
                        && tx.getState() != TransactionState.ABORTED
                        && tx.getExpiresAtMillis() <= clock.millis()) {
                    expired.add(tx.getTransactionId());
                }
            }
        }
        for (long id : expired) {
            abort(id);
        }
    }

    private void cleanupTerminalReservations() throws Exception {
        List<Transaction> terminal;
        synchronized (this) {
            terminal = new ArrayList<>(snapshot.getTransactionsList());
        }
        for (Transaction tx : terminal) {
            if (tx.getState() == TransactionState.ABORTED
                    || tx.getState() == TransactionState.PUBLISHED) {
                for (String owner : IngestWire.owners(tx)) {
                    String identity = owner + ":" + tx.getTransactionId();
                    if (!cleaned.contains(identity)) {
                        participants.discard(owner, tx.getTransactionId());
                        cleaned.add(identity);
                    }
                }
            }
        }
    }

    @Override
    public void close() throws IOException {
        closed = true;
        recovery.shutdownNow();
        synchronized (publisher) {
            synchronized (this) {
                store.close();
            }
        }
    }
}
