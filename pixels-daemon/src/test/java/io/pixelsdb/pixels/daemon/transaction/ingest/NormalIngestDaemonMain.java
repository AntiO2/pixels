/*
 * Copyright 2026 PixelsDB.
 *
 * This file is part of Pixels and is licensed under the GNU Affero General
 * Public License, version 3 or (at your option) any later version.
 */
package io.pixelsdb.pixels.daemon.transaction.ingest;

import com.google.protobuf.Empty;
import io.grpc.ManagedChannel;
import io.grpc.ManagedChannelBuilder;
import io.grpc.Server;
import io.grpc.ServerBuilder;
import io.grpc.stub.StreamObserver;
import io.pixelsdb.pixels.common.ingest.MutationBatch;
import io.pixelsdb.pixels.common.ingest.MutationStreamId;
import io.pixelsdb.pixels.common.ingest.MutationStreamSeal;
import io.pixelsdb.pixels.common.ingest.durable.AtomicStateFile;
import io.pixelsdb.pixels.common.ingest.rpc.IngestClient;
import io.pixelsdb.pixels.common.ingest.rpc.IngestOptions;
import io.pixelsdb.pixels.common.ingest.wire.ColumnBatchCodec;
import io.pixelsdb.pixels.common.ingest.wire.IngestWire;
import io.pixelsdb.pixels.common.physical.StorageFactory;
import io.pixelsdb.pixels.common.utils.ConfigFactory;
import io.pixelsdb.pixels.common.utils.Constants;
import io.pixelsdb.pixels.common.utils.EtcdUtil;
import io.pixelsdb.pixels.common.utils.NetUtils;
import io.pixelsdb.pixels.core.PixelsFooterCache;
import io.pixelsdb.pixels.core.PixelsReader;
import io.pixelsdb.pixels.core.PixelsReaderImpl;
import io.pixelsdb.pixels.core.reader.PixelsReaderOption;
import io.pixelsdb.pixels.core.reader.PixelsRecordReader;
import io.pixelsdb.pixels.core.vector.VectorizedRowBatch;
import io.pixelsdb.pixels.daemon.MetadataProto;
import io.pixelsdb.pixels.daemon.NodeProto;
import io.pixelsdb.pixels.daemon.NodeServiceGrpc;
import io.pixelsdb.pixels.daemon.ServerContainer;
import io.pixelsdb.pixels.daemon.retina.RetinaServer;
import io.pixelsdb.pixels.daemon.transaction.TransServer;
import io.pixelsdb.pixels.ingest.IngestProto.AllocateWriterRequest;
import io.pixelsdb.pixels.ingest.IngestProto.BeginWriteRequest;
import io.pixelsdb.pixels.ingest.IngestProto.InstallationSnapshot;
import io.pixelsdb.pixels.ingest.IngestProto.PrepareWriteRequest;
import io.pixelsdb.pixels.ingest.IngestProto.ReadPin;
import io.pixelsdb.pixels.ingest.IngestProto.Transaction;
import io.pixelsdb.pixels.ingest.IngestProto.TransactionState;
import io.pixelsdb.pixels.ingest.IngestProto.WriterAssignment;
import io.pixelsdb.pixels.retina.RetinaProto;
import io.pixelsdb.pixels.retina.RetinaWorkerServiceGrpc;
import io.pixelsdb.pixels.retina.ingest.LocalMutationJournal;

import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.UUID;
import java.util.concurrent.TimeUnit;

/**
 * Process-level verification of the production daemon service classes.
 *
 * <p>Only metadata catalog and topology discovery are fixtures. Transaction identity comes from
 * the separately launched real etcd process; TransServer, RetinaServer, ServerContainer, RPC,
 * journal, installer, buffer, SQLite MainIndex, visibility and Pixels file materialization are
 * production implementations.</p>
 */
public final class NormalIngestDaemonMain
{
    private static final long CUTOVER_BASELINE = 1_000_000_000L;

    private NormalIngestDaemonMain() {}

    public static void main(String[] args)
    {
        int exit = 0;
        try
        {
            if (args.length != 5)
            {
                throw new IllegalArgumentException(
                        "usage: ROOT ETCD_PORT RETINA_PORT TRANSACTION_PORT "
                                + "write|recover|cutover-reject|cutover");
            }
            run(Paths.get(args[0]), Integer.parseInt(args[1]),
                    Integer.parseInt(args[2]), Integer.parseInt(args[3]), args[4]);
        }
        catch (Throwable failure)
        {
            failure.printStackTrace(System.err);
            exit = 1;
        }
        System.exit(exit);
    }

    private static void run(Path root, int etcdPort, int retinaPort,
                            int transactionPort, String phase) throws Exception
    {
        Files.createDirectories(root);
        Files.createDirectories(root.resolve("ordered"));
        Files.createDirectories(root.resolve("compact"));
        String host = NetUtils.getLocalHostName();
        String owner = host + ":" + retinaPort;
        Path secretFile = root.resolve("credential");
        String secret;
        if (Files.exists(secretFile))
        {
            secret = new String(Files.readAllBytes(secretFile), StandardCharsets.UTF_8).trim();
        }
        else
        {
            secret = "normal-daemon-" + UUID.randomUUID();
            Files.write(secretFile, secret.getBytes(StandardCharsets.UTF_8));
        }

        ConfigFactory config = ConfigFactory.Instance();
        setting(config, "retina.enable", "true");
        setting(config, "retina.ingest.enabled", "true");
        setting(config, "retina.ingest.auth.secret.file", secretFile.toString());
        setting(config, "retina.ingest.coordinator.state.dir", root.resolve("decisions").toString());
        setting(config, "retina.ingest.participant.plan.dir", root.resolve("plans").toString());
        setting(config, "retina.ingest.participant.wal.dir", root.resolve("wal").toString());
        boolean cutover = phase.equals("cutover") || phase.equals("cutover-reject");
        setting(config, "retina.ingest.cutover.baseline.timestamp",
                Long.toString(cutover ? CUTOVER_BASELINE : 0));
        setting(config, "retina.ingest.transaction.lease.ms", "30000");
        setting(config, "retina.ingest.terminal.retention.ms", "30000");
        setting(config, "retina.server.host", host);
        setting(config, "retina.server.port", Integer.toString(retinaPort));
        setting(config, "trans.server.host", "127.0.0.1");
        setting(config, "trans.server.port", Integer.toString(transactionPort));
        setting(config, "retina.ingest.coordinator.host", "127.0.0.1");
        setting(config, "retina.ingest.coordinator.port", Integer.toString(transactionPort));
        setting(config, "etcd.hosts", "127.0.0.1");
        setting(config, "etcd.port", Integer.toString(etcdPort));
        if (phase.equals("cutover"))
        {
            // The supported operator procedure performs a guarded/CAS advance while all
            // writers are drained. This isolated etcd has no concurrent writer, so a direct
            // assignment models the already-completed administrative step.
            EtcdUtil.Instance().putKeyValue(
                    Constants.AI_TRANS_ID_KEY, Long.toString(CUTOVER_BASELINE + 1));
        }
        setting(config, "retina.storage.gc.enabled", "false");
        setting(config, "retina.gc.interval", "1");
        setting(config, "retina.buffer.memTable.size", "64");
        setting(config, "retina.buffer.flush.count", "1");
        setting(config, "retina.buffer.flush.interval", "1");
        setting(config, "retina.buffer.object.storage.folder", root.resolve("objects").toUri().toString());
        setting(config, "retina.storage.gc.journal.dir", root.resolve("gc").toUri().toString());
        setting(config, "retina.offload.checkpoint.dir", root.resolve("offload").toUri().toString());
        setting(config, "retina.recovery.checkpoint.dir", root.resolve("recovery").toUri().toString());
        setting(config, "index.sqlite.path", root.resolve("sqlite").toString());
        setting(config, "enabled.storage.schemes", "file");
        setting(config, "node.bucket.num", "1");
        setting(config, "node.virtual.num", "1");
        setting(config, "index.bucket.num", "1");
        setting(config, "index.cache.enabled", "false");
        setting(config, "cache.enabled", "false");
        setting(config, "projection.read.enabled", "false");
        setting(config, "fixed.split.size", "1");
        setting(config, "scaling.enabled", "false");
        setting(config, "retina.buffer.split.enable", "true");

        if (!phase.equals("write") && !phase.equals("recover")
                && !phase.equals("cutover-reject") && !phase.equals("cutover"))
        {
            throw new IllegalArgumentException("Unknown daemon verification phase: " + phase);
        }
        PhaseState recoveredState = phase.equals("write") ? null : loadPhaseState(root);
        if (phase.equals("recover"))
        {
            verifyCheckpointReclamation(root, recoveredState);
        }

        SqlIngestFixture.Catalog catalog = new SqlIngestFixture.Catalog(root);
        if (recoveredState != null && countPublishedRows(catalog, root) != 65)
        {
            throw new AssertionError("phase-one files do not contain exactly 65 rows");
        }
        Server metadata = ServerBuilder.forPort(0).addService(catalog).build().start();
        setting(config, "metadata.server.host", "127.0.0.1");
        setting(config, "metadata.server.port", Integer.toString(metadata.getPort()));

        NodeProto.NodeInfo node = NodeProto.NodeInfo.newBuilder()
                .setAddress(host).setPort(retinaPort).setVirtualNodeId(0).build();
        Server topology = ServerBuilder.forPort(0).addService(new NodeServiceGrpc.NodeServiceImplBase()
        {
            @Override
            public void getRetinaByBucket(NodeProto.GetRetinaByBucketRequest request,
                                          StreamObserver<NodeProto.GetRetinaByBucketResponse> observer)
            {
                reply(observer, NodeProto.GetRetinaByBucketResponse.newBuilder().setNode(node).build());
            }

            @Override
            public void getRetinaList(Empty request, StreamObserver<NodeProto.GetRetinaListResponse> observer)
            {
                reply(observer, NodeProto.GetRetinaListResponse.newBuilder().addNodes(node).build());
            }
        }).build().start();
        setting(config, "node.server.host", "127.0.0.1");
        setting(config, "node.server.port", Integer.toString(topology.getPort()));

        ServerContainer container = new ServerContainer();
        IngestClient client = null;
        long cutoverCommitTimestamp = -1;
        try
        {
            TransServer transaction = new TransServer(transactionPort);
            container.addServer("transaction", transaction);
            awaitRunning(container, "transaction", transaction);
            RetinaServer retina = new RetinaServer(retinaPort);
            container.addServer("retina", retina);
            awaitRunning(container, "retina", retina);

            client = new IngestClient("127.0.0.1", transactionPort, secret,
                    64 * 1024 * 1024, 30000);
            awaitParticipantReady(client, owner);
            if (phase.equals("write"))
            {
                InsertResult checkpointed = insertRows(client, 64, "checkpointed", 1);
                InsertResult replay = insertRows(client, 1, "replay", 2);
                savePhaseState(root, checkpointed, replay);
                verifyLegacyFence(retinaPort);
                verifyBufferedRows(retinaPort, replay.transaction.getCommitTimestamp(), 1);
                awaitPublishedFiles(catalog, 1);
                awaitJournalGeneration(root.resolve("wal"), 1);
                System.out.println("PIXELS_NORMAL_INGEST_DAEMON_PHASE1_PASS rows=65"
                        + " checkpointedTransaction=" + checkpointed.transaction.getTransactionId());
            }
            else if (phase.equals("recover"))
            {
                assertPublished(client, recoveredState.checkpointTransactionId);
                assertPublished(client, recoveredState.replayTransactionId);
            }
            else if (phase.equals("cutover"))
            {
                InsertResult afterCutover = insertRows(client, 1, "after-cutover", 3);
                cutoverCommitTimestamp = afterCutover.transaction.getCommitTimestamp();
                if (cutoverCommitTimestamp <= CUTOVER_BASELINE)
                {
                    throw new AssertionError("cutover commit timestamp did not exceed baseline");
                }
                verifyLegacyFence(retinaPort);
                verifyBufferedRows(retinaPort, cutoverCommitTimestamp, 1);
            }
            else
            {
                throw new AssertionError(
                        "daemon accepted an allocator value at/below the cutover baseline");
            }
        }
        finally
        {
            if (client != null) client.close();
            container.shutdownAll();
            if (!container.awaitTermination(90, TimeUnit.SECONDS))
            {
                throw new IllegalStateException("normal daemon services did not stop");
            }
            topology.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
            metadata.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
            catalog.close();
        }
        long rows = countPublishedRows(catalog, root);
        long expectedRows = phase.equals("cutover") ? 66 : 65;
        if (rows != expectedRows)
        {
            throw new AssertionError("graceful shutdown/restart produced " + rows
                    + " physical rows instead of " + expectedRows);
        }
        if (phase.equals("recover"))
        {
            System.out.println("PIXELS_NORMAL_INGEST_DAEMON_PASS rows=65 pixelsFiles="
                    + catalog.publishedFileCount()
                    + " services=TransServer,RetinaServer checkpointRestart=2");
        }
        else if (phase.equals("cutover"))
        {
            System.out.println("PIXELS_NORMAL_INGEST_CUTOVER_PASS oldRows=65 totalRows=66"
                    + " baseline=" + CUTOVER_BASELINE
                    + " commitTimestamp=" + cutoverCommitTimestamp + " legacyFence=1");
        }
    }

    private static final class InsertResult
    {
        private final Transaction transaction;
        private final int payloadBytes;

        private InsertResult(Transaction transaction, int payloadBytes)
        {
            this.transaction = transaction;
            this.payloadBytes = payloadBytes;
        }
    }

    private static final class PhaseState
    {
        private final long checkpointTransactionId;
        private final long replayTransactionId;
        private final int checkpointPayloadBytes;

        private PhaseState(long checkpointTransactionId, long replayTransactionId,
                           int checkpointPayloadBytes)
        {
            this.checkpointTransactionId = checkpointTransactionId;
            this.replayTransactionId = replayTransactionId;
            this.checkpointPayloadBytes = checkpointPayloadBytes;
        }
    }

    private static InsertResult insertRows(
            IngestClient client, int count, String label, int taskId) throws Exception
    {
        Transaction opened = client.coordinator().beginWrite(BeginWriteRequest.newBuilder()
                .setRequestId("normal-daemon-" + label)
                .setSchemaName("s").setTableName("t").setReadTimestamp(0).build());
        WriterAssignment writer = client.coordinator().allocateWriter(
                AllocateWriterRequest.newBuilder()
                        .setTransactionId(opened.getTransactionId())
                        .setRequestId("normal-daemon-" + label + "-writer")
                        .setTaskId(taskId).build());
        MutationStreamId stream = new MutationStreamId(
                opened.getTransactionId(), writer.getWriterId(),
                opened.getTable().getTableId(), 0, MutationStreamId.Kind.APPEND_ROWS);
        List<byte[][]> rows = new ArrayList<>(count);
        for (int i = 0; i < count; i++)
        {
            rows.add(new byte[][] {
                    ByteBuffer.allocate(Long.BYTES).putLong(i).array(),
                    label.getBytes(StandardCharsets.UTF_8)
            });
        }
        byte[] payload = ColumnBatchCodec.encode(
                rows, opened.getTable().getColumnsCount(), 1024 * 1024);
        MutationBatch batch = new MutationBatch(
                stream, 0, opened.getTable().getSchemaVersion(),
                ColumnBatchCodec.FORMAT, count, payload);
        client.transport(opened.getTable()).append(batch).get(30, TimeUnit.SECONDS);
        MutationStreamSeal expected = new MutationStreamSeal(
                stream, 1, count, payload.length,
                MutationStreamSeal.extendDigest(
                        MutationStreamSeal.emptyDigest(), batch.getDigest()));
        MutationStreamSeal seal =
                client.transport(opened.getTable()).seal(expected).get(30, TimeUnit.SECONDS);
        Transaction prepared = client.coordinator().prepareWrite(
                PrepareWriteRequest.newBuilder()
                        .setTransactionId(opened.getTransactionId())
                        .addSeals(IngestWire.encode(seal)).build());
        if (prepared.getState() != TransactionState.PREPARED)
        {
            throw new AssertionError("transaction did not prepare: " + prepared.getState());
        }
        Transaction committed =
                client.coordinator().commitWrite(IngestWire.id(opened.getTransactionId()));
        if (committed.getState() != TransactionState.PUBLISHED)
        {
            throw new AssertionError("transaction did not publish: " + committed.getState());
        }
        return new InsertResult(committed, payload.length);
    }

    private static void savePhaseState(
            Path root, InsertResult checkpointed, InsertResult replay) throws Exception
    {
        Properties properties = new Properties();
        properties.setProperty("checkpoint.transaction",
                Long.toString(checkpointed.transaction.getTransactionId()));
        properties.setProperty("checkpoint.payload.bytes",
                Integer.toString(checkpointed.payloadBytes));
        properties.setProperty("replay.transaction",
                Long.toString(replay.transaction.getTransactionId()));
        Path target = root.resolve("phase-state.properties");
        Path temporary = root.resolve("phase-state.properties.new");
        try (OutputStream output = Files.newOutputStream(temporary))
        {
            properties.store(output, "Normal daemon checkpoint/restart state");
        }
        Files.move(temporary, target,
                java.nio.file.StandardCopyOption.ATOMIC_MOVE,
                java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }

    private static PhaseState loadPhaseState(Path root) throws Exception
    {
        Properties properties = new Properties();
        try (InputStream input = Files.newInputStream(root.resolve("phase-state.properties")))
        {
            properties.load(input);
        }
        long checkpoint = Long.parseLong(properties.getProperty("checkpoint.transaction", "-1"));
        long replay = Long.parseLong(properties.getProperty("replay.transaction", "-1"));
        int payload = Integer.parseInt(properties.getProperty("checkpoint.payload.bytes", "-1"));
        if (checkpoint <= 0 || replay <= checkpoint || payload <= 0)
        {
            throw new AssertionError("invalid phase-one verification state");
        }
        return new PhaseState(checkpoint, replay, payload);
    }

    private static void verifyCheckpointReclamation(Path root, PhaseState phase) throws Exception
    {
        IngestOptions options = new IngestOptions();
        try (AtomicStateFile plans = new AtomicStateFile(
                root.resolve("plans"), options.maxStateBytes))
        {
            InstallationSnapshot snapshot = InstallationSnapshot.parseFrom(plans.read());
            boolean payloadPlanRetained = snapshot.getBatchesList().stream()
                    .anyMatch(batch -> batch.getStream().getTransactionId()
                            == phase.checkpointTransactionId);
            boolean checkpointRetained = snapshot.getCheckpointsList().stream()
                    .anyMatch(checkpoint -> checkpoint.getTransactionId()
                            == phase.checkpointTransactionId);
            if (payloadPlanRetained || !checkpointRetained)
            {
                throw new AssertionError(
                        "checkpoint did not replace the transaction batch plan");
            }
        }
        try (LocalMutationJournal journal = new LocalMutationJournal(
                root.resolve("wal"), options.maxBatchBytes,
                options.walMaxBytes, options.walMaxRecords))
        {
            if (journal.getGeneration() <= 0
                    || !journal.getCheckpointedTransactions()
                            .contains(phase.checkpointTransactionId))
            {
                throw new AssertionError("WAL payload was not replaced by a checkpoint fence");
            }
            if (journal.getJournalBytes() >= phase.checkpointPayloadBytes)
            {
                throw new AssertionError("checkpointed WAL payload was not physically reclaimed");
            }
            try (java.util.stream.Stream<Path> paths = Files.list(root.resolve("wal")))
            {
                long generations = paths.filter(path ->
                        path.getFileName().toString().matches("mutations(?:\\.[0-9]+)?\\.wal"))
                        .count();
                if (generations != 1)
                {
                    throw new AssertionError("obsolete WAL generations remain: " + generations);
                }
            }
        }
    }

    private static void assertPublished(IngestClient client, long transactionId)
    {
        Transaction recovered = client.coordinator().getWrite(IngestWire.id(transactionId));
        if (recovered.getState() != TransactionState.PUBLISHED)
        {
            throw new AssertionError("recovered transaction is not PUBLISHED: "
                    + transactionId + " " + recovered.getState());
        }
    }

    private static void verifyLegacyFence(int retinaPort) throws Exception
    {
        ManagedChannel channel = ManagedChannelBuilder.forAddress("127.0.0.1", retinaPort)
                .usePlaintext().build();
        try
        {
            RetinaWorkerServiceGrpc.RetinaWorkerServiceBlockingStub retina =
                    RetinaWorkerServiceGrpc.newBlockingStub(channel)
                            .withDeadlineAfter(30, TimeUnit.SECONDS);
            RetinaProto.UpdateRecordResponse legacyWrite = retina.updateRecord(
                    RetinaProto.UpdateRecordRequest.newBuilder()
                            .setHeader(RetinaProto.RequestHeader.newBuilder()
                                    .setToken("legacy-write-must-fail"))
                            .setSchemaName("s").setVirtualNodeId(0).build());
            if (legacyWrite.getHeader().getErrorCode() == 0)
            {
                throw new AssertionError("legacy Retina write bypassed transactional cutover");
            }
        }
        finally
        {
            channel.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
        }
    }

    private static void verifyBufferedRows(int retinaPort, long timestamp, int expected)
            throws Exception
    {
        ManagedChannel channel = ManagedChannelBuilder.forAddress("127.0.0.1", retinaPort)
                .usePlaintext().build();
        try
        {
            RetinaProto.GetWriteBufferResponse response =
                    RetinaWorkerServiceGrpc.newBlockingStub(channel)
                            .withDeadlineAfter(30, TimeUnit.SECONDS)
                            .getWriteBuffer(RetinaProto.GetWriteBufferRequest.newBuilder()
                                    .setHeader(RetinaProto.RequestHeader.newBuilder()
                                            .setToken("normal-daemon-read"))
                                    .setSchemaName("s").setTableName("t")
                                    .setVirtualNodeId(0).setTimestamp(timestamp).build());
            if (response.getData().isEmpty())
            {
                throw new AssertionError("published row was not visible in the shared buffer");
            }
            try (VectorizedRowBatch rows =
                    VectorizedRowBatch.deserialize(response.getData().toByteArray()))
            {
                if (rows.size != expected)
                {
                    throw new AssertionError("expected " + expected
                            + " visible buffer row(s), got " + rows.size);
                }
            }
        }
        finally
        {
            channel.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
        }
    }

    private static void awaitPublishedFiles(SqlIngestFixture.Catalog catalog, long expected)
            throws Exception
    {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(45);
        while (System.nanoTime() < deadline)
        {
            if (catalog.publishedFileCount() >= expected)
            {
                return;
            }
            Thread.sleep(100);
        }
        throw new AssertionError("Retina did not publish the expected Pixels file");
    }

    private static void awaitJournalGeneration(Path wal, long expected) throws Exception
    {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(45);
        while (System.nanoTime() < deadline)
        {
            try (java.util.stream.Stream<Path> paths = Files.list(wal))
            {
                if (paths.anyMatch(path -> path.getFileName().toString()
                        .matches("mutations\\.[1-9][0-9]*\\.wal")))
                {
                    return;
                }
            }
            Thread.sleep(100);
        }
        throw new AssertionError("journal did not advance to generation " + expected);
    }

    private static long countPublishedRows(SqlIngestFixture.Catalog catalog, Path root)
            throws Exception
    {
        long rows = 0;
        for (MetadataProto.File file : catalog.files.values())
        {
            if (file.getType() != MetadataProto.File.Type.REGULAR)
            {
                continue;
            }
            String path = root.resolve("ordered").resolve(file.getName()).toUri().toString();
            try (PixelsReader reader = PixelsReaderImpl.newBuilder()
                    .setStorage(StorageFactory.Instance().getStorage(path))
                    .setPath(path).setPixelsFooterCache(new PixelsFooterCache()).build())
            {
                PixelsReaderOption option = new PixelsReaderOption();
                option.includeCols(new String[] {"id", "label"});
                try (PixelsRecordReader records = reader.read(option))
                {
                    VectorizedRowBatch batch;
                    while ((batch = records.readBatch()) != null && batch.size > 0)
                    {
                        rows += batch.size;
                    }
                }
            }
        }
        return rows;
    }

    private static void awaitParticipantReady(IngestClient client, String owner) throws Exception
    {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
        Throwable last = null;
        while (System.nanoTime() < deadline)
        {
            try
            {
                ReadPin pin = client.participant(owner).pinRead(ReadPin.newBuilder()
                        .setTransactionId(1).setReadTimestamp(0).build());
                client.participant(owner).releaseRead(pin);
                return;
            }
            catch (Throwable failure)
            {
                last = failure;
                Thread.sleep(100);
            }
        }
        throw new IllegalStateException("Retina participant never became READY", last);
    }

    private static void awaitRunning(
            ServerContainer container, String name,
            io.pixelsdb.pixels.common.server.Server server) throws Exception
    {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
        while (System.nanoTime() < deadline)
        {
            if (container.checkServer(name) && server.isRunning()) return;
            Thread.sleep(50);
        }
        throw new IllegalStateException(name + " server thread did not start");
    }

    private static void setting(ConfigFactory config, String key, String value)
    {
        config.addProperty(key, value);
    }

    private static <T> void reply(StreamObserver<T> observer, T value)
    {
        observer.onNext(value);
        observer.onCompleted();
    }
}
