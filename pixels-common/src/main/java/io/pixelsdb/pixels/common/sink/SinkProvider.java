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
<<<<<<<< HEAD:pixels-daemon/src/main/java/io/pixelsdb/pixels/daemon/PixelsRetina.java
package io.pixelsdb.pixels.daemon;

/**
 * @author gengdy
 * @create 2025-01-21
 */
public class PixelsRetina
{
    public static void main(String[] args)
    {
        DaemonMain.main(args);
    }
========
package io.pixelsdb.pixels.common.sink;

import io.pixelsdb.pixels.common.utils.ConfigFactory;

import java.util.Properties;

public interface SinkProvider {
    void start(ConfigFactory config);
    void shutdown();
    boolean isRunning();
>>>>>>>> master:pixels-common/src/main/java/io/pixelsdb/pixels/common/sink/SinkProvider.java
}
