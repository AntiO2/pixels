package io.pixelsdb.pixels.daemon.transaction.ingest;

import java.nio.file.*;

/** Separate backend process: connector dependencies must not contaminate Trino's HTTP runtime. */
public final class SqlIngestFixtureMain {
    public static void main(String[] args) throws Exception {
        Path control = Paths.get(args[0]);
        Files.createDirectories(control);
        try (SqlIngestFixture fixture = new SqlIngestFixture()) {
            fixture.exportConfiguration(control.resolve("pixels.properties"));
            fixture.exportStatus(control.resolve("status.properties"));
            Files.write(control.resolve("ready"), new byte[0]);
            System.out.println("PIXELS_SQL_FIXTURE_READY " + fixture.root);
            long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.MINUTES.toNanos(10);
            while (!Files.exists(control.resolve("stop")) && System.nanoTime() < deadline) {
                fixture.exportStatus(control.resolve("status.properties"));
                Thread.sleep(50);
            }
            fixture.exportStatus(control.resolve("status.properties"));
        }
    }
}
