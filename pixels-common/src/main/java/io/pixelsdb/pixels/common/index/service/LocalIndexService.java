/*
 * Copyright 2025 PixelsDB.
 *
 * This file is part of Pixels.
 *
 * Pixels is free software: you can redistribute it and/or modify
 * it under the terms of the Affero GNU General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Pixels is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * Affero GNU General Public License for more details.
 *
 * You should have received a copy of the Affero GNU General Public
 * License along with Pixels.  If not, see
 * <https://www.gnu.org/licenses/>.
 */
package io.pixelsdb.pixels.common.index.service;

import io.pixelsdb.pixels.common.exception.IndexException;
import io.pixelsdb.pixels.common.exception.MainIndexException;
import io.pixelsdb.pixels.common.exception.MetadataException;
import io.pixelsdb.pixels.common.exception.RetinaException;
import io.pixelsdb.pixels.common.exception.RowIdException;
import io.pixelsdb.pixels.common.exception.SinglePointIndexException;
import io.pixelsdb.pixels.common.exception.VectorIndexException;
import io.pixelsdb.pixels.common.index.*;
import io.pixelsdb.pixels.common.metadata.MetadataService;
import io.pixelsdb.pixels.common.metadata.domain.File;
import io.pixelsdb.pixels.common.metadata.domain.Layout;
import io.pixelsdb.pixels.common.metadata.domain.Path;
import io.pixelsdb.pixels.common.metadata.domain.Schema;
import io.pixelsdb.pixels.common.metadata.domain.Table;
import io.pixelsdb.pixels.common.retina.RetinaService;
import io.pixelsdb.pixels.common.utils.RetinaUtils;
import io.pixelsdb.pixels.index.IndexProto;
import io.pixelsdb.pixels.common.utils.ConfigFactory;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class LocalIndexService implements IndexService
{
    private static final LocalIndexService defaultInstance = new LocalIndexService();
    private static boolean upsertMode;
    public static LocalIndexService Instance()
    {
        return defaultInstance;
    }

    private LocalIndexService()
    {
        upsertMode = Boolean.parseBoolean(ConfigFactory.Instance().getProperty("retina.upsert-mode.enabled"));
    }

    @Override
    public IndexProto.RowIdBatch allocateRowIdBatch(long tableId, int numRowIds) throws IndexException
    {
        try
        {
            return MainIndexFactory.Instance().getMainIndex(tableId).allocateRowIdBatch(tableId, numRowIds);
        }
        catch (RowIdException | MainIndexException e)
        {
            throw new IndexException("Failed to allocate row ids for tableId=" + tableId, e);
        }
    }

    @Override
    public IndexProto.RowLocation lookupUniqueIndex(IndexProto.IndexKey key, IndexOption indexOption) throws IndexException
    {
        try
        {
            long tableId = key.getTableId();
            long indexId = key.getIndexId();
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            long rowId = singlePointIndex.getUniqueRowId(key);
            if (rowId >= 0)
            {
                IndexProto.RowLocation rowLocation = mainIndex.getLocation(rowId);
                if (rowLocation != null)
                {
                    return rowLocation;
                }
                else
                {
                    throw new IndexException("Failed to get row location for rowId=" + rowId);
                }
            }
            else
            {
                return null;
            }
        }
        catch (SinglePointIndexException | MainIndexException e)
        {
            throw new IndexException("Failed to lookup unique index for key=" + key, e);
        }
    }

    @Override
    public List<IndexProto.RowLocation> lookupNonUniqueIndex(IndexProto.IndexKey key, IndexOption indexOption) throws IndexException
    {
        try
        {
            long tableId = key.getTableId();
            long indexId = key.getIndexId();
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // Get all row IDs for the given index key
            List<Long> rowIds = singlePointIndex.getRowIds(key);
            List<IndexProto.RowLocation> rowLocations = new ArrayList<>();
            if (!rowIds.isEmpty())
            {
                // Iterate and resolve each rowId into a RowLocation
                for (long rowId : rowIds)
                {
                    IndexProto.RowLocation rowLocation = mainIndex.getLocation(rowId);
                    if (rowLocation != null)
                    {
                        rowLocations.add(rowLocation);
                    }
                    else
                    {
                        // If any row location fails, stop and throw an exception
                        throw new IndexException("Failed to get row location for rowId=" + rowId);
                    }
                }
                return rowLocations;
            }
            else
            {
                return null;
            }
        }
        catch (SinglePointIndexException | MainIndexException e)
        {
            throw new IndexException("Failed to lookup non-unique index for key=" + key, e);
        }
    }

    @Override
    public boolean putPrimaryIndexEntry(IndexProto.PrimaryIndexEntry entry, IndexOption indexOption) throws IndexException
    {
        try
        {
            IndexProto.IndexKey key = entry.getIndexKey();
            long tableId = key.getTableId();
            long indexId = key.getIndexId();
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // Insert into single point index
            boolean spSuccess = singlePointIndex.putEntry(entry.getIndexKey(), entry.getRowId());
            if (!spSuccess)
            {
                throw new IndexException("Failed to put entry into single point index for key=" + key);
            }
            // Insert into main index
            boolean mainSuccess = mainIndex.putEntry(entry.getRowId(), entry.getRowLocation());
            if (!mainSuccess)
            {
                throw new IndexException("Failed to put entry into main index for rowId=" + entry.getRowId());
            }
            return true;
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException("Failed to put entry into single point index for key=" + entry.getIndexKey(), e);
        }
        catch (MainIndexException e)
        {
            throw new IndexException("Failed to put entry into main index for rowId=" + entry.getRowId(), e);
        }
    }

    @Override
    public boolean putPrimaryIndexEntries(long tableId, long indexId, List<IndexProto.PrimaryIndexEntry> entries, IndexOption indexOption) throws IndexException
    {
        try
        {
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // Batch insert into single point index
            boolean success = singlePointIndex.putPrimaryEntries(entries);
            if (!success)
            {
                throw new IndexException("Failed to put primary entries into single point index, tableId="
                        + tableId + ", indexId=" + indexId);
            }
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            for (Boolean mainSuccess : mainIndex.putEntries(entries))
            {
                if(!mainSuccess)
                {
                    throw new MainIndexException("Failed to put entry into main index, tableId: " + tableId);
                }
            }
            return true;
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException("Failed to put primary entries into single point index, tableId="
                    + tableId + ", indexId=" + indexId, e);
        }
        catch (MainIndexException e)
        {
            // Retained for consistency with original code, though normally not expected here
            throw new IndexException("Failed to put primary entries into main index, tableId="
                    + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean putSecondaryIndexEntry(IndexProto.SecondaryIndexEntry entry, IndexOption indexOption) throws IndexException
    {
        try
        {
            IndexProto.IndexKey key = entry.getIndexKey();
            long tableId = key.getTableId();
            long indexId = key.getIndexId();
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // Insert into secondary index
            boolean success = singlePointIndex.putEntry(entry.getIndexKey(), entry.getRowId());
            if (!success)
            {
                throw new IndexException("Failed to put entry into secondary index for key=" + key);
            }
            return true;
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException("Failed to put entry into secondary index for key=" + entry.getIndexKey(), e);
        }
    }

    @Override
    public boolean putSecondaryIndexEntries(long tableId, long indexId, List<IndexProto.SecondaryIndexEntry> entries, IndexOption indexOption) throws IndexException
    {
        try
        {
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            boolean success = singlePointIndex.putSecondaryEntries(entries);
            if (!success)
            {
                throw new IndexException("Failed to put secondary index entries for tableId=" + tableId + ", indexId=" + indexId);
            }
            return true;
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException("Error putting secondary index entries for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public IndexProto.RowLocation deletePrimaryIndexEntry(IndexProto.IndexKey key, IndexOption indexOption) throws IndexException
    {
        try
        {
            long tableId = key.getTableId();
            long indexId = key.getIndexId();
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            long prevRowId = singlePointIndex.deleteUniqueEntry(key);
            if (prevRowId < 0)
            {
                if (upsertMode)
                {
                    return null;
                }
                throw new IndexException("Primary index entry not found for tableId=" + tableId + ", indexId=" + indexId);
            }
            IndexProto.RowLocation location = mainIndex.getLocation(prevRowId);
            if (location == null)
            {
                throw new IndexException("Failed to get row location for rowId=" + prevRowId + " (tableId=" + tableId + ")");
            }
            return location;
        }
        catch (MainIndexException | SinglePointIndexException e)
        {
            throw new IndexException("Error deleting primary index entry for tableId="
                    + key.getTableId() + ", indexId=" + key.getIndexId(), e);
        }
    }

    @Override
    public List<IndexProto.RowLocation> deletePrimaryIndexEntries(
            long tableId, long indexId, List<IndexProto.IndexKey> keys, IndexOption indexOption) throws IndexException
    {
        try
        {
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            List<Long> prevRowIds = singlePointIndex.deleteEntries(keys);
            if (prevRowIds == null || prevRowIds.isEmpty())
            {
                if (upsertMode)
                {
                    return new ArrayList<>();
                }
                throw new IndexException("Primary index entries not found for tableId="
                        + tableId + ", indexId=" + indexId);
            }
            List<IndexProto.RowLocation> locations = mainIndex.getLocations(prevRowIds);
            if (locations == null || locations.isEmpty())
            {
                throw new IndexException("Failed to get row locations for tableId=" + tableId + ", indexId=" + indexId);
            }
            return locations;
        }
        catch (MainIndexException | SinglePointIndexException e)
        {
            throw new IndexException("Error deleting primary index entries for tableId="
                    + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public List<Long> deleteSecondaryIndexEntry(IndexProto.IndexKey key, IndexOption indexOption) throws IndexException
    {
        try
        {
            long tableId = key.getTableId();
            long indexId = key.getIndexId();
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            List<Long> prevRowIds = singlePointIndex.deleteEntry(key);
            if (prevRowIds == null || prevRowIds.isEmpty())
            {
                throw new IndexException("Failed to get previous row ids for tableId=" + tableId + ", indexId=" + indexId);
            }
            return prevRowIds;
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException("Failed to delete secondary index entry for key=" + key, e);
        }
    }

    @Override
    public List<Long> deleteSecondaryIndexEntries(long tableId, long indexId, List<IndexProto.IndexKey> keys, IndexOption indexOption) throws IndexException
    {
        try
        {
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            List<Long> prevRowIds = singlePointIndex.deleteEntries(keys);
            if (prevRowIds == null || prevRowIds.isEmpty())
            {
                throw new IndexException("Failed to get previous row ids for tableId=" + tableId + ", indexId=" + indexId);
            }
            return prevRowIds;
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException(
                    "Failed to delete secondary index entries for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public IndexProto.RowLocation updatePrimaryIndexEntry(IndexProto.PrimaryIndexEntry indexEntry, IndexOption indexOption) throws IndexException
    {
        IndexProto.IndexKey key = indexEntry.getIndexKey();
        long tableId = key.getTableId();
        long indexId = key.getIndexId();
        try
        {
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // update the entry in the single point index and get the previous row ID
            long prevRowId = singlePointIndex.updatePrimaryEntry(key, indexEntry.getRowId());
            IndexProto.RowLocation prevLocation = null;
            if (prevRowId >= 0)
            {
                // retrieve the previous RowLocation from the main index
                prevLocation = mainIndex.getLocation(prevRowId);
                if (prevLocation == null)
                {
                    throw new IndexException("Failed to get previous row location for rowId=" + prevRowId);
                }
            }
            else
            {
                if (!upsertMode)
                {
                    throw new IndexException("Failed to get previous row id for tableId=" + tableId + ", indexId=" + indexId);
                }
            }
            boolean mainSuccess = mainIndex.putEntry(indexEntry.getRowId(), indexEntry.getRowLocation());
            if (!mainSuccess)
            {
                throw new MainIndexException("Failed to put entry into main index for rowId=" + indexEntry.getRowId());
            }
            return prevLocation;
        }
        catch (MainIndexException e)
        {
            throw new IndexException(
                    "Failed to update primary index entry in main index for tableId=" + tableId, e);
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException(
                    "Failed to update primary index entry in single point index for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public List<IndexProto.RowLocation> updatePrimaryIndexEntries
            (long tableId, long indexId, List<IndexProto.PrimaryIndexEntry> indexEntries, IndexOption indexOption) throws IndexException
    {
        try
        {
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // update multiple entries in the single point index, returning previous row IDs
            List<Long> prevRowIds = singlePointIndex.updatePrimaryEntries(indexEntries);
            if (prevRowIds == null || prevRowIds.isEmpty())
            {
                if (!upsertMode)
                {
                    throw new IndexException("Failed to get previous row ids for tableId=" + tableId + ", indexId=" + indexId);
                }
                prevRowIds = new ArrayList<>();
            }
            List<IndexProto.RowLocation> prevRowLocations = mainIndex.getLocations(prevRowIds);
            if (prevRowLocations == null || prevRowLocations.isEmpty())
            {
                if (!(upsertMode && prevRowIds.isEmpty()))
                {
                    throw new IndexException("Failed to get previous row locations for tableId=" +
                            tableId + ", indexId=" + indexId);
                }

            }
            for (Boolean mainSuccess : mainIndex.putEntries(indexEntries))
            {
                if(!mainSuccess)
                {
                    throw new MainIndexException("Failed to update entries in main index: " + tableId);
                }
            }
            return prevRowLocations;
        }
        catch (MainIndexException e)
        {
            throw new IndexException("Failed to update primary index entries in main index for tableId=" + tableId, e);
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException("Failed to update primary index entries in single point index for tableId=" +
                    tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public List<Long> updateSecondaryIndexEntry(IndexProto.SecondaryIndexEntry indexEntry, IndexOption indexOption) throws IndexException
    {
        IndexProto.IndexKey key = indexEntry.getIndexKey();
        long tableId = key.getTableId();
        long indexId = key.getIndexId();
        try
        {
            // get the single point index for the table and index ID
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // update the secondary index entry and return previous row IDs
            List<Long> prevRowIds = singlePointIndex.updateSecondaryEntry(key, indexEntry.getRowId());
            if (prevRowIds == null || prevRowIds.isEmpty())
            {
                throw new IndexException("Failed to get previous row ids for tableId=" + tableId + ", indexId=" + indexId);
            }
            return prevRowIds;
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException(
                    "Failed to update secondary index entry for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public List<Long> updateSecondaryIndexEntries
            (long tableId, long indexId, List<IndexProto.SecondaryIndexEntry> indexEntries, IndexOption indexOption) throws IndexException
    {
        try
        {
            // get the single point index for the table and index ID
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // update all secondary index entries and return previous row IDs
            List<Long> prevRowIds = singlePointIndex.updateSecondaryEntries(indexEntries);
            if (prevRowIds == null || prevRowIds.isEmpty())
            {
                throw new IndexException("Failed to get previous row ids for tableId=" + tableId + ", indexId=" + indexId);
            }
            return prevRowIds;
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException(
                    "Failed to update secondary index entries for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean purgeIndexEntries
            (long tableId, long indexId, List<IndexProto.IndexKey> indexKeys, boolean isPrimary, IndexOption indexOption) throws IndexException
    {
        try
        {
            // get the single point index for the table and index
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            // purge the entries from the index
            List<Long> rowIds = singlePointIndex.purgeEntries(indexKeys);
            if (rowIds == null || rowIds.isEmpty())
            {
                // no entries found to purge
                return false;
            }
            if (isPrimary)
            {
                // if primary index, delete corresponding rows from MainIndex
                MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
                int last = rowIds.size() - 1;
                IndexProto.RowLocation rowLocationFirst = mainIndex.getLocation(rowIds.get(0));
                IndexProto.RowLocation rowLocationLast = mainIndex.getLocation(rowIds.get(last));

                RowIdRange rowIdRange = new RowIdRange(
                        rowIds.get(0), rowIds.get(last),
                        rowLocationFirst.getFileId(),
                        rowLocationFirst.getRgId(),
                        rowLocationFirst.getRgRowOffset(),
                        rowLocationLast.getRgRowOffset()
                );
                if (mainIndex.hasCache())
                {
                    mainIndex.flushCache(rowLocationFirst.getFileId());
                }
                mainIndex.deleteRowIdRange(rowIdRange);
            }
            return true;
        }
        catch (MainIndexException e)
        {
            throw new IndexException("Failed to purge main index entries for tableId=" + tableId, e);
        }
        catch (SinglePointIndexException e)
        {
            throw new IndexException("Failed to purge single point index entries for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean flushIndexEntriesOfFile
            (long tableId, long indexId, long fileId, boolean isPrimary, IndexOption indexOption) throws IndexException
    {
        try
        {
            if (isPrimary)
            {
                // get the MainIndex for the table
                MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
                if (mainIndex == null)
                {
                    // MainIndex not found
                    return false;
                }
                // flush cache of the specified file
                mainIndex.flushCache(fileId);
            }
            return true;
        }
        catch (MainIndexException e)
        {
            throw new IndexException("Failed to flush main index for tableId=" + tableId + ", fileId=" + fileId, e);
        }
    }

    @Override
    public boolean openIndex(long tableId, long indexId, boolean isPrimary, IndexOption indexOption) throws IndexException
    {
        try
        {
            // get the single-point index
            SinglePointIndex singlePointIndex = SinglePointIndexFactory.Instance().getSinglePointIndex(tableId, indexId, indexOption);
            if (singlePointIndex == null)
            {
                throw new IndexException("Failed to open single-point index for tableId=" + tableId + ", indexId=" + indexId);
            }
            // if it's a primary index, ensure the main index exists
            if (isPrimary)
            {
                MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
                if (mainIndex == null)
                {
                    throw new IndexException("Failed to open main index for tableId=" + tableId);
                }
            }
            return true;
        }
        catch (SinglePointIndexException | MainIndexException e)
        {
            throw new IndexException("Failed to open index for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean closeIndex(long tableId, long indexId, boolean isPrimary, IndexOption option) throws IndexException
    {
        try
        {
            // close the single-point index
            SinglePointIndexFactory.Instance().closeIndex(tableId, indexId, false, option);
            // if it's a primary index, also close the main index
            if (isPrimary)
            {
                MainIndexFactory.Instance().closeIndex(tableId, false);
            }
            return true;
        }
        catch (SinglePointIndexException | MainIndexException e)
        {
            throw new IndexException("Failed to close index for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean removeIndex(long tableId, long indexId, boolean isPrimary, IndexOption option) throws IndexException
    {
        try
        {
            // close and remove the single-point index completely
            SinglePointIndexFactory.Instance().closeIndex(tableId, indexId, true, option);
            // if it's a primary index, also remove the main index completely
            if (isPrimary)
            {
                MainIndexFactory.Instance().closeIndex(tableId, true);
            }
            return true;
        }
        catch (SinglePointIndexException | MainIndexException e)
        {
            throw new IndexException("Failed to remove index for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean upsertVectorIndexEntry(IndexProto.VectorIndexEntry entry, VectorIndexOption indexOption) throws IndexException
    {
        try
        {
            VectorIndex vectorIndex = VectorIndexFactory.Instance().getVectorIndex(entry.getTableId(), entry.getIndexId(), indexOption);
            return vectorIndex.upsert(entry.getRowId(), entry.getValuesList().stream().mapToDouble(Double::doubleValue).toArray(),
                    entry.getTimestamp());
        }
        catch (VectorIndexException e)
        {
            throw new IndexException("Failed to upsert vector index entry for indexId=" + entry.getIndexId(), e);
        }
    }

    @Override
    public boolean upsertVectorIndexEntries(long tableId, long indexId, List<IndexProto.VectorIndexEntry> entries,
                                            VectorIndexOption indexOption) throws IndexException
    {
        try
        {
            VectorIndex vectorIndex = VectorIndexFactory.Instance().getVectorIndex(tableId, indexId, indexOption);
            return vectorIndex.upsert(entries);
        }
        catch (VectorIndexException e)
        {
            throw new IndexException("Failed to upsert vector index entries for indexId=" + indexId, e);
        }
    }

    @Override
    public List<VectorSearchResult> searchVectorIndex(long tableId, long indexId, double[] queryVector, int topK,
                                                      long timestamp, VectorIndexOption indexOption) throws IndexException
    {
        try
        {
            VectorIndex vectorIndex = VectorIndexFactory.Instance().getVectorIndex(tableId, indexId, indexOption);
            int oversampleFactor = Integer.parseInt(ConfigFactory.Instance().getProperty("index.hnsw.oversample.factor"));
            List<VectorSearchResult> candidates = vectorIndex.search(queryVector, Math.max(topK, topK * oversampleFactor), indexOption);
            MainIndex mainIndex = MainIndexFactory.Instance().getMainIndex(tableId);
            MetadataService metadataService = MetadataService.Instance();
            Map<Long, Path> pathMap = getPathMap(metadataService, tableId);
            List<VectorSearchResult> visible = new ArrayList<>(topK);
            for (VectorSearchResult candidate : candidates)
            {
                IndexProto.RowLocation rowLocation = mainIndex.getLocation(candidate.getRowId());
                if (rowLocation == null)
                {
                    continue;
                }
                File file = metadataService.getFileById(rowLocation.getFileId());
                Path path = pathMap.get(file.getPathId());
                RetinaService retinaService = (path == null) ? RetinaService.Instance() :
                        RetinaUtils.getRetinaServiceFromPath(File.getFilePath(path, file));
                RetinaService.VisibilityResult visibilityResult = retinaService.queryVisibility(rowLocation.getFileId(),
                        new int[]{rowLocation.getRgId()}, timestamp,
                        indexOption.getTransId() == null ? -1L : indexOption.getTransId());
                if (visibilityResult.isOffloaded())
                {
                    continue;
                }
                long[][] bitmaps = visibilityResult.getBitmaps();
                if (bitmaps.length == 0 || !isDeleted(bitmaps[0], rowLocation.getRgRowOffset()))
                {
                    visible.add(candidate);
                    if (visible.size() >= topK)
                    {
                        break;
                    }
                }
            }
            return visible;
        }
        catch (VectorIndexException | MainIndexException | MetadataException | RetinaException e)
        {
            throw new IndexException("Failed to search vector index for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean openVectorIndex(long tableId, long indexId, VectorIndexOption indexOption) throws IndexException
    {
        try
        {
            return VectorIndexFactory.Instance().getVectorIndex(tableId, indexId, indexOption) != null;
        }
        catch (VectorIndexException e)
        {
            throw new IndexException("Failed to open vector index for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean closeVectorIndex(long tableId, long indexId, VectorIndexOption option) throws IndexException
    {
        try
        {
            VectorIndexFactory.Instance().closeIndex(tableId, indexId, false, option);
            return true;
        }
        catch (VectorIndexException e)
        {
            throw new IndexException("Failed to close vector index for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    @Override
    public boolean removeVectorIndex(long tableId, long indexId, VectorIndexOption option) throws IndexException
    {
        try
        {
            VectorIndexFactory.Instance().closeIndex(tableId, indexId, true, option);
            return true;
        }
        catch (VectorIndexException e)
        {
            throw new IndexException("Failed to remove vector index for tableId=" + tableId + ", indexId=" + indexId, e);
        }
    }

    private static boolean isDeleted(long[] bitmap, int rowOffset)
    {
        if (bitmap == null || bitmap.length == 0)
        {
            return false;
        }
        int wordIndex = rowOffset / 64;
        if (wordIndex >= bitmap.length)
        {
            return false;
        }
        return (bitmap[wordIndex] & (1L << (rowOffset % 64))) != 0;
    }

    private static Map<Long, Path> getPathMap(MetadataService metadataService, long tableId) throws MetadataException
    {
        Table table = metadataService.getTableById(tableId);
        Schema schema = metadataService.getSchemaById(table.getSchemaId());
        List<Layout> layouts = metadataService.getLayouts(schema.getName(), table.getName());
        Map<Long, Path> pathMap = new HashMap<>();
        for (Layout layout : layouts)
        {
            for (Path path : layout.getOrderedPaths())
            {
                pathMap.put(path.getId(), path);
            }
            for (Path path : layout.getCompactPaths())
            {
                pathMap.put(path.getId(), path);
            }
            for (Path path : layout.getProjectionPaths().values())
            {
                pathMap.put(path.getId(), path);
            }
        }
        return pathMap;
    }
}
