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

import io.pixelsdb.pixels.common.ingest.MutationBatch;
import io.pixelsdb.pixels.common.ingest.MutationStreamId;
import io.pixelsdb.pixels.common.ingest.MutationStreamSeal;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.zip.CRC32;

/**
 * Bounded, transaction-private staging journal shared by many streams.
 *
 * <p>Append acknowledges acceptance only. Seal forces the WAL, atomically
 * replaces a checksummed durable-prefix marker, and syncs its directory before
 * returning. Recovery validates that entire prefix and discards only its
 * unacknowledged suffix. No recovery path treats corruption as an empty log.
 *
 * <p>The configured directory must already exist on a local filesystem with
 * directory force and atomic replacement support. There is one exclusive local
 * owner. This is not replication or distributed epoch fencing.
 *
 * <p>This class never updates a MemTable, index, row allocator, or transaction
 * outcome. discardAbortedTransaction must only be invoked after the caller has
 * verified an authoritative ABORT decision. Payloads remain on disk; only batch
 * descriptors and stream state are retained in memory. Rotation/reclamation are
 * not implemented: admission fails at the configured byte/record limits.
 */
public final class LocalMutationJournal implements Closeable
{
    static final int MAGIC = 0x50494D4A;
    static final int MARKER_MAGIC = 0x50494D44;
    static final int VERSION = 1;
    static final int HEADER_BYTES = 8;
    static final String WAL_NAME = "mutations.wal";
    static final String MARKER_NAME = "durable.offset";
    private static final int MARKER_BYTES = 20;
    private static final int MAX_FIXED_BODY_BYTES = 96;
    private static final int APPEND = 1;
    private static final int SEAL = 2;
    private static final int ABORT = 3;

    private final Path directory;
    private final Path markerPath;
    private final int maxPayloadBytes;
    private final long maxJournalBytes;
    private final int maxRecords;
    private final FileChannel channel;
    private final FileLock lock;
    private final Map<MutationStreamId, StreamState> streams = new HashMap<>();
    private final Set<Long> abortedTransactions = new HashSet<>();
    private long durableOffset;
    private int recordCount;
    private boolean failed;
    private boolean closed;

    public LocalMutationJournal(Path directory, int maxPayloadBytes,
                                long maxJournalBytes, int maxRecords) throws IOException
    {
        if (maxPayloadBytes <= 0 || maxPayloadBytes > Integer.MAX_VALUE - MAX_FIXED_BODY_BYTES - 8
                || maxJournalBytes < HEADER_BYTES || maxRecords <= 0)
        {
            throw new IllegalArgumentException("Invalid journal limits");
        }
        this.directory = directory.toRealPath();
        if (!Files.isDirectory(this.directory))
        {
            throw new IOException("Journal directory must already exist");
        }
        this.markerPath = this.directory.resolve(MARKER_NAME);
        this.maxPayloadBytes = maxPayloadBytes;
        this.maxJournalBytes = maxJournalBytes;
        this.maxRecords = maxRecords;
        Path walPath = this.directory.resolve(WAL_NAME);
        if (Files.exists(markerPath) && !Files.exists(walPath))
        {
            throw new IOException("Durable marker exists but WAL is missing");
        }
        this.channel = FileChannel.open(walPath, StandardOpenOption.CREATE,
                StandardOpenOption.READ, StandardOpenOption.WRITE);
        FileLock acquired = null;
        try
        {
            try
            {
                acquired = channel.tryLock();
            }
            catch (OverlappingFileLockException e)
            {
                throw new IOException("Journal already has an owner", e);
            }
            if (acquired == null)
            {
                throw new IOException("Journal already has an owner");
            }
            if (channel.size() == 0 && !Files.exists(markerPath))
            {
                ByteBuffer header = ByteBuffer.allocate(HEADER_BYTES).putInt(MAGIC).putInt(VERSION);
                header.flip();
                writeFully(channel, header);
                persistDurablePrefix();
            }
            else
            {
                recover();
            }
            this.lock = acquired;
        }
        catch (IOException | RuntimeException e)
        {
            if (acquired != null)
            {
                try { acquired.release(); } catch (IOException releaseError) { e.addSuppressed(releaseError); }
            }
            try { channel.close(); } catch (IOException closeError) { e.addSuppressed(closeError); }
            throw e;
        }
    }

    /** Return the original frame offset for identical retransmissions. */
    public synchronized long append(MutationBatch batch) throws IOException
    {
        ensureOpen();
        requireNotAborted(batch.getStreamId().getTransactionId());
        if (batch.getPayloadBytes() > maxPayloadBytes)
        {
            throw new IOException("Batch exceeds journal payload limit");
        }
        StreamState state = streams.get(batch.getStreamId());
        if (state != null && batch.getSequence() < state.entries.size())
        {
            Entry existing = state.entries.get((int) batch.getSequence());
            if (!Arrays.equals(existing.digest, batch.getDigest()))
            {
                throw new IOException("Conflicting content for batch " + batch.getStreamId()
                        + "/" + batch.getSequence());
            }
            return existing.offset;
        }
        validateNextBatch(state, batch);
        long offset = appendRecord(encodeBatch(batch));
        rememberBatch(batch, offset);
        return offset;
    }

    /** Seal only this stream; other writers in the transaction remain open. */
    public synchronized MutationStreamSeal seal(MutationStreamSeal expected) throws IOException
    {
        ensureOpen();
        requireNotAborted(expected.getStreamId().getTransactionId());
        StreamState state = requireStream(expected.getStreamId());
        if (!state.boundary(expected.getStreamId()).equals(expected))
        {
            throw new IOException("Stream seal does not match received batch sequence, totals, or digest");
        }
        if (state.seal == null)
        {
            appendRecord(encodeSeal(expected));
            state.seal = expected;
        }
        // Repeated seal also supplies a durability barrier after a retry.
        persistDurablePrefix();
        return state.seal;
    }

    /** Explicit local group-sync barrier; does not seal or commit any stream. */
    public synchronized void sync() throws IOException
    {
        ensureOpen();
        persistDurablePrefix();
    }

    public synchronized Optional<MutationStreamSeal> getSeal(MutationStreamId id) throws IOException
    {
        ensureOpen();
        requireNotAborted(id.getTransactionId());
        StreamState state = streams.get(id);
        return state == null ? Optional.empty() : Optional.ofNullable(state.seal);
    }

    /** Preparation-only read. Returned bytes are not public query state. */
    public synchronized MutationBatch readSealedBatch(MutationStreamId id, long sequence) throws IOException
    {
        ensureOpen();
        requireNotAborted(id.getTransactionId());
        StreamState state = requireStream(id);
        if (state.seal == null || sequence < 0 || sequence >= state.entries.size())
        {
            throw new IOException("Batch is not inside a sealed stream");
        }
        try
        {
            Entry entry = state.entries.get((int) sequence);
            DataInputStream input = new DataInputStream(new ByteArrayInputStream(readBody(entry.offset, durableOffset)));
            if (input.readUnsignedByte() != APPEND)
            {
                throw new IOException("Expected batch record");
            }
            MutationBatch batch = decodeBatch(input);
            requireEnd(input);
            if (!batch.getStreamId().equals(id) || batch.getSequence() != sequence
                    || !Arrays.equals(batch.getDigest(), entry.digest))
            {
                throw new IOException("Batch descriptor does not match durable bytes");
            }
            return batch;
        }
        catch (IOException | RuntimeException e)
        {
            failed = true;
            throw new IOException("Cannot read staged batch; journal is fail-closed", e);
        }
    }

    /**
     * Record a verified ABORT outcome and fence all streams of that transaction.
     * The caller supplies the decision; this method must not decide an outcome.
     */
    public synchronized void discardAbortedTransaction(long transactionId) throws IOException
    {
        ensureOpen();
        if (transactionId < 0)
        {
            throw new IllegalArgumentException("Negative transaction id");
        }
        if (!abortedTransactions.contains(transactionId))
        {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream out = new DataOutputStream(bytes);
            out.writeByte(ABORT);
            out.writeLong(transactionId);
            appendRecord(bytes.toByteArray());
            abortedTransactions.add(transactionId);
        }
        persistDurablePrefix();
    }

    public synchronized long getDurableOffset() throws IOException
    {
        ensureOpen();
        return durableOffset;
    }

    private void recover() throws IOException
    {
        if (!Files.exists(markerPath) || Files.size(markerPath) != MARKER_BYTES)
        {
            throw new IOException("Missing or malformed durable-prefix marker");
        }
        ByteBuffer marker = ByteBuffer.wrap(Files.readAllBytes(markerPath));
        if (marker.getInt() != MARKER_MAGIC || marker.getInt() != VERSION)
        {
            throw new IOException("Unsupported durable-prefix marker");
        }
        durableOffset = marker.getLong();
        if (marker.getInt() != checksum(marker.array(), 0, MARKER_BYTES - 4)
                || durableOffset < HEADER_BYTES || durableOffset > maxJournalBytes
                || durableOffset > channel.size())
        {
            throw new IOException("Invalid durable prefix or truncated acknowledged WAL");
        }
        ByteBuffer header = ByteBuffer.allocate(HEADER_BYTES);
        readFully(channel, header, 0);
        header.flip();
        if (header.getInt() != MAGIC || header.getInt() != VERSION)
        {
            throw new IOException("Unsupported WAL header");
        }
        long offset = HEADER_BYTES;
        try
        {
            while (offset < durableOffset)
            {
                byte[] body = readBody(offset, durableOffset);
                if (++recordCount > maxRecords)
                {
                    throw new IOException("Durable journal exceeds configured record limit");
                }
                DataInputStream in = new DataInputStream(new ByteArrayInputStream(body));
                int type = in.readUnsignedByte();
                if (type == APPEND)
                {
                    MutationBatch batch = decodeBatch(in);
                    requireNotAborted(batch.getStreamId().getTransactionId());
                    validateNextBatch(streams.get(batch.getStreamId()), batch);
                    rememberBatch(batch, offset);
                }
                else if (type == SEAL)
                {
                    MutationStreamSeal seal = decodeSeal(in);
                    requireNotAborted(seal.getStreamId().getTransactionId());
                    StreamState state = requireStream(seal.getStreamId());
                    if (state.seal != null || !state.boundary(seal.getStreamId()).equals(seal))
                    {
                        throw new IOException("Invalid durable stream seal");
                    }
                    state.seal = seal;
                }
                else if (type == ABORT)
                {
                    long txId = in.readLong();
                    if (txId < 0 || !abortedTransactions.add(txId))
                    {
                        throw new IOException("Invalid duplicate ABORT record");
                    }
                }
                else
                {
                    throw new IOException("Unknown WAL record type: " + type);
                }
                requireEnd(in);
                offset += 8L + body.length;
            }
        }
        catch (IllegalArgumentException | ArithmeticException e)
        {
            throw new IOException("Invalid durable WAL record", e);
        }
        // Only bytes not covered by an acknowledged sync may be discarded.
        if (channel.size() != durableOffset)
        {
            channel.truncate(durableOffset);
            channel.force(true);
        }
        channel.position(durableOffset);
    }

    private void persistDurablePrefix() throws IOException
    {
        try
        {
            long end = channel.position();
            channel.force(true);
            ByteBuffer marker = ByteBuffer.allocate(MARKER_BYTES);
            marker.putInt(MARKER_MAGIC).putInt(VERSION).putLong(end);
            marker.putInt(checksum(marker.array(), 0, MARKER_BYTES - 4));
            marker.flip();
            Path temporary = directory.resolve("durable.offset.tmp");
            try (FileChannel output = FileChannel.open(temporary, StandardOpenOption.CREATE,
                    StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE))
            {
                writeFully(output, marker);
                output.force(true);
            }
            Files.move(temporary, markerPath, StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
            try (FileChannel dir = FileChannel.open(directory, StandardOpenOption.READ))
            {
                dir.force(true);
            }
            durableOffset = end;
        }
        catch (IOException e)
        {
            failed = true;
            throw e;
        }
    }

    private long appendRecord(byte[] body) throws IOException
    {
        if (recordCount >= maxRecords || channel.position() > maxJournalBytes - 8L - body.length)
        {
            throw new IOException("Journal capacity exceeded; checkpoint/reclamation is required");
        }
        long offset = channel.position();
        ByteBuffer frame = ByteBuffer.allocate(body.length + 8);
        frame.putInt(body.length).putInt(checksum(body, 0, body.length)).put(body).flip();
        try
        {
            writeFully(channel, frame);
            recordCount++;
            return offset;
        }
        catch (IOException e)
        {
            failed = true;
            throw e;
        }
    }

    private byte[] readBody(long offset, long boundary) throws IOException
    {
        if (boundary - offset < 8)
        {
            throw new IOException("Truncated acknowledged frame header");
        }
        ByteBuffer header = ByteBuffer.allocate(8);
        readFully(channel, header, offset);
        header.flip();
        int length = header.getInt();
        int expectedChecksum = header.getInt();
        if (length <= 0 || length > maxPayloadBytes + MAX_FIXED_BODY_BYTES
                || length > boundary - offset - 8)
        {
            throw new IOException("Invalid or truncated acknowledged frame length");
        }
        byte[] body = new byte[length];
        readFully(channel, ByteBuffer.wrap(body), offset + 8);
        if (checksum(body, 0, body.length) != expectedChecksum)
        {
            throw new IOException("Checksum mismatch in acknowledged WAL frame");
        }
        return body;
    }

    private static byte[] encodeBatch(MutationBatch batch) throws IOException
    {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeByte(APPEND);
        writeId(out, batch.getStreamId());
        out.writeLong(batch.getSequence());
        out.writeLong(batch.getSchemaVersion());
        out.writeInt(batch.getPayloadFormat());
        out.writeInt(batch.getRowCount());
        out.writeInt(batch.getPayloadBytes());
        out.write(batch.getPayload());
        return bytes.toByteArray();
    }

    private MutationBatch decodeBatch(DataInputStream in) throws IOException
    {
        MutationStreamId id = readId(in);
        long sequence = in.readLong();
        long schema = in.readLong();
        int format = in.readInt();
        int rows = in.readInt();
        int length = in.readInt();
        if (length <= 0 || length > maxPayloadBytes || length != in.available())
        {
            throw new IOException("Invalid batch payload length");
        }
        byte[] payload = new byte[length];
        in.readFully(payload);
        return new MutationBatch(id, sequence, schema, format, rows, payload);
    }

    private static byte[] encodeSeal(MutationStreamSeal seal) throws IOException
    {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeByte(SEAL);
        writeId(out, seal.getStreamId());
        out.writeLong(seal.getBatchCount());
        out.writeLong(seal.getRowCount());
        out.writeLong(seal.getPayloadBytes());
        out.write(seal.getDigest());
        return bytes.toByteArray();
    }

    private static MutationStreamSeal decodeSeal(DataInputStream in) throws IOException
    {
        MutationStreamId id = readId(in);
        long batches = in.readLong();
        long rows = in.readLong();
        long bytes = in.readLong();
        byte[] digest = new byte[MutationBatch.DIGEST_BYTES];
        in.readFully(digest);
        return new MutationStreamSeal(id, batches, rows, bytes, digest);
    }

    private static void writeId(DataOutputStream out, MutationStreamId id) throws IOException
    {
        out.writeLong(id.getTransactionId());
        out.writeLong(id.getWriterId());
        out.writeLong(id.getTableId());
        out.writeInt(id.getShardId());
        out.writeInt(id.getKind().getCode());
    }

    private static MutationStreamId readId(DataInputStream in) throws IOException
    {
        return new MutationStreamId(in.readLong(), in.readLong(), in.readLong(),
                in.readInt(), MutationStreamId.Kind.fromCode(in.readInt()));
    }

    private static void requireEnd(DataInputStream in) throws IOException
    {
        if (in.available() != 0) { throw new IOException("Trailing bytes in WAL record"); }
    }

    private void validateNextBatch(StreamState state, MutationBatch batch) throws IOException
    {
        if (batch.getSequence() != (state == null ? 0 : state.entries.size()))
        {
            throw new IOException("Mutation stream sequence has a gap or duplicate");
        }
        if (state != null && (state.seal != null || state.schema != batch.getSchemaVersion()
                || state.format != batch.getPayloadFormat()))
        {
            throw new IOException("Stream is sealed or its pinned schema/format changed");
        }
    }

    private void rememberBatch(MutationBatch batch, long offset)
    {
        StreamState state = streams.computeIfAbsent(batch.getStreamId(),
                ignored -> new StreamState(batch.getSchemaVersion(), batch.getPayloadFormat()));
        state.entries.add(new Entry(offset, batch.getDigest()));
        state.rows = Math.addExact(state.rows, batch.getRowCount());
        state.bytes = Math.addExact(state.bytes, batch.getPayloadBytes());
        state.digest = MutationStreamSeal.extendDigest(state.digest, batch.getDigest());
    }

    private StreamState requireStream(MutationStreamId id) throws IOException
    {
        StreamState state = streams.get(id);
        if (state == null) { throw new IOException("Unknown mutation stream: " + id); }
        return state;
    }

    private void requireNotAborted(long txId) throws IOException
    {
        if (abortedTransactions.contains(txId)) { throw new IOException("Transaction was aborted: " + txId); }
    }

    private void ensureOpen() throws IOException
    {
        if (closed || failed) { throw new IOException("Journal is closed or failed; reopen for recovery"); }
    }

    private static int checksum(byte[] data, int offset, int length)
    {
        CRC32 crc = new CRC32();
        crc.update(data, offset, length);
        return (int) crc.getValue();
    }

    private static void writeFully(FileChannel output, ByteBuffer data) throws IOException
    {
        while (data.hasRemaining()) { output.write(data); }
    }

    private static void readFully(FileChannel input, ByteBuffer data, long offset) throws IOException
    {
        while (data.hasRemaining())
        {
            int read = input.read(data, offset);
            if (read < 0) { throw new IOException("Unexpected end of WAL"); }
            offset += read;
        }
    }

    /** Close does not acknowledge or checkpoint unsealed appends. */
    @Override
    public synchronized void close() throws IOException
    {
        if (closed) { return; }
        closed = true;
        try { lock.release(); }
        finally { channel.close(); }
    }

    private static final class Entry
    {
        final long offset;
        final byte[] digest;
        Entry(long offset, byte[] digest) { this.offset = offset; this.digest = digest; }
    }

    private static final class StreamState
    {
        final long schema;
        final int format;
        final List<Entry> entries = new ArrayList<>();
        long rows;
        long bytes;
        byte[] digest = MutationStreamSeal.emptyDigest();
        MutationStreamSeal seal;

        StreamState(long schema, int format) { this.schema = schema; this.format = format; }

        MutationStreamSeal boundary(MutationStreamId id)
        {
            return new MutationStreamSeal(id, entries.size(), rows, bytes, digest);
        }
    }
}
