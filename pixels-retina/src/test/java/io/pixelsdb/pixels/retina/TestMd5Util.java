package io.pixelsdb.pixels.retina;

import org.junit.jupiter.api.Test;

import java.nio.ByteBuffer;

public class TestMd5Util
{
    @Test
    public void testMd5Util()
    {
        ByteBuffer byteBuffer = ByteBuffer.allocate(1024);
        for(int i = 0; i < 100 ;i ++)
        {
            byteBuffer.putInt(i);
        }
        byteBuffer.rewind();
        byte[] array = byteBuffer.array();
        String md5ByteBuffer = Md5Util.md5(byteBuffer);
        String md5Array = Md5Util.md5(array);

        System.out.println(md5ByteBuffer);
        System.out.println(md5Array);

    }
}
