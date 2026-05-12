/*
 * Copyright 2026 PixelsDB.
 */
package io.pixelsdb.pixels.index.hnsw;

import io.pixelsdb.pixels.common.exception.VectorIndexException;
import io.pixelsdb.pixels.common.index.VectorDistanceMetric;
import io.pixelsdb.pixels.common.index.VectorIndexOption;
import io.pixelsdb.pixels.common.index.VectorSearchResult;
import io.pixelsdb.pixels.common.utils.ConfigFactory;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.nio.file.Path;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class TestHnswVectorIndex
{
    private static final long TABLE_ID = 42L;
    private static final long INDEX_ID = 99L;
    private static final int DIMENSION = 3;

    @TempDir
    Path tempDir;

    @AfterEach
    void cleanup() throws Exception
    {
        VectorIndexOption option = new VectorIndexOption();
        option.setVNodeId(0);
        new HnswVectorIndex(TABLE_ID, INDEX_ID, VectorDistanceMetric.cosine, DIMENSION, option)
                .closeAndRemove();
    }

    @Test
    void shouldPersistAndReloadNearestNeighbors() throws Exception
    {
        configure();
        VectorIndexOption option = new VectorIndexOption();
        option.setVNodeId(0);
        option.setEfSearch(32);

        HnswVectorIndex index = new HnswVectorIndex(TABLE_ID, INDEX_ID, VectorDistanceMetric.cosine, DIMENSION, option);
        assertTrue(index.upsert(1L, new double[] {1.0, 0.0, 0.0}, 100L));
        assertTrue(index.upsert(2L, new double[] {0.0, 1.0, 0.0}, 101L));
        index.close();

        HnswVectorIndex reloaded = new HnswVectorIndex(TABLE_ID, INDEX_ID, VectorDistanceMetric.cosine, DIMENSION, option);
        List<VectorSearchResult> results = reloaded.search(new double[] {0.9, 0.1, 0.0}, 2, option);

        assertEquals(2, results.size());
        assertEquals(1L, results.get(0).getRowId());
        assertTrue(results.get(0).getScore() <= results.get(1).getScore());

        reloaded.closeAndRemove();
    }

    @Test
    void shouldRejectDimensionMismatch() throws Exception
    {
        configure();
        VectorIndexOption option = new VectorIndexOption();
        option.setVNodeId(0);

        HnswVectorIndex index = new HnswVectorIndex(TABLE_ID, INDEX_ID, VectorDistanceMetric.cosine, DIMENSION, option);
        assertThrows(VectorIndexException.class, () -> index.upsert(1L, new double[] {1.0, 0.0}, 1L));
        index.closeAndRemove();
    }

    private void configure()
    {
        ConfigFactory.Instance().addProperty("index.hnsw.data.path", tempDir.toString());
        ConfigFactory.Instance().addProperty("index.hnsw.m", "16");
        ConfigFactory.Instance().addProperty("index.hnsw.ef.construction", "200");
        ConfigFactory.Instance().addProperty("index.hnsw.ef.search", "64");
        ConfigFactory.Instance().addProperty("index.hnsw.max.item.count", "100");
    }
}
