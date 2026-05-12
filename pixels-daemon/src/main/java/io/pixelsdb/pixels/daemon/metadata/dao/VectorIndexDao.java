/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.daemon.metadata.dao;

import io.pixelsdb.pixels.daemon.MetadataProto;

import java.util.List;

public abstract class VectorIndexDao implements Dao<MetadataProto.VectorIndex>
{
    @Override
    public abstract MetadataProto.VectorIndex getById(long id);

    @Override
    public List<MetadataProto.VectorIndex> getAll()
    {
        throw new UnsupportedOperationException("getAll is not supported.");
    }

    public abstract List<MetadataProto.VectorIndex> getAllByTableId(long tableId);

    public boolean save(MetadataProto.VectorIndex vectorIndex)
    {
        if (exists(vectorIndex))
        {
            return update(vectorIndex);
        }
        return insert(vectorIndex) > 0;
    }

    public abstract boolean exists(MetadataProto.VectorIndex vectorIndex);

    public abstract long insert(MetadataProto.VectorIndex vectorIndex);

    public abstract boolean update(MetadataProto.VectorIndex vectorIndex);

    public abstract boolean deleteById(long id);
}
