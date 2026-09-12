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
import io.pixelsdb.pixels.common.ingest.rpc.IngestClient;
import io.pixelsdb.pixels.common.ingest.wire.ColumnBatchCodec;
import io.pixelsdb.pixels.common.ingest.wire.IngestWire;
import io.pixelsdb.pixels.common.utils.ConfigFactory;
import io.pixelsdb.pixels.common.utils.NetUtils;
import io.pixelsdb.pixels.core.vector.VectorizedRowBatch;
import io.pixelsdb.pixels.daemon.NodeProto;
import io.pixelsdb.pixels.daemon.NodeServiceGrpc;
import io.pixelsdb.pixels.daemon.ServerContainer;
import io.pixelsdb.pixels.daemon.retina.RetinaServer;
import io.pixelsdb.pixels.daemon.transaction.TransServer;
import io.pixelsdb.pixels.ingest.IngestProto.AllocateWriterRequest;
import io.pixelsdb.pixels.ingest.IngestProto.BeginWriteRequest;
import io.pixelsdb.pixels.ingest.IngestProto.PrepareWriteRequest;
import io.pixelsdb.pixels.ingest.IngestProto.ReadPin;
import io.pixelsdb.pixels.ingest.IngestProto.Transaction;
import io.pixelsdb.pixels.ingest.IngestProto.TransactionState;
import io.pixelsdb.pixels.ingest.IngestProto.WriterAssignment;
import io.pixelsdb.pixels.retina.RetinaProto;
import io.pixelsdb.pixels.retina.RetinaWorkerServiceGrpc;

import java.net.ServerSocket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Collections;
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
    private NormalIngestDaemonMain() {}

    public static void main(String[] args)
    {
        int exit = 0;
        try
        {
            run(Paths.get(args[0]), Integer.parseInt(args[1]));
        }
        catch (Throwable failure)
        {
            failure.printStackTrace(System.err);
            exit = 1;
        }
        System.exit(exit);
    }

    private static void run(Path root, int etcdPort) throws Exception
    {
        Files.createDirectories(root);
        Files.createDirectories(root.resolve("ordered"));
        Files.createDirectories(root.resolve("compact"));
        int retinaPort = freePort();
        int transactionPort = freePort();
        String host = NetUtils.getLocalHostName();
        String owner = host + ":" + retinaPort;
        String secret = "normal-daemon-" + UUID.randomUUID();
        Path secretFile = root.resolve("credential");
        Files.write(secretFile, secret.getBytes(StandardCharsets.UTF_8));

        ConfigFactory config = ConfigFactory.Instance();
        setting(config, "retina.enable", "true");
        setting(config, "retina.ingest.enabled", "true");
        setting(config, "retina.ingest.auth.secret.file", secretFile.toString());
        setting(config, "retina.ingest.coordinator.state.dir", root.resolve("decisions").toString());
        setting(config, "retina.ingest.participant.plan.dir", root.resolve("plans").toString());
        setting(config, "retina.ingest.participant.wal.dir", root.resolve("wal").toString());
        setting(config, "retina.ingest.cutover.baseline.timestamp", "0");
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
        setting(config, "retina.storage.gc.enabled", "false");
        setting(config, "retina.gc.interval", "1");
        setting(config, "retina.buffer.memTable.size", "64");
        setting(config, "retina.buffer.flush.count", "2");
        setting(config, "retina.buffer.flush.interval", "3600");
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

        SqlIngestFixture.Catalog catalog = new SqlIngestFixture.Catalog(root);
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
        ManagedChannel retinaReads = null;
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

            Transaction opened = client.coordinator().beginWrite(BeginWriteRequest.newBuilder()
                    .setRequestId("normal-daemon-insert")
                    .setSchemaName("s").setTableName("t").setReadTimestamp(0).build());
            WriterAssignment writer = client.coordinator().allocateWriter(
                    AllocateWriterRequest.newBuilder()
                            .setTransactionId(opened.getTransactionId())
                            .setRequestId("normal-daemon-writer").setTaskId(1).build());
            MutationStreamId stream = new MutationStreamId(
                    opened.getTransactionId(), writer.getWriterId(),
                    opened.getTable().getTableId(), 0, MutationStreamId.Kind.APPEND_ROWS);
            byte[][] row = new byte[][] {
                    ByteBuffer.allocate(Long.BYTES).putLong(7).array(),
                    "daemon".getBytes(StandardCharsets.UTF_8)
            };
            byte[] payload = ColumnBatchCodec.encode(
                    Collections.singletonList(row), opened.getTable().getColumnsCount(), 4096);
            MutationBatch batch = new MutationBatch(
                    stream, 0, opened.getTable().getSchemaVersion(),
                    ColumnBatchCodec.FORMAT, 1, payload);
            client.transport(opened.getTable()).append(batch).get(30, TimeUnit.SECONDS);
            MutationStreamSeal expected = new MutationStreamSeal(
                    stream, 1, 1, payload.length,
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

            retinaReads = ManagedChannelBuilder.forAddress("127.0.0.1", retinaPort)
                    .usePlaintext().build();
            RetinaWorkerServiceGrpc.RetinaWorkerServiceBlockingStub retinaStub =
                    RetinaWorkerServiceGrpc.newBlockingStub(retinaReads)
                            .withDeadlineAfter(30, TimeUnit.SECONDS);
            RetinaProto.UpdateRecordResponse legacyWrite = retinaStub.updateRecord(
                    RetinaProto.UpdateRecordRequest.newBuilder()
                            .setHeader(RetinaProto.RequestHeader.newBuilder()
                                    .setToken("legacy-write-must-fail"))
                            .setSchemaName("s").setVirtualNodeId(0).build());
            if (legacyWrite.getHeader().getErrorCode() == 0)
            {
                throw new AssertionError("legacy Retina write bypassed transactional cutover");
            }
            RetinaProto.GetWriteBufferResponse response =
                    retinaStub.getWriteBuffer(RetinaProto.GetWriteBufferRequest.newBuilder()
                                    .setHeader(RetinaProto.RequestHeader.newBuilder()
                                            .setToken("normal-daemon-read"))
                                    .setSchemaName("s").setTableName("t")
                                    .setVirtualNodeId(0)
                                    .setTimestamp(committed.getCommitTimestamp()).build());
            if (response.getData().isEmpty())
            {
                throw new AssertionError("published row was not visible in the shared buffer");
            }
            try (VectorizedRowBatch rows =
                    VectorizedRowBatch.deserialize(response.getData().toByteArray()))
            {
                if (rows.size != 1)
                {
                    throw new AssertionError("expected one visible row, got " + rows.size);
                }
            }
        }
        finally
        {
            if (retinaReads != null) retinaReads.shutdownNow();
            if (client != null) client.close();
            container.shutdownAll();
            if (!container.awaitTermination(90, TimeUnit.SECONDS))
            {
                throw new IllegalStateException("normal daemon services did not stop");
            }
            topology.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
            metadata.shutdownNow().awaitTermination(5, TimeUnit.SECONDS);
        }
        if (catalog.publishedFileCount() < 1)
        {
            throw new AssertionError("graceful shutdown did not materialize the shared buffer");
        }
        System.out.println("PIXELS_NORMAL_INGEST_DAEMON_PASS rows=1 pixelsFiles="
                + catalog.publishedFileCount() + " services=TransServer,RetinaServer");
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

    private static int freePort() throws Exception
    {
        try (ServerSocket socket = new ServerSocket(0))
        {
            return socket.getLocalPort();
        }
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
