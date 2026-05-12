/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.common.index;

import io.pixelsdb.pixels.common.exception.VectorIndexException;

import javax.annotation.Nonnull;

public interface VectorIndexProvider
{
    VectorIndex createInstance(long tableId, long indexId, @Nonnull VectorIndex.Scheme scheme,
                               @Nonnull VectorDistanceMetric distanceMetric, int dimension,
                               VectorIndexOption indexOption) throws VectorIndexException;

    boolean compatibleWith(@Nonnull VectorIndex.Scheme scheme);
}
