/*
 * Copyright 2025 PixelsDB.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
package io.pixelsdb.pixels.index.rocksdb;

import org.rocksdb.*;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.stream.Collectors;

/**
 * @package: io.pixelsdb.pixels.index.rocksdb
 * @className: RocksDBFactory
 * @author: AntiO2
 * @date: 2025/9/8 10:33
 */
public class RocksDBFactory
{
    private static RocksDB instance;
    private static String dbPath;

    private RocksDBFactory() { }

    private static RocksDB createRocksDB(String path) throws RocksDBException {
        // 1. Get existing column families (returns empty list for new database)
        List<byte[]> existingColumnFamilies;
        try {
            existingColumnFamilies = RocksDB.listColumnFamilies(new Options(), path);
        } catch (RocksDBException e) {
            // For new database, return list containing only default column family
            existingColumnFamilies = Collections.singletonList(RocksDB.DEFAULT_COLUMN_FAMILY);
        }

        // 2. Ensure default column family is included
        if (!existingColumnFamilies.contains(RocksDB.DEFAULT_COLUMN_FAMILY)) {
            existingColumnFamilies = new ArrayList<>(existingColumnFamilies);
            existingColumnFamilies.add(RocksDB.DEFAULT_COLUMN_FAMILY);
        }

        // --- 🔧 全局 RocksDB 优化参数 ---
        final long blockCacheSize = 512L * 1024 * 1024; // 512MB block cache
        final long writeBufferSize = 128L * 1024 * 1024; // 128MB write buffer

        // Block-based table 配置
        BlockBasedTableConfig tableConfig = new BlockBasedTableConfig()
                .setBlockSize(32 * 1024) // 32KB Block，适合大 key
                .setBlockCacheSize(blockCacheSize)
                .setCacheIndexAndFilterBlocks(true)
                .setCacheIndexAndFilterBlocksWithHighPriority(true)
                .setPinL0FilterAndIndexBlocksInCache(true)
                .setFilterPolicy(new BloomFilter(10, false)); // 10 bits/key 布隆过滤器

        // Column family options
        ColumnFamilyOptions cfOptions = new ColumnFamilyOptions()
                .setTableFormatConfig(tableConfig)
                .setCompressionType(CompressionType.ZSTD_COMPRESSION) // 比 Snappy 更快
                .setBottommostCompressionType(CompressionType.ZSTD_COMPRESSION)
                .optimizeLevelStyleCompaction(writeBufferSize);

        // 3. Prepare column family descriptors
        List<ColumnFamilyDescriptor> descriptors = existingColumnFamilies.stream()
                .map(name -> new ColumnFamilyDescriptor(name, cfOptions))
                .collect(Collectors.toList());

        // 4. RocksDB 实例选项
        DBOptions dbOptions = new DBOptions()
                .setCreateIfMissing(true)
                .setCreateMissingColumnFamilies(true)
                .setMaxOpenFiles(-1) // 防止文件句柄限制
                .setIncreaseParallelism(Runtime.getRuntime().availableProcessors())
                .setMaxBackgroundJobs(4)
                .setUseDirectReads(true) // 避免双层 page cache
                .setUseDirectIoForFlushAndCompaction(true)
                .setAllowConcurrentMemtableWrite(true)
                .setEnableWriteThreadAdaptiveYield(true);

        // 5. Open database
        List<ColumnFamilyHandle> handles = new ArrayList<>();

        return RocksDB.open(dbOptions, path, descriptors, handles);
    }



    public static synchronized RocksDB getRocksDB(String rocksDBPath) throws RocksDBException
    {
        if (instance == null)
        {
            dbPath = rocksDBPath;
            instance = createRocksDB(rocksDBPath);
        } else if (!dbPath.equals(rocksDBPath))
        {
            throw new RocksDBException("RocksDB already initialized with path: "
                    + dbPath + ", cannot reinitialize with: " + rocksDBPath);
        }
        return instance;
    }

    public static synchronized void close()
    {
        if (instance != null)
        {
            instance.close();
            instance = null;
            dbPath = null;
        }
    }

    public static synchronized String getDbPath()
    {
        return dbPath;
    }
}
