/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.index.hnsw;

import io.pixelsdb.pixels.common.exception.VectorIndexException;
import io.pixelsdb.pixels.common.index.VectorDistanceMetric;
import io.pixelsdb.pixels.common.index.VectorIndex;
import io.pixelsdb.pixels.common.index.VectorIndexOption;
import io.pixelsdb.pixels.common.index.VectorIndexProvider;

import javax.annotation.Nonnull;

public class HnswVectorIndexProvider implements VectorIndexProvider
{
    @Override
    public VectorIndex createInstance(long tableId, long indexId, @Nonnull VectorIndex.Scheme scheme,
                                      @Nonnull VectorDistanceMetric distanceMetric, int dimension,
                                      VectorIndexOption indexOption) throws VectorIndexException
    {
        if (scheme == VectorIndex.Scheme.hnsw)
        {
            return new HnswVectorIndex(tableId, indexId, distanceMetric, dimension, indexOption);
        }
        throw new VectorIndexException("Unsupported scheme: " + scheme);
    }

    @Override
    public boolean compatibleWith(@Nonnull VectorIndex.Scheme scheme)
    {
        return scheme == VectorIndex.Scheme.hnsw;
    }
}
