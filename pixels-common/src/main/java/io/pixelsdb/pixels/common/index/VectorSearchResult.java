/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.common.index;

public class VectorSearchResult
{
    private final long rowId;
    private final double score;

    public VectorSearchResult(long rowId, double score)
    {
        this.rowId = rowId;
        this.score = score;
    }

    public long getRowId()
    {
        return rowId;
    }

    public double getScore()
    {
        return score;
    }
}
