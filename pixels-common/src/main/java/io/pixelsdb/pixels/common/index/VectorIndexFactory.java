/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.common.index;

import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import io.pixelsdb.pixels.common.exception.MetadataException;
import io.pixelsdb.pixels.common.exception.VectorIndexException;
import io.pixelsdb.pixels.common.metadata.MetadataService;
import io.pixelsdb.pixels.common.utils.ConfigFactory;
import io.pixelsdb.pixels.common.utils.ShutdownHookManager;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.IOException;
import java.util.List;
import java.util.Map;
import java.util.ServiceLoader;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentSkipListSet;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

import static com.google.common.base.Preconditions.checkArgument;
import static java.util.Objects.requireNonNull;

public class VectorIndexFactory
{
    private static final Logger logger = LogManager.getLogger(VectorIndexFactory.class);
    private final Map<TableVectorIndex, Map<Integer, VectorIndex>> vectorIndexImpls = new ConcurrentHashMap<>();
    private final Map<Long, TableVectorIndex> indexIdToTableIndex = new ConcurrentHashMap<>();
    private final ConcurrentSkipListSet<VectorIndex.Scheme> enabledSchemes = new ConcurrentSkipListSet<>();
    private final ImmutableMap<VectorIndex.Scheme, VectorIndexProvider> providers;
    private final Lock lock = new ReentrantLock();

    private VectorIndexFactory()
    {
        String value = ConfigFactory.Instance().getProperty("enabled.vector.index.schemes");
        requireNonNull(value, "enabled.vector.index.schemes is not configured");
        String[] schemeNames = value.trim().split(",");
        checkArgument(schemeNames.length > 0, "at least one vector index scheme must be enabled");

        ImmutableMap.Builder<VectorIndex.Scheme, VectorIndexProvider> builder = ImmutableMap.builder();
        ServiceLoader<VectorIndexProvider> loader = ServiceLoader.load(VectorIndexProvider.class);
        for (String name : schemeNames)
        {
            VectorIndex.Scheme scheme = VectorIndex.Scheme.from(name);
            enabledSchemes.add(scheme);
            boolean found = false;
            for (VectorIndexProvider provider : loader)
            {
                if (provider.compatibleWith(scheme))
                {
                    builder.put(scheme, provider);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                logger.warn("no vector index provider exists for scheme: {}", scheme.name());
            }
        }
        this.providers = builder.build();
    }

    private static volatile VectorIndexFactory instance;

    public static VectorIndexFactory Instance()
    {
        if (instance == null)
        {
            synchronized (VectorIndexFactory.class)
            {
                if (instance == null)
                {
                    instance = new VectorIndexFactory();
                    ShutdownHookManager.Instance().registerShutdownHook(VectorIndexFactory.class, false, () ->
                    {
                        try
                        {
                            instance.closeAll();
                        }
                        catch (VectorIndexException e)
                        {
                            logger.error("Failed to close all vector index instances.", e);
                        }
                    });
                }
            }
        }
        return instance;
    }

    public List<VectorIndex.Scheme> getEnabledSchemes()
    {
        return ImmutableList.copyOf(enabledSchemes);
    }

    public VectorIndex getVectorIndex(long tableId, long indexId, VectorIndexOption indexOption)
            throws VectorIndexException
    {
        TableVectorIndex tableVectorIndex = indexIdToTableIndex.get(indexId);
        if (tableVectorIndex == null)
        {
            lock.lock();
            try
            {
                tableVectorIndex = indexIdToTableIndex.get(indexId);
                if (tableVectorIndex == null)
                {
                    io.pixelsdb.pixels.common.metadata.domain.VectorIndex vectorIndex =
                            MetadataService.Instance().getVectorIndex(indexId);
                    if (vectorIndex == null)
                    {
                        throw new VectorIndexException("vector index with id " + indexId + " does not exist");
                    }
                    tableVectorIndex = new TableVectorIndex(tableId, indexId, vectorIndex.getIndexScheme(),
                            vectorIndex.getMetric(), vectorIndex.getDimension());
                    indexIdToTableIndex.put(indexId, tableVectorIndex);
                }
            }
            catch (MetadataException e)
            {
                throw new VectorIndexException("failed to query vector index information from metadata", e);
            }
            finally
            {
                lock.unlock();
            }
        }
        return getVectorIndex(tableVectorIndex, indexOption);
    }

    public VectorIndex getVectorIndex(TableVectorIndex tableVectorIndex, VectorIndexOption indexOption)
            throws VectorIndexException
    {
        requireNonNull(tableVectorIndex, "tableVectorIndex is null");
        checkArgument(enabledSchemes.contains(tableVectorIndex.scheme),
                "vector index scheme '" + tableVectorIndex.scheme + "' is not enabled");

        Map<Integer, VectorIndex> vNodeMap = vectorIndexImpls.computeIfAbsent(tableVectorIndex, k -> new ConcurrentHashMap<>());
        int vNodeId = indexOption.getVNodeId();
        VectorIndex vectorIndex = vNodeMap.get(vNodeId);
        if (vectorIndex == null)
        {
            lock.lock();
            try
            {
                vectorIndex = vNodeMap.get(vNodeId);
                if (vectorIndex == null)
                {
                    vectorIndex = providers.get(tableVectorIndex.scheme).createInstance(tableVectorIndex.tableId,
                            tableVectorIndex.indexId, tableVectorIndex.scheme, tableVectorIndex.metric,
                            tableVectorIndex.dimension, indexOption);
                    vNodeMap.put(vNodeId, vectorIndex);
                }
            }
            finally
            {
                lock.unlock();
            }
        }
        return vectorIndex;
    }

    public void closeAll() throws VectorIndexException
    {
        lock.lock();
        try
        {
            for (Map<Integer, VectorIndex> vNodeMap : vectorIndexImpls.values())
            {
                for (VectorIndex index : vNodeMap.values())
                {
                    if (index != null)
                    {
                        try
                        {
                            index.close();
                        }
                        catch (IOException e)
                        {
                            throw new VectorIndexException("failed to close vector index", e);
                        }
                    }
                }
            }
            vectorIndexImpls.clear();
            indexIdToTableIndex.clear();
        }
        finally
        {
            lock.unlock();
        }
    }

    public void closeIndex(long tableId, long indexId, boolean remove, VectorIndexOption option) throws VectorIndexException
    {
        lock.lock();
        try
        {
            TableVectorIndex target = null;
            for (TableVectorIndex tableVectorIndex : vectorIndexImpls.keySet())
            {
                if (tableVectorIndex.tableId == tableId && tableVectorIndex.indexId == indexId)
                {
                    target = tableVectorIndex;
                    break;
                }
            }
            if (target == null)
            {
                return;
            }
            Map<Integer, VectorIndex> vNodeMap = vectorIndexImpls.get(target);
            VectorIndex vectorIndex = vNodeMap == null ? null : vNodeMap.remove(option.getVNodeId());
            if (vectorIndex != null)
            {
                try
                {
                    if (remove)
                    {
                        vectorIndex.closeAndRemove();
                    }
                    else
                    {
                        vectorIndex.close();
                    }
                }
                catch (IOException e)
                {
                    throw new VectorIndexException("failed to close vector index with id " + indexId, e);
                }
            }
            if (vNodeMap == null || vNodeMap.isEmpty())
            {
                vectorIndexImpls.remove(target);
                indexIdToTableIndex.remove(indexId);
            }
        }
        finally
        {
            lock.unlock();
        }
    }

    private static final class TableVectorIndex
    {
        private final long tableId;
        private final long indexId;
        private final VectorIndex.Scheme scheme;
        private final VectorDistanceMetric metric;
        private final int dimension;

        private TableVectorIndex(long tableId, long indexId, VectorIndex.Scheme scheme,
                                 VectorDistanceMetric metric, int dimension)
        {
            this.tableId = tableId;
            this.indexId = indexId;
            this.scheme = scheme;
            this.metric = metric;
            this.dimension = dimension;
        }

        @Override
        public boolean equals(Object o)
        {
            if (this == o)
            {
                return true;
            }
            if (!(o instanceof TableVectorIndex))
            {
                return false;
            }
            TableVectorIndex other = (TableVectorIndex) o;
            return tableId == other.tableId && indexId == other.indexId && scheme == other.scheme
                    && metric == other.metric && dimension == other.dimension;
        }

        @Override
        public int hashCode()
        {
            int result = Long.hashCode(tableId);
            result = 31 * result + Long.hashCode(indexId);
            result = 31 * result + scheme.hashCode();
            result = 31 * result + metric.hashCode();
            result = 31 * result + dimension;
            return result;
        }
    }
}
