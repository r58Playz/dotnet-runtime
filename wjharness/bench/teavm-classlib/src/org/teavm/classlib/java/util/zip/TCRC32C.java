package org.teavm.classlib.java.util.zip;

import java.nio.ByteBuffer;
import java.util.zip.Checksum;

/* java.util.zip.CRC32C (Castagnoli), table-driven. */
public final class TCRC32C implements Checksum {
    private static final int[] TABLE = new int[256];

    static {
        for (int i = 0; i < 256; ++i) {
            int c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) != 0 ? (c >>> 1) ^ 0x82F63B78 : c >>> 1;
            }
            TABLE[i] = c;
        }
    }

    private int crc = 0xFFFFFFFF;

    @Override
    public void update(int b) {
        crc = (crc >>> 8) ^ TABLE[(crc ^ b) & 0xFF];
    }

    @Override
    public void update(byte[] b, int off, int len) {
        int c = crc;
        for (int i = off; i < off + len; ++i) {
            c = (c >>> 8) ^ TABLE[(c ^ b[i]) & 0xFF];
        }
        crc = c;
    }

    public void update(byte[] b) {
        update(b, 0, b.length);
    }

    public void update(ByteBuffer buffer) {
        while (buffer.hasRemaining()) {
            update(buffer.get());
        }
    }

    @Override
    public long getValue() {
        return (~crc) & 0xFFFFFFFFL;
    }

    @Override
    public void reset() {
        crc = 0xFFFFFFFF;
    }
}
