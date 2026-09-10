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

import com.google.protobuf.ByteString;

import io.pixelsdb.pixels.common.ingest.*;
import io.pixelsdb.pixels.ingest.IngestProto.*;

import java.io.*;
import java.security.*;
import java.util.*;

/** Wire conversions preserve identities and verify digests before accepting input. */
public final class IngestWire {
    private IngestWire() {}

    public static StreamId encode(MutationStreamId s) {
        return StreamId.newBuilder()
                .setTransactionId(s.getTransactionId())
                .setWriterId(s.getWriterId())
                .setTableId(s.getTableId())
                .setShardId(s.getShardId())
                .setKindValue(s.getKind().getCode())
                .build();
    }

    public static MutationStreamId decode(StreamId s) {
        return new MutationStreamId(
                s.getTransactionId(),
                s.getWriterId(),
                s.getTableId(),
                s.getShardId(),
                MutationStreamId.Kind.fromCode(s.getKindValue()));
    }

    public static StreamSeal encode(MutationStreamSeal s) {
        return StreamSeal.newBuilder()
                .setStream(encode(s.getStreamId()))
                .setBatchCount(s.getBatchCount())
                .setRowCount(s.getRowCount())
                .setPayloadBytes(s.getPayloadBytes())
                .setDigest(ByteString.copyFrom(s.getDigest()))
                .build();
    }

    public static MutationStreamSeal decode(StreamSeal s) {
        return new MutationStreamSeal(
                decode(s.getStream()),
                s.getBatchCount(),
                s.getRowCount(),
                s.getPayloadBytes(),
                s.getDigest().toByteArray());
    }

    public static AppendRequest encode(MutationBatch b) {
        return AppendRequest.newBuilder()
                .setStream(encode(b.getStreamId()))
                .setSequence(b.getSequence())
                .setSchemaVersion(b.getSchemaVersion())
                .setPayloadFormat(b.getPayloadFormat())
                .setRowCount(b.getRowCount())
                .setPayload(ByteString.copyFrom(b.getPayload()))
                .setDigest(ByteString.copyFrom(b.getDigest()))
                .build();
    }

    public static MutationBatch decode(AppendRequest r) throws IOException {
        MutationBatch b =
                new MutationBatch(
                        decode(r.getStream()),
                        r.getSequence(),
                        r.getSchemaVersion(),
                        r.getPayloadFormat(),
                        r.getRowCount(),
                        r.getPayload().toByteArray());
        if (!MessageDigest.isEqual(b.getDigest(), r.getDigest().toByteArray()))
            throw new IOException("Batch digest mismatch");
        return b;
    }

    public static TransactionId id(long id) {
        return TransactionId.newBuilder().setTransactionId(id).build();
    }

    public static String owner(Route r) {
        return r.getHost() + ":" + r.getPort();
    }

    public static Route route(TableSpec t, int shard) throws IOException {
        for (Route r : t.getRoutesList()) if (r.getShardId() == shard) return r;
        throw new IOException("Unmapped shard " + shard);
    }

    public static boolean committed(Transaction t) {
        return t.getState() == TransactionState.COMMIT_DECIDED
                || t.getState() == TransactionState.PUBLISHED;
    }

    public static Set<String> owners(Transaction t) {
        Set<String> out = new TreeSet<>();
        for (StreamId s : t.getStreamsList()) {
            try {
                out.add(owner(route(t.getTable(), s.getShardId())));
            } catch (IOException e) {
                throw new IllegalArgumentException(e);
            }
        }
        return out;
    }

    public static List<StreamSeal> localSeals(Transaction t, String owner) throws IOException {
        List<StreamSeal> out = new ArrayList<>();
        for (StreamSeal s : t.getSealsList())
            if (owner(route(t.getTable(), s.getStream().getShardId())).equals(owner)) out.add(s);
        return out;
    }

    public static byte[] prepareDigest(Transaction t, String owner) throws IOException {
        try {
            MessageDigest d = MessageDigest.getInstance("SHA-256");
            d.update(t.getTable().toByteArray());
            for (StreamSeal s : localSeals(t, owner)) {
                byte[] b = s.toByteArray();
                d.update(java.nio.ByteBuffer.allocate(4).putInt(b.length).array());
                d.update(b);
            }
            return d.digest();
        } catch (NoSuchAlgorithmException e) {
            throw new AssertionError(e);
        }
    }

    public static String batchKey(MutationStreamId s, long seq) {
        return s.getTransactionId()
                + ":"
                + s.getWriterId()
                + ":"
                + s.getTableId()
                + ":"
                + s.getShardId()
                + ":"
                + s.getKind().getCode()
                + ":"
                + seq;
    }
}
