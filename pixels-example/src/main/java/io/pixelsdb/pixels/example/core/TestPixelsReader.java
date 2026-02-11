/*
 * Copyright 2018 PixelsDB.
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
package io.pixelsdb.pixels.example.core;

import io.pixelsdb.pixels.common.physical.Storage;
import io.pixelsdb.pixels.common.physical.StorageFactory;
import io.pixelsdb.pixels.core.PixelsFooterCache;
import io.pixelsdb.pixels.core.PixelsReader;
import io.pixelsdb.pixels.core.PixelsReaderImpl;
import io.pixelsdb.pixels.core.TypeDescription;
import io.pixelsdb.pixels.core.reader.PixelsReaderOption;
import io.pixelsdb.pixels.core.reader.PixelsRecordReader;
import io.pixelsdb.pixels.core.vector.VectorizedRowBatch;

import java.io.IOException;
import java.util.List;

/**
 * @author hank
 */
public class TestPixelsReader {
    public static void main(String[] args) {
        String currentPath = "s3://home-zinuo/hybench/sf1200_v3/loantrans/v-0-ordered/realtime-pixels-retina_20260124112621_29_69356.pxl";
        int totalIterations = 100;
        double deleteDice = 0.9;
        try {
            Storage storage = StorageFactory.Instance().getStorage("s3");
            PixelsReader reader = PixelsReaderImpl.newBuilder()
                    .setStorage(storage)
                    .setPath(currentPath)
                    .setPixelsFooterCache(new PixelsFooterCache())
                    .build();

            PixelsReaderOption option = new PixelsReaderOption();
            option.includeCols(reader.getFileSchema().getFieldNames().toArray(new String[0]));
            option.setDeleteDice(deleteDice);
            option.transTimestamp(Long.MAX_VALUE);

            long globalValidRows = 0;
            long globalDeletedRows = 0;

            // --- 开始计时 ---
            long startTime = System.currentTimeMillis();

            for (int iter = 1; iter <= totalIterations; iter++) {
                // 如果迭代次数很多，可以关掉打印以减少 IO 对耗时的影响
                // System.out.println("Iteration " + iter + " starting...");

                PixelsRecordReader recordReader = reader.read(option);
                VectorizedRowBatch rowBatch;
                while (true) {
                    rowBatch = recordReader.readBatch(10000);
                    globalValidRows += rowBatch.size;
                    globalDeletedRows += rowBatch.deletedSize;
                    if (rowBatch.endOfFile) break;
                }
            }

            // --- 结束计时 ---
            long endTime = System.currentTimeMillis();
            long totalTimeMs = endTime - startTime;

            // --- 数据计算 ---
            long totalRows = globalValidRows + globalDeletedRows;
            double deleteRate = totalRows == 0 ? 0 : (double) globalDeletedRows / totalRows * 100;
            double avgIterTime = (double) totalTimeMs / totalIterations;
            // 吞吐量：每秒处理的总行数 (Total Rows / Second)
            double throughput = totalTimeMs == 0 ? 0 : (double) totalRows / (totalTimeMs / 1000.0);

            // 1. 标准控制台打印 (用于直观查看)
            System.out.println("================== Result Statistics ==================");
            System.out.println(String.format("Time Taken: %d ms", totalTimeMs));
            System.out.println(String.format("Throughput: %.2f rows/s", throughput));

            // 2. CSV 格式输出 (最后一行为 CSV 内容)
            System.out.println("\nCSV Output (Header + Data):");
            System.out.println("Iterations,TotalRows,ValidRows,DeletedRows,DeleteRate(%),TotalTime(ms),AvgIterTime(ms),Throughput(rows/s)");

            String csvResult = String.format("%d,%d,%d,%d,%.2f,%d,%.2f,%.2f",
                    totalIterations, totalRows, globalValidRows, globalDeletedRows,
                    deleteRate, totalTimeMs, avgIterTime, throughput);
            System.out.println(csvResult);

            reader.close();
        } catch (IOException e) {
            e.printStackTrace();
        }
    }
}