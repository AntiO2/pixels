package io.pixelsdb.pixels.index.rocksdb;

import org.rocksdb.AbstractComparator;
import org.rocksdb.ComparatorOptions;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class TimeStampComparator extends AbstractComparator {
    private final int timestampSize;
    private final ByteOrder order;

    public TimeStampComparator(ComparatorOptions options, int timestampSize) {
        super(options);
        this.timestampSize = timestampSize;
        this.order = ByteOrder.BIG_ENDIAN;
    }

    @Override
    public String name() {
        return "TimestampComparator";
    }

    @Override
    public int compare(ByteBuffer a, ByteBuffer b) {
        int ret = compareWithoutTimestamp(a, b);
        if (ret != 0) {
            return ret;
        }

        long tsA = extractTimestamp(a);
        long tsB = extractTimestamp(b);

        return Long.compare(tsB, tsA);
    }

    private int compareWithoutTimestamp(ByteBuffer a, ByteBuffer b) {
        int userKeyLenA = a.remaining() - timestampSize;
        int userKeyLenB = b.remaining() - timestampSize;
        int minLen = Math.min(userKeyLenA, userKeyLenB);

        for (int i = 0; i < minLen; i++) {
            int ba = a.get(a.position() + i) & 0xFF;
            int bb = b.get(b.position() + i) & 0xFF;
            if (ba != bb) {
                return ba - bb;
            }
        }
        return Integer.compare(userKeyLenA, userKeyLenB);
    }

    private long extractTimestamp(ByteBuffer buf) {
        int pos = buf.position() + buf.remaining() - timestampSize;
        ByteBuffer tsBuf = buf.duplicate();
        tsBuf.position(pos);
        tsBuf.order(order);
        return tsBuf.getLong();
    }
}
