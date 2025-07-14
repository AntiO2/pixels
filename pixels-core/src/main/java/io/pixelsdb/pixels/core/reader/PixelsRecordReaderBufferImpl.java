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

package io.pixelsdb.pixels.core.reader;

import io.pixelsdb.pixels.common.physical.*;
import io.pixelsdb.pixels.core.TypeDescription;
import io.pixelsdb.pixels.core.vector.ColumnVector;
import io.pixelsdb.pixels.core.vector.VectorizedRowBatch;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

public class PixelsRecordReaderBufferImpl implements PixelsRecordReader {

    private byte[] data;
    private byte[] activeMemtableData;
    private VectorizedRowBatch resultRowBatch = null;

    /**
     * Columns included by reader option; if included, set true
     */
    private boolean[] includedColumns;
    /**
     * The ith element in resultColumns is the column id (column's index in the file schema)
     * of ith included column in the read option. The order of columns in the read option's
     * includedCols may be arbitrary, not related to the column order in schema.
     */
    private int[] resultColumns;
    /**
     * The target columns to read after matching reader option.
     * Each element represents a column id (column's index in the file schema).
     * Different from resultColumns, the ith column id in targetColumns
     * corresponds to the ith true value in this.includedColumns, i.e.,
     * The elements in targetColumns and resultColumns are in different order,
     * but they are all the index of the columns in the file schema.
     */
    private int[] targetColumns;

    private final List<Long> fileIds;
    private int fileIdIndex = 0;
    private final PixelsReaderOption option;
    private final Storage storage;

    private final String schemaName;
    private final String tableName;
    private final TypeDescription typeDescription;
    private final int colNum;
    private int includedColumnNum = 0;
    private long readTimeNanos = 0L;
    private boolean checkValid = false;
    private boolean activeMemtableDataEverRead = false;
    private boolean everRead;
    private boolean endOfBuffer = false;
    private final int typeMode = TypeDescription.Mode.CREATE_INT_VECTOR_FOR_INT;
    private static final Long POLL_INTERVAL_MILLS = 200L;

    private final ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();

    PixelsRecordReaderBufferImpl(PixelsReaderOption option,
                                 byte[] activeMemtableData, List<Long> fileIds, // read version
                                 Storage storage,
                                 String schemaName, String tableName, // to locate file with file id
                                 TypeDescription typeDescription
    )  throws IOException
    {
        this.option = option;
        this.activeMemtableData = activeMemtableData;
        this.fileIds = fileIds;
        this.storage = storage;
        this.schemaName = schemaName;
        this.tableName = tableName;
        this.typeDescription = typeDescription;
        this.colNum = typeDescription.getChildrenWithHiddenColumn().size();

        checkBeforeRead();
    }

    private void checkBeforeRead() throws IOException {
        // filter included columns
        includedColumnNum = 0;
        String[] optionIncludedCols = option.getIncludedCols();
        // if size of cols is 0, create an empty row batch

        List<Integer> optionColsIndices = new ArrayList<>();
        this.includedColumns = new boolean[colNum];
        for (String col : optionIncludedCols)
        {
            for (int j = 0; j < colNum; j++)
            {
                if (col.equalsIgnoreCase(typeDescription.getFieldNames().get(j)))
                {
                    optionColsIndices.add(j);
                    includedColumns[j] = true;
                    includedColumnNum++;
                    break;
                }
            }
        }

        // check included columns
        if (includedColumnNum != optionIncludedCols.length && !option.isTolerantSchemaEvolution())
        {
            checkValid = false;
            throw new IOException("includedColumnsNum is " + includedColumnNum +
                    " whereas optionIncludedCols.length is " + optionIncludedCols.length);
        }

        // create result columns storing result column ids in user specified order
        this.resultColumns = new int[optionIncludedCols.length];
        for (int i = 0; i < optionIncludedCols.length; i++)
        {
            this.resultColumns[i] = optionColsIndices.get(i);
        }
        // assign target columns, ordered by original column order in schema
        int targetColumnNum = new HashSet<>(optionColsIndices).size();
        targetColumns = new int[targetColumnNum];
        int targetColIdx = 0;
        for (int i = 0; i < includedColumns.length; i++)
        {
            if (includedColumns[i])
            {
                targetColumns[targetColIdx] = i;
                targetColIdx++;
            }
        }

        checkValid = true;
    }


    // get Data of the next memtable
    private boolean read() throws IOException {
        if (!checkValid)
        {
            return false;
        }

        if(endOfBuffer)
        {
            return false;
        }

        if(!activeMemtableDataEverRead)
        {
            // We haven't read active memory table data yet
            activeMemtableDataEverRead = true;
            if(activeMemtableData != null && activeMemtableData.length != 0) {
                data = activeMemtableData;
                return true;
            }
        }

        if (fileIdIndex >= fileIds.size()) {
            endOfBuffer = true;
            return false;
        }

        String path = getMinioPathFromId(fileIdIndex++);
        getMemtableDataFromMinio(path);
        return true;
    }


    @Override
    public int prepareBatch(int batchSize) throws IOException {
        if(endOfBuffer) {
            return 0;
        }
        if(data == null) {
            return 0;
        }
        resultRowBatch = VectorizedRowBatch.deserialize(data);
        return resultRowBatch.size;
    }

    /**
     * Create a row batch without any data, only sets the number of rows (size) and OEF.
     * Such a row batch is used for queries such as select count(*).
     * @param size the number of rows in the row batch.
     * @return the empty row batch.
     */
    private VectorizedRowBatch createEmptyEOFRowBatch(int size)
    {
        TypeDescription resultSchema = TypeDescription.createSchema(new ArrayList<>());
        VectorizedRowBatch resultRowBatch = resultSchema.createRowBatch(0, this.typeMode);
        resultRowBatch.projectionSize = 0;
        resultRowBatch.endOfFile = true;
        resultRowBatch.size = size;
        return resultRowBatch;
    }

    @Override
    public VectorizedRowBatch readBatch() throws IOException {
        if(!everRead) {
            long start = System.nanoTime();
            if (!read())
            {
                throw new IOException("failed to read file.");
            }
            readTimeNanos += System.nanoTime() - start;
        }

        resultRowBatch = VectorizedRowBatch.deserialize(data);

        
        return resultRowBatch;
    }

    @Override
    public TypeDescription getResultSchema() {
        // TODO(AntiO2): Schema evolution is currently not supported in Retina, but may be added in the future.
        return typeDescription;
    }

    @Override
    public boolean isValid() {
        return false;
    }

    @Override
    public boolean isEndOfFile() {
        return endOfBuffer;
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

            if (!fileExists.get()) {
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
        return readBatch();
    }

    @Override
    public VectorizedRowBatch readBatch(int batchSize) throws IOException {
        return readBatch();
    }

    @Override
    public VectorizedRowBatch readBatch(boolean reuse) throws IOException {
        return readBatch();
    }
}
