/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.common.index;

import io.pixelsdb.pixels.common.exception.VectorIndexException;

import java.io.Closeable;
import java.io.IOException;
import java.util.List;

public interface VectorIndex extends Closeable
{
    enum Scheme
    {
        hnsw;

        public static Scheme from(String value)
        {
            return valueOf(value.toLowerCase());
        }
    }

    long getTableId();

    long getIndexId();

    VectorDistanceMetric getDistanceMetric();

    int getDimension();

    boolean upsert(long rowId, double[] vector, long version) throws VectorIndexException;

    boolean upsert(List<io.pixelsdb.pixels.index.IndexProto.VectorIndexEntry> entries) throws VectorIndexException;

    List<VectorSearchResult> search(double[] queryVector, int topK, VectorIndexOption option) throws VectorIndexException;

    @Override
    @Deprecated
    void close() throws IOException;

    boolean closeAndRemove() throws VectorIndexException;
}
