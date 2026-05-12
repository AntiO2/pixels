/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.index.hnsw;

import com.github.jelmerk.hnswlib.core.Item;

public class HnswVectorItem implements Item<Long, double[]>
{
    private final long rowId;
    private final double[] vector;
    private final int dimensions;
    private final long version;

    public HnswVectorItem(long rowId, double[] vector, int dimensions, long version)
    {
        this.rowId = rowId;
        this.vector = vector;
        this.dimensions = dimensions;
        this.version = version;
    }

    @Override
    public Long id()
    {
        return rowId;
    }

    @Override
    public double[] vector()
    {
        return vector;
    }

    @Override
    public int dimensions()
    {
        return dimensions;
    }

    @Override
    public long version()
    {
        return version;
    }
}
