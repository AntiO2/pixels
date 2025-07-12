/*
 * Copyright 2025 PixelsDB.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Affero GNU General Public License for more details.
 *
 * You should have received a copy of the Affero GNU General Public
 * License along with Pixels.  If not, see
 * <https://www.gnu.org/licenses/>.
 */

package io.pixelsdb.pixels.retina;

import io.pixelsdb.pixels.common.physical.PhysicalReader;
import io.pixelsdb.pixels.common.physical.PhysicalReaderUtil;
import io.pixelsdb.pixels.common.physical.Storage;
import io.pixelsdb.pixels.core.TypeDescription;
import io.pixelsdb.pixels.core.reader.PixelsRecordReader;
import io.pixelsdb.pixels.core.vector.VectorizedRowBatch;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.List;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

public class PixelsWriterBufferRecordReader implements PixelsRecordReader {

    private byte[] data;
    private boolean activeMemtableRead = false;

    private List<Long> fileIds;
    private int fileIdIndex = 0;

    private final Storage storage;

    private final String schemaName;
    private final String tableName;

    private static final Long POLL_INTERVAL_MILLS = 200L;

    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();

    PixelsWriterBufferRecordReader(byte[] activeMemtableData, List<Long> fileIds, // read version
                                   Storage storage,
                                   String schemaName, String tableName // to locate file with file id
                                   ) {

        if(activeMemtableData == null || activeMemtableData.length == 0) {
            activeMemtableRead = true; // we do not need to read active memory table
        } else {
            activeMemtableRead = false;
            data = activeMemtableData;
        }
        this.fileIds = fileIds;
        this.storage = storage;
        this.schemaName = schemaName;
        this.tableName = tableName;
    }

    @Override
    public int prepareBatch(int batchSize) throws IOException {
        if (!activeMemtableRead) {
            activeMemtableRead = true;
        }

        if (fileIdIndex >= fileIds.size()) {
        }

        String path = getMinioPathFromId(fileIdIndex++);
        getMemtableDataFromMinio(path);
        return 0;
    }

    @Override
    public VectorizedRowBatch readBatch() throws IOException {
        return null;
    }

    @Override
    public TypeDescription getResultSchema() {
        return null;
    }

    @Override
    public boolean isValid() {
        return false;
    }

    @Override
    public boolean isEndOfFile() {
        return false;
    }

    @Override
    public boolean seekToRow(long rowIndex) throws IOException {
        return false;
    }

    @Override
    public boolean skip(long rowNum) throws IOException {
        return false;
    }

    @Override
    public long getCompletedRows() {
        return 0;
    }

    @Override
    public long getCompletedBytes() {
        return 0;
    }

    @Override
    public int getNumReadRequests() {
        return 0;
    }

    @Override
    public long getReadTimeNanos() {
        return 0;
    }

    @Override
    public long getMemoryUsage() {
        return 0;
    }

    @Override
    public void close() throws IOException {
        scheduler.shutdown();
    }

    private String getMinioPathFromId(Integer id) {
        return schemaName + '/' + tableName + '/' + id;
    }

    private void getMemtableDataFromMinio(String path) throws IOException {
        // Firstly, if the id is an immutable memtable,
        // we need to wait for it to be flushed to the storage
        // (currently implemented using minio)
        CountDownLatch latch = new CountDownLatch(1);
        final AtomicBoolean fileExists = new AtomicBoolean(false);

        ScheduledFuture<?> pollTask = scheduler.scheduleAtFixedRate(() -> {
            try {
                if (storage.exists(path)) {
                    fileExists.set(true);
                    latch.countDown();
                }
            } catch (IOException e) {
                fileExists.set(false);
                latch.countDown();
            }
        }, 0, POLL_INTERVAL_MILLS, TimeUnit.MILLISECONDS);
        try {
            latch.await();
            pollTask.cancel(true);

            if(!fileExists.get()) {
                throw new IOException("Can't get Retina File: " + path);
            }
            // Create Physical Reader & read this object fully
            ByteBuffer buffer;
            try (PhysicalReader reader = PhysicalReaderUtil.newPhysicalReader(storage, path)) {
                int length = (int) reader.getFileLength();
                buffer = reader.readFully(length);
            }
            data = buffer.array();
        } catch (InterruptedException e) {
            throw new RuntimeException("failed to sleep for retry to get retina file", e);
        }
    }


    @Override
    public VectorizedRowBatch readBatch(int batchSize, boolean reuse) throws IOException {
        throw new UnsupportedOperationException("readBatch(int, boolean) is not implemented");
    }

    @Override
    public VectorizedRowBatch readBatch(int batchSize) throws IOException {
        throw new UnsupportedOperationException("readBatch(int) is not implemented");
    }

    @Override
    public VectorizedRowBatch readBatch(boolean reuse) throws IOException {
        throw new UnsupportedOperationException("readBatch(boolean) is not implemented");
    }

}
