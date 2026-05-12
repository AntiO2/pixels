/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.common.metadata.domain;

import io.pixelsdb.pixels.common.index.VectorDistanceMetric;
import io.pixelsdb.pixels.common.index.VectorIndex.Scheme;
import io.pixelsdb.pixels.daemon.MetadataProto;

public class VectorIndex extends Base
{
    private long vectorColumnId;
    private VectorDistanceMetric metric;
    private int dimension;
    private Scheme indexScheme;
    private String paramsJson;
    private long tableId;
    private long schemaVersionId;

    public VectorIndex() {}

    public VectorIndex(MetadataProto.VectorIndex vectorIndex)
    {
        this.setId(vectorIndex.getId());
        this.vectorColumnId = vectorIndex.getVectorColumnId();
        this.metric = VectorDistanceMetric.from(vectorIndex.getMetric());
        this.dimension = vectorIndex.getDimension();
        this.indexScheme = Scheme.from(vectorIndex.getIndexScheme());
        this.paramsJson = vectorIndex.getParamsJson();
        this.tableId = vectorIndex.getTableId();
        this.schemaVersionId = vectorIndex.getSchemaVersionId();
    }

    public long getVectorColumnId()
    {
        return vectorColumnId;
    }

    public void setVectorColumnId(long vectorColumnId)
    {
        this.vectorColumnId = vectorColumnId;
    }

    public VectorDistanceMetric getMetric()
    {
        return metric;
    }

    public void setMetric(VectorDistanceMetric metric)
    {
        this.metric = metric;
    }

    public int getDimension()
    {
        return dimension;
    }

    public void setDimension(int dimension)
    {
        this.dimension = dimension;
    }

    public Scheme getIndexScheme()
    {
        return indexScheme;
    }

    public void setIndexScheme(Scheme indexScheme)
    {
        this.indexScheme = indexScheme;
    }

    public String getParamsJson()
    {
        return paramsJson;
    }

    public void setParamsJson(String paramsJson)
    {
        this.paramsJson = paramsJson;
    }

    public long getTableId()
    {
        return tableId;
    }

    public void setTableId(long tableId)
    {
        this.tableId = tableId;
    }

    public long getSchemaVersionId()
    {
        return schemaVersionId;
    }

    public void setSchemaVersionId(long schemaVersionId)
    {
        this.schemaVersionId = schemaVersionId;
    }

    @Override
    public MetadataProto.VectorIndex toProto()
    {
        return MetadataProto.VectorIndex.newBuilder()
                .setId(this.getId())
                .setVectorColumnId(this.vectorColumnId)
                .setMetric(this.metric.name())
                .setDimension(this.dimension)
                .setIndexScheme(this.indexScheme.name())
                .setParamsJson(this.paramsJson == null ? "" : this.paramsJson)
                .setTableId(this.tableId)
                .setSchemaVersionId(this.schemaVersionId)
                .build();
    }
}
