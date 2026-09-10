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

import static org.junit.jupiter.api.Assertions.*;

import io.grpc.*;
import io.grpc.stub.StreamObserver;
import io.pixelsdb.pixels.common.index.MainIndexFactory;
import io.pixelsdb.pixels.common.index.service.*;
import io.pixelsdb.pixels.common.ingest.*;
import io.pixelsdb.pixels.common.ingest.durable.AtomicStateFile;
import io.pixelsdb.pixels.common.ingest.rpc.*;
import io.pixelsdb.pixels.common.ingest.wire.*;
import io.pixelsdb.pixels.common.metadata.MetadataService;
import io.pixelsdb.pixels.common.physical.StorageFactory;
import io.pixelsdb.pixels.common.utils.ConfigFactory;
import io.pixelsdb.pixels.core.*;
import io.pixelsdb.pixels.core.ingest.IngestTables;
import io.pixelsdb.pixels.core.reader.*;
import io.pixelsdb.pixels.core.vector.VectorizedRowBatch;
import io.pixelsdb.pixels.daemon.*;
import io.pixelsdb.pixels.index.IndexProto;
import io.pixelsdb.pixels.ingest.IngestProto.*;
import io.pixelsdb.pixels.retina.*;
import io.pixelsdb.pixels.retina.ingest.*;

import org.junit.jupiter.api.Test;

import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Real buffers, native visibility, SQLite MainIndex, local objects and Pixels files.
 * Catalog/node RPCs and the etcd-backed allocation source are isolated test doubles.
 */
public class TestPixelsIngestStorage {
    private static <T> void reply(StreamObserver<T> out, T value) {
        out.onNext(value);
        out.onCompleted();
    }

    private static MetadataProto.ResponseHeader ok(MetadataProto.RequestHeader request) {
        return MetadataProto.ResponseHeader.newBuilder().setToken(request.getToken()).build();
    }

    static class Catalog extends MetadataServiceGrpc.MetadataServiceImplBase {
        final Map<Long, MetadataProto.File> files = new ConcurrentHashMap<>();
        final AtomicLong ids = new AtomicLong(100);
        final AtomicBoolean rejectPublication = new AtomicBoolean(false);
        final MetadataProto.Layout layout;

        Catalog(Path root) {
            layout =
                    MetadataProto.Layout.newBuilder()
                            .setId(1)
                            .setTableId(73)
                            .setSchemaVersionId(1)
                            .setVersion(1)
                            .setPermission(MetadataProto.Permission.READ_WRITE)
                            .setOrdered("{\"columnOrder\":[\"v\"]}")
                            .setCompact("{}")
                            .setSplits("{}")
                            .setProjections("{}")
                            .addOrderedPaths(
                                    MetadataProto.Path.newBuilder()
                                            .setId(1)
                                            .setLayoutId(1)
                                            .setUri(root.resolve("ordered").toUri().toString())
                                            .setType(MetadataProto.Path.Type.ORDERED))
                            .addCompactPaths(
                                    MetadataProto.Path.newBuilder()
                                            .setId(2)
                                            .setLayoutId(1)
                                            .setUri(root.resolve("compact").toUri().toString())
                                            .setType(MetadataProto.Path.Type.COMPACT))
                            .build();
        }

        public void getTable(
                MetadataProto.GetTableRequest r, StreamObserver<MetadataProto.GetTableResponse> o) {
            reply(
                    o,
                    MetadataProto.GetTableResponse.newBuilder()
                            .setHeader(ok(r.getHeader()))
                            .setTable(
                                    MetadataProto.Table.newBuilder()
                                            .setId(73)
                                            .setName("t")
                                            .setType("user")
                                            .setSchemaId(1)
                                            .setStorageScheme("file"))
                            .addLayouts(layout)
                            .build());
        }

        public void getColumns(
                MetadataProto.GetColumnsRequest r,
                StreamObserver<MetadataProto.GetColumnsResponse> o) {
            reply(
                    o,
                    MetadataProto.GetColumnsResponse.newBuilder()
                            .setHeader(ok(r.getHeader()))
                            .addColumns(
                                    MetadataProto.Column.newBuilder()
                                            .setId(1)
                                            .setTableId(73)
                                            .setName("v")
                                            .setType("varbinary"))
                            .build());
        }

        public void getLayout(
                MetadataProto.GetLayoutRequest r,
                StreamObserver<MetadataProto.GetLayoutResponse> o) {
            reply(
                    o,
                    MetadataProto.GetLayoutResponse.newBuilder()
                            .setHeader(ok(r.getHeader()))
                            .setLayout(layout)
                            .build());
        }

        public void getSinglePointIndices(
                MetadataProto.GetSinglePointIndicesRequest r,
                StreamObserver<MetadataProto.GetSinglePointIndicesResponse> o) {
            reply(
                    o,
                    MetadataProto.GetSinglePointIndicesResponse.newBuilder()
                            .setHeader(ok(r.getHeader()))
                            .build());
        }

        public void addFiles(
                MetadataProto.AddFilesRequest r, StreamObserver<MetadataProto.AddFilesResponse> o) {
            for (MetadataProto.File f : r.getFilesList()) {
                long id = ids.incrementAndGet();
                files.put(id, f.toBuilder().setId(id).build());
            }
            reply(
                    o,
                    MetadataProto.AddFilesResponse.newBuilder()
                            .setHeader(ok(r.getHeader()))
                            .build());
        }

        public void getFileId(
                MetadataProto.GetFileIdRequest r,
                StreamObserver<MetadataProto.GetFileIdResponse> o) {
            long id =
                    files.values().stream()
                            .filter(f -> r.getFilePathUri().endsWith("/" + f.getName()))
                            .findFirst()
                            .orElseThrow(() -> new IllegalArgumentException("missing file"))
                            .getId();
            reply(
                    o,
                    MetadataProto.GetFileIdResponse.newBuilder()
                            .setHeader(ok(r.getHeader()))
                            .setFileId(id)
                            .build());
        }

        public void getFileById(
                MetadataProto.GetFileByIdRequest r,
                StreamObserver<MetadataProto.GetFileByIdResponse> o) {
            MetadataProto.GetFileByIdResponse.Builder b =
                    MetadataProto.GetFileByIdResponse.newBuilder().setHeader(ok(r.getHeader()));
            if (files.containsKey(r.getFileId())) {
                b.setFile(files.get(r.getFileId()));
            }
            reply(o, b.build());
        }

        public void getFilesByType(
                MetadataProto.GetFilesByTypeRequest r,
                StreamObserver<MetadataProto.GetFilesByTypeResponse> o) {
            MetadataProto.GetFilesByTypeResponse.Builder b =
                    MetadataProto.GetFilesByTypeResponse.newBuilder().setHeader(ok(r.getHeader()));
            files.values().stream()
                    .filter(
                            f ->
                                    r.getFileTypesList().contains(f.getType())
                                            && (!r.hasPathId() || r.getPathId() == f.getPathId()))
                    .forEach(b::addFiles);
            reply(o, b.build());
        }

        public void updateFile(
                MetadataProto.UpdateFileRequest r,
                StreamObserver<MetadataProto.UpdateFileResponse> o) {
            if (rejectPublication.get()
                    && r.getFile().getType() == MetadataProto.File.Type.REGULAR) {
                o.onError(
                        Status.UNAVAILABLE
                                .withDescription("injected catalog publication failure")
                                .asRuntimeException());
                return;
            }
            files.put(r.getFile().getId(), r.getFile());
            reply(
                    o,
                    MetadataProto.UpdateFileResponse.newBuilder()
                            .setHeader(ok(r.getHeader()))
                            .build());
        }

        public void deleteFiles(
                MetadataProto.DeleteFilesRequest r,
                StreamObserver<MetadataProto.DeleteFilesResponse> o) {
            r.getFileIdsList().forEach(files::remove);
            reply(
                    o,
                    MetadataProto.DeleteFilesResponse.newBuilder()
                            .setHeader(ok(r.getHeader()))
                            .build());
        }
    }

    @Test
    public void realBufferInstallationIsIdempotentAndCoalescesKeylessRows() throws Exception {
        Path root = Files.createTempDirectory("pixels-ingest-storage-");
        Files.createDirectories(root.resolve("ordered"));
        Files.createDirectories(root.resolve("compact"));
        Path secret = root.resolve("credential");
        Files.write(
                secret,
                "test-ingest-storage-secret-01234567890"
                        .getBytes(java.nio.charset.StandardCharsets.UTF_8));
        ConfigFactory config = ConfigFactory.Instance();
        Map<String, String> changes = new LinkedHashMap<>();
        changes.put("retina.enable", "true");
        changes.put("retina.ingest.enabled", "true");
        changes.put("retina.ingest.auth.secret.file", secret.toString());
        changes.put("retina.storage.gc.enabled", "false");
        changes.put("retina.buffer.memTable.size", "64");
        changes.put("retina.buffer.flush.count", "2");
        changes.put("retina.buffer.flush.interval", "1");
        changes.put(
                "retina.buffer.object.storage.folder", root.resolve("objects").toUri().toString());
        changes.put("retina.storage.gc.journal.dir", root.resolve("gc").toUri().toString());
        changes.put("retina.offload.checkpoint.dir", root.resolve("offload").toUri().toString());
        changes.put("index.sqlite.path", root.resolve("sqlite").toString());
        changes.put("enabled.storage.schemes", "file");
        changes.put("node.bucket.num", "1");
        changes.put("node.virtual.num", "1");
        changes.put("index.bucket.num", "1");
        changes.put("index.cache.enabled", "false");
        changes.put("cache.enabled", "false");
        Map<String, String> previous = new HashMap<>();
        changes.forEach(
                (k, v) -> {
                    previous.put(k, config.getProperty(k));
                    config.addProperty(k, v);
                });
        Catalog catalog = new Catalog(root);
        Server meta = ServerBuilder.forPort(0).addService(catalog).build().start();
        NodeServiceGrpc.NodeServiceImplBase node =
                new NodeServiceGrpc.NodeServiceImplBase() {
                    public void getRetinaByBucket(
                            NodeProto.GetRetinaByBucketRequest r,
                            StreamObserver<NodeProto.GetRetinaByBucketResponse> o) {
                        reply(
                                o,
                                NodeProto.GetRetinaByBucketResponse.newBuilder()
                                        .setNode(
                                                NodeProto.NodeInfo.newBuilder()
                                                        .setAddress("127.0.0.1")
                                                        .setPort(18890)
                                                        .setVirtualNodeId(0))
                                        .build());
                    }
                };
        Server nodes = ServerBuilder.forPort(0).addService(node).build().start();
        config.addProperty("metadata.server.host", "127.0.0.1");
        config.addProperty("metadata.server.port", Integer.toString(meta.getPort()));
        config.addProperty("node.server.host", "127.0.0.1");
        config.addProperty("node.server.port", Integer.toString(nodes.getPort()));
        PixelsWriteBuffer buffer = null;
        PixelsIngestInstaller installer = null;
        try {
            RetinaResourceManager resources = RetinaResourceManager.Instance();
            resources.getIngestReadPins().ready();
            ReadPin pin =
                    resources
                            .getIngestReadPins()
                            .pin(
                                    ReadPin.newBuilder()
                                            .setTransactionId(99)
                                            .setReadTimestamp(0)
                                            .build());
            TableSpec table = IngestTables.load("s", "t");
            assertEquals(0, table.getIndexesCount());
            resources.addWriteBuffer("s", "t");
            buffer = resources.getIngestBuffer("s", "t", 0);
            AtomicLong allocation = new AtomicLong(1000),
                    allocationCalls = new AtomicLong(),
                    putCalls = new AtomicLong();
            AtomicBoolean failOnce = new AtomicBoolean(true);
            IndexService delegate = LocalIndexService.Instance();
            IndexService index =
                    (IndexService)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {IndexService.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("allocateRowIdBatch")) {
                                            int count = (Integer) args[1];
                                            allocationCalls.incrementAndGet();
                                            return IndexProto.RowIdBatch.newBuilder()
                                                    .setRowIdStart(allocation.getAndAdd(count))
                                                    .setLength(count)
                                                    .build();
                                        }
                                        try {
                                            Object result = method.invoke(delegate, args);
                                            if (method.getName()
                                                    .equals("putMainIndexEntriesOnly")) {
                                                putCalls.incrementAndGet();
                                                if (failOnce.compareAndSet(true, false)) {
                                                    throw new io.pixelsdb.pixels.common.exception
                                                            .IndexException(
                                                            "after real MainIndex write");
                                                }
                                            }
                                            return result;
                                        } catch (InvocationTargetException e) {
                                            throw e.getCause();
                                        }
                                    });
            installer =
                    new PixelsIngestInstaller(
                            new AtomicStateFile(
                                    Files.createDirectory(root.resolve("plans")), 16 * 1024 * 1024),
                            new IngestOptions(),
                            "127.0.0.1:18890",
                            resources,
                            index,
                            MetadataService.Instance());
            List<byte[][]> rows = Collections.nCopies(250, new byte[][] {new byte[] {7}});
            byte[] payload = ColumnBatchCodec.encode(rows, 1, 1024 * 1024);
            MutationBatch batch =
                    new MutationBatch(
                            new MutationStreamId(100, 1, 73, 0, MutationStreamId.Kind.APPEND_ROWS),
                            0,
                            table.getSchemaVersion(),
                            ColumnBatchCodec.FORMAT,
                            rows.size(),
                            payload);
            Transaction tx =
                    Transaction.newBuilder()
                            .setTransactionId(100)
                            .setTable(table)
                            .setCommitTimestamp(200)
                            .setState(TransactionState.COMMIT_DECIDED)
                            .build();
            installer.prepare(tx, Collections.singletonList(batch));
            assertEquals(0, catalog.files.size());
            PixelsIngestInstaller target = installer;
            assertThrows(
                    io.pixelsdb.pixels.common.exception.IndexException.class,
                    () -> target.install(tx, Collections.singletonList(batch), false));
            installer.install(tx, Collections.singletonList(batch), false);
            installer.install(tx, Collections.singletonList(batch), false);
            assertEquals(
                    1, allocationCalls.get(), "Replay must reuse the recorded allocator result");
            for (long id = 1000; id < 1250; id++) {
                assertNotNull(MainIndexFactory.Instance().getMainIndex(73).getLocation(id));
            }
            assertEquals(
                    2, catalog.files.size(), "Rows, not statement boundaries, roll shared files");
            assertTrue(
                    catalog.files.values().stream()
                            .allMatch(
                                    f -> f.getType() == MetadataProto.File.Type.TEMPORARY_INGEST));
            long visible = bufferedRows(buffer);
            assertEquals(250, visible);
            // A second transaction fills the last block and starts the next generation.
            MutationBatch second =
                    new MutationBatch(
                            new MutationStreamId(101, 1, 73, 0, MutationStreamId.Kind.APPEND_ROWS),
                            0,
                            table.getSchemaVersion(),
                            ColumnBatchCodec.FORMAT,
                            10,
                            ColumnBatchCodec.encode(
                                    Collections.nCopies(10, new byte[][] {new byte[] {7}}),
                                    1,
                                    1024 * 1024));
            Transaction next = tx.toBuilder().setTransactionId(101).setCommitTimestamp(201).build();
            installer.prepare(next, Collections.singletonList(second));
            installer.install(next, Collections.singletonList(second), false);
            assertEquals(260, bufferedRows(buffer));
            assertEquals(2, allocationCalls.get());
            // Query the real read overlay before files exist. Use bitmap identities
            // from the same captured version, including objects still spilling.
            assertEquals(250, readBuffered(resources, root, 200));
            IndexProto.RowLocation deleted =
                    MainIndexFactory.Instance().getMainIndex(73).getLocation(1000);
            resources.deleteRecord(deleted, 200);
            assertEquals(249, readBuffered(resources, root, 200));
            assertEquals(259, readBuffered(resources, root, 201));
            assertEquals(0, readBuffered(resources, root, 199));
            long priorPuts = putCalls.get();
            catalog.rejectPublication.set(true);
            resources.getIngestReadPins().release(pin);
            // Wait until the actual SQLite flush is durable, while metadata publication fails.
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
            while (System.nanoTime() < deadline && !hasFlushedFile(root.resolve("sqlite"))) {
                Thread.sleep(25);
            }
            assertTrue(
                    hasFlushedFile(root.resolve("sqlite")),
                    "MainIndex per-file marker must be persisted");
            installer.install(tx, Collections.singletonList(batch), false);
            assertEquals(
                    priorPuts,
                    putCalls.get(),
                    "A catalog retry must not re-put already flushed MainIndex entries");
            catalog.rejectPublication.set(false);
            deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
            while (System.nanoTime() < deadline
                    && catalog.files.values().stream()
                                    .filter(f -> f.getType() == MetadataProto.File.Type.REGULAR)
                                    .count()
                            < 2) {
                Thread.sleep(25);
            }
            List<MetadataProto.File> regular = new ArrayList<>();
            catalog.files.values().stream()
                    .filter(f -> f.getType() == MetadataProto.File.Type.REGULAR)
                    .forEach(regular::add);
            assertEquals(2, regular.size());
            int materialized = 0;
            for (MetadataProto.File file : regular) {
                String path = root.resolve("ordered").resolve(file.getName()).toUri().toString();
                try (PixelsReader reader =
                        PixelsReaderImpl.newBuilder()
                                .setStorage(StorageFactory.Instance().getStorage(path))
                                .setPath(path)
                                .setPixelsFooterCache(new PixelsFooterCache())
                                .build()) {
                    PixelsReaderOption option = new PixelsReaderOption();
                    option.includeCols(new String[] {"v"});
                    try (PixelsRecordReader records = reader.read(option)) {
                        VectorizedRowBatch data;
                        while ((data = records.readBatch()) != null && data.size > 0) {
                            materialized += data.size;
                        }
                    }
                }
            }
            assertEquals(256, materialized);
            assertEquals(4, bufferedRows(buffer));
            assertEquals(260, materialized + bufferedRows(buffer));
        } finally {
            if (installer != null) {
                installer.close();
            }
            if (buffer != null) {
                buffer.close();
            }
            nodes.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
            meta.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
            previous.forEach(
                    (k, v) -> {
                        if (v != null) {
                            config.addProperty(k, v);
                        }
                    });
        }
    }

    static long readBuffered(RetinaResourceManager resources, Path root, long timestamp)
            throws Exception {
        RetinaProto.GetWriteBufferResponse response =
                resources.getWriteBuffer("s", "t", timestamp, 0).build();
        PixelsReaderOption option = new PixelsReaderOption();
        option.includeCols(new String[] {"v"});
        option.transTimestamp(timestamp);
        String folder = root.resolve("objects").toUri().toString();
        long count = 0;
        try (PixelsRecordReaderBufferImpl reader =
                new PixelsRecordReaderBufferImpl(
                        option,
                        io.pixelsdb.pixels.common.utils.NetUtils.getLocalHostName(),
                        response.getData().toByteArray(),
                        response.getIdsList(),
                        response.getBitmapsList(),
                        StorageFactory.Instance().getStorage(folder),
                        73,
                        0,
                        TypeDescription.fromString("struct<v:varbinary>"))) {
            while (!reader.isEndOfFile()) {
                VectorizedRowBatch batch = reader.readBatch();
                count += batch.size;
            }
        }
        return count;
    }

    static long bufferedRows(PixelsWriteBuffer buffer) {
        SuperVersion view = buffer.getCurrentVersion();
        try {
            long rows = view.getActiveMemTable() == null ? 0 : view.getActiveMemTable().getSize();
            for (MemTable mem : view.getImmutableMemTables()) {
                rows += mem.getSize();
            }
            for (ObjectEntry object : view.getObjectEntries()) {
                rows += object.getLength();
            }
            return rows;
        } finally {
            view.unref();
        }
    }

    static boolean hasFlushedFile(Path dir) throws Exception {
        try (java.util.stream.Stream<Path> paths = Files.walk(dir)) {
            for (Path path :
                    (Iterable<Path>)
                            paths.filter(
                                            p ->
                                                    p.toString().endsWith(".db")
                                                            || p.toString().endsWith(".sqlite"))
                                    ::iterator) {
                try (java.sql.Connection c =
                                java.sql.DriverManager.getConnection("jdbc:sqlite:" + path);
                        java.sql.Statement s = c.createStatement();
                        java.sql.ResultSet r =
                                s.executeQuery("select count(*) from row_id_range_flush_markers")) {
                    if (r.next() && r.getLong(1) > 0) {
                        return true;
                    }
                }
            }
        }
        return false;
    }
}
