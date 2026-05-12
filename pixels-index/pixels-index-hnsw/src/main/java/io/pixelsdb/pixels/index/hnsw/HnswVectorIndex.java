/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.index.hnsw;

import com.github.jelmerk.hnswlib.core.DistanceFunction;
import com.github.jelmerk.hnswlib.core.DistanceFunctions;
import com.github.jelmerk.hnswlib.core.SearchResult;
import com.github.jelmerk.hnswlib.core.hnsw.HnswIndex;
import io.pixelsdb.pixels.common.exception.VectorIndexException;
import io.pixelsdb.pixels.common.index.VectorDistanceMetric;
import io.pixelsdb.pixels.common.index.VectorIndex;
import io.pixelsdb.pixels.common.index.VectorIndexOption;
import io.pixelsdb.pixels.common.index.VectorSearchResult;
import io.pixelsdb.pixels.common.utils.ConfigFactory;
import io.pixelsdb.pixels.index.IndexProto;
import org.apache.commons.io.FileUtils;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

public class HnswVectorIndex implements VectorIndex
{
    private final long tableId;
    private final long indexId;
    private final VectorDistanceMetric metric;
    private final int dimension;
    private final File dataDir;
    private final File indexFile;
    private final AtomicBoolean closed = new AtomicBoolean(false);
    private final AtomicBoolean removed = new AtomicBoolean(false);
    private HnswIndex<Long, double[], HnswVectorItem, Double> index;

    public HnswVectorIndex(long tableId, long indexId, VectorDistanceMetric metric, int dimension,
                           VectorIndexOption indexOption) throws VectorIndexException
    {
        this.tableId = tableId;
        this.indexId = indexId;
        this.metric = metric;
        this.dimension = dimension;
        String basePath = ConfigFactory.Instance().getProperty("index.hnsw.data.path");
        this.dataDir = new File(basePath, "t" + tableId + "_i" + indexId + "_v" + indexOption.getVNodeId());
        this.indexFile = new File(this.dataDir, "index.bin");
        try
        {
            FileUtils.forceMkdir(this.dataDir);
            this.index = loadOrCreateIndex();
        }
        catch (IOException e)
        {
            throw new VectorIndexException("Failed to initialize HNSW index", e);
        }
    }

    @Override
    public long getTableId()
    {
        return tableId;
    }

    @Override
    public long getIndexId()
    {
        return indexId;
    }

    @Override
    public VectorDistanceMetric getDistanceMetric()
    {
        return metric;
    }

    @Override
    public int getDimension()
    {
        return dimension;
    }

    @Override
    public synchronized boolean upsert(long rowId, double[] vector, long version) throws VectorIndexException
    {
        checkClosed();
        validate(vector);
        if (!index.add(new HnswVectorItem(rowId, vector, dimension, version)))
        {
            return false;
        }
        persist();
        return true;
    }

    @Override
    public synchronized boolean upsert(List<IndexProto.VectorIndexEntry> entries) throws VectorIndexException
    {
        checkClosed();
        for (IndexProto.VectorIndexEntry entry : entries)
        {
            double[] vector = entry.getValuesList().stream().mapToDouble(Double::doubleValue).toArray();
            validate(vector);
            if (!index.add(new HnswVectorItem(entry.getRowId(), vector, dimension, entry.getTimestamp())))
            {
                return false;
            }
        }
        persist();
        return true;
    }

    @Override
    public synchronized List<VectorSearchResult> search(double[] queryVector, int topK, VectorIndexOption option)
            throws VectorIndexException
    {
        checkClosed();
        validate(queryVector);
        int efSearch = option.getEfSearch() != null ? option.getEfSearch() :
                Integer.parseInt(ConfigFactory.Instance().getProperty("index.hnsw.ef.search"));
        index.setEf(Math.max(efSearch, topK));
        List<SearchResult<HnswVectorItem, Double>> results = index.findNearest(queryVector, topK);
        List<VectorSearchResult> searchResults = new ArrayList<>(results.size());
        for (SearchResult<HnswVectorItem, Double> result : results)
        {
            searchResults.add(new VectorSearchResult(result.item().id(), result.distance()));
        }
        return searchResults;
    }

    @Override
    public synchronized void close() throws IOException
    {
        if (closed.compareAndSet(false, true) && !removed.get())
        {
            try
            {
                persist();
            }
            catch (VectorIndexException e)
            {
                throw new IOException("Failed to persist HNSW index on close", e);
            }
        }
    }

    @Override
    public synchronized boolean closeAndRemove() throws VectorIndexException
    {
        removed.set(true);
        closed.set(true);
        try
        {
            FileUtils.deleteDirectory(dataDir);
            return true;
        }
        catch (IOException e)
        {
            throw new VectorIndexException("Failed to remove HNSW index directory", e);
        }
    }

    private HnswIndex<Long, double[], HnswVectorItem, Double> loadOrCreateIndex() throws IOException
    {
        if (indexFile.exists())
        {
            try (FileInputStream inputStream = new FileInputStream(indexFile))
            {
                return HnswIndex.load(inputStream);
            }
        }
        int maxItemCount = Integer.parseInt(ConfigFactory.Instance().getProperty("index.hnsw.max.item.count"));
        int m = Integer.parseInt(ConfigFactory.Instance().getProperty("index.hnsw.m"));
        int efConstruction = Integer.parseInt(ConfigFactory.Instance().getProperty("index.hnsw.ef.construction"));
        int ef = Integer.parseInt(ConfigFactory.Instance().getProperty("index.hnsw.ef.search"));
        return HnswIndex
                .newBuilder(dimension, getDistanceFunction(metric), maxItemCount)
                .withM(m)
                .withEfConstruction(efConstruction)
                .withEf(ef)
                .build();
    }

    private void persist() throws VectorIndexException
    {
        try (FileOutputStream outputStream = new FileOutputStream(indexFile))
        {
            index.save(outputStream);
        }
        catch (IOException e)
        {
            throw new VectorIndexException("Failed to persist HNSW index", e);
        }
    }

    private static DistanceFunction<double[], Double> getDistanceFunction(VectorDistanceMetric metric)
    {
        switch (metric)
        {
            case cosine:
                return DistanceFunctions.DOUBLE_COSINE_DISTANCE;
            case l2:
                return DistanceFunctions.DOUBLE_EUCLIDEAN_DISTANCE;
            case inner_product:
                return DistanceFunctions.DOUBLE_INNER_PRODUCT;
            default:
                throw new IllegalArgumentException("Unsupported metric: " + metric);
        }
    }

    private void validate(double[] vector) throws VectorIndexException
    {
        if (vector == null)
        {
            throw new VectorIndexException("vector is null");
        }
        if (vector.length != dimension)
        {
            throw new VectorIndexException("vector dimension mismatch, expected " + dimension + " but got " + vector.length);
        }
    }

    private void checkClosed() throws VectorIndexException
    {
        if (closed.get())
        {
            throw new VectorIndexException("vector index is already closed");
        }
    }
}
