/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.common.index;

import io.pixelsdb.pixels.index.IndexProto;

public class VectorIndexOption extends IndexOption
{
    private Integer efSearch;
    private Long transId;

    public VectorIndexOption() {}

    public VectorIndexOption(IndexProto.IndexOption option)
    {
        super(option);
        if (option.hasEfSearch())
        {
            this.efSearch = option.getEfSearch();
        }
        if (option.hasTransId())
        {
            this.transId = option.getTransId();
        }
    }

    public Integer getEfSearch()
    {
        return efSearch;
    }

    public void setEfSearch(Integer efSearch)
    {
        this.efSearch = efSearch;
    }

    public Long getTransId()
    {
        return transId;
    }

    public void setTransId(Long transId)
    {
        this.transId = transId;
    }

    @Override
    public IndexProto.IndexOption toProto()
    {
        IndexProto.IndexOption.Builder builder = IndexProto.IndexOption.newBuilder()
                .setVirtualNodeId(getVNodeId());
        if (efSearch != null)
        {
            builder.setEfSearch(efSearch);
        }
        if (transId != null)
        {
            builder.setTransId(transId);
        }
        return builder.build();
    }
}
