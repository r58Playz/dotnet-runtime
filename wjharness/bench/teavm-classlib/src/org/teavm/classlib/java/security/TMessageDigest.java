package org.teavm.classlib.java.security;

import java.nio.ByteBuffer;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;

/* java.security.MessageDigest with MD5, SHA-1 and SHA-256 (what Guava's Hashing and UUID.nameUUIDFromBytes ask for). */
public abstract class TMessageDigest {
    private final String algorithm;

    protected TMessageDigest(String algorithm) {
        this.algorithm = algorithm;
    }

    public static TMessageDigest getInstance(String algorithm) throws NoSuchAlgorithmException {
        switch (algorithm.toUpperCase()) {
            case "MD5":
                return new Md(algorithm, 0);
            case "SHA-1":
            case "SHA1":
            case "SHA":
                return new Md(algorithm, 1);
            case "SHA-256":
            case "SHA256":
                return new Md(algorithm, 2);
            default:
                throw new NoSuchAlgorithmException(algorithm + " MessageDigest not available");
        }
    }

    public static TMessageDigest getInstance(String algorithm, String provider) throws NoSuchAlgorithmException {
        return getInstance(algorithm);
    }

    public final String getAlgorithm() {
        return algorithm;
    }

    public final int getDigestLength() {
        return engineGetDigestLength();
    }

    public void update(byte input) {
        engineUpdate(new byte[] { input }, 0, 1);
    }

    public void update(byte[] input) {
        engineUpdate(input, 0, input.length);
    }

    public void update(byte[] input, int offset, int len) {
        engineUpdate(input, offset, len);
    }

    public final void update(ByteBuffer input) {
        byte[] b = new byte[input.remaining()];
        input.get(b);
        engineUpdate(b, 0, b.length);
    }

    public byte[] digest() {
        byte[] d = engineDigest();
        engineReset();
        return d;
    }

    public byte[] digest(byte[] input) {
        update(input);
        return digest();
    }

    public int digest(byte[] buf, int offset, int len) {
        byte[] d = digest();
        System.arraycopy(d, 0, buf, offset, d.length);
        return d.length;
    }

    public void reset() {
        engineReset();
    }

    public static boolean isEqual(byte[] a, byte[] b) {
        return Arrays.equals(a, b);
    }

    @Override
    public Object clone() throws CloneNotSupportedException {
        throw new CloneNotSupportedException();
    }

    protected abstract int engineGetDigestLength();

    protected abstract void engineUpdate(byte[] input, int offset, int len);

    protected abstract byte[] engineDigest();

    protected abstract void engineReset();

    /* MD5 (kind 0, little-endian), SHA-1 (1) and SHA-256 (2) over one 64-byte block buffer. */
    private static final class Md extends TMessageDigest {
        private static final int[] K256 = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        private static final int[] S5 = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
        private static final int[] K5 = new int[64];

        static {
            for (int i = 0; i < 64; ++i) {
                K5[i] = (int) (long) Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296.0);
            }
        }

        private final int kind;
        private final int[] h = new int[8];
        private final byte[] block = new byte[64];
        private int blockLen;
        private long total;

        Md(String algorithm, int kind) {
            super(algorithm);
            this.kind = kind;
            engineReset();
        }

        @Override
        protected int engineGetDigestLength() {
            return kind == 0 ? 16 : kind == 1 ? 20 : 32;
        }

        @Override
        protected void engineReset() {
            blockLen = 0;
            total = 0;
            if (kind == 2) {
                int[] iv = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                    0x5be0cd19 };
                System.arraycopy(iv, 0, h, 0, 8);
            } else {
                h[0] = 0x67452301;
                h[1] = 0xefcdab89;
                h[2] = 0x98badcfe;
                h[3] = 0x10325476;
                h[4] = 0xc3d2e1f0;
            }
        }

        @Override
        protected void engineUpdate(byte[] input, int offset, int len) {
            total += len;
            while (len > 0) {
                int n = Math.min(64 - blockLen, len);
                System.arraycopy(input, offset, block, blockLen, n);
                blockLen += n;
                offset += n;
                len -= n;
                if (blockLen == 64) {
                    compress();
                    blockLen = 0;
                }
            }
        }

        @Override
        protected byte[] engineDigest() {
            long bits = total * 8;
            byte[] pad = new byte[(blockLen < 56 ? 56 : 120) - blockLen + 8];
            pad[0] = (byte) 0x80;
            for (int i = 0; i < 8; ++i) {
                pad[pad.length - 8 + i] = (byte) (kind == 0 ? bits >>> (8 * i) : bits >>> (56 - 8 * i));
            }
            long t = total;
            engineUpdate(pad, 0, pad.length);
            total = t;
            int n = engineGetDigestLength();
            byte[] out = new byte[n];
            for (int i = 0; i < n; ++i) {
                int w = h[i / 4];
                out[i] = (byte) (kind == 0 ? w >>> (8 * (i % 4)) : w >>> (24 - 8 * (i % 4)));
            }
            return out;
        }

        private int be(int i) {
            return (block[i] & 255) << 24 | (block[i + 1] & 255) << 16 | (block[i + 2] & 255) << 8 | (block[i + 3] & 255);
        }

        private int le(int i) {
            return (block[i] & 255) | (block[i + 1] & 255) << 8 | (block[i + 2] & 255) << 16 | (block[i + 3] & 255) << 24;
        }

        private void compress() {
            if (kind == 0) {
                int[] m = new int[16];
                for (int i = 0; i < 16; ++i) {
                    m[i] = le(i * 4);
                }
                int a = h[0];
                int b = h[1];
                int c = h[2];
                int d = h[3];
                for (int i = 0; i < 64; ++i) {
                    int f;
                    int g;
                    if (i < 16) {
                        f = (b & c) | (~b & d);
                        g = i;
                    } else if (i < 32) {
                        f = (d & b) | (~d & c);
                        g = (5 * i + 1) % 16;
                    } else if (i < 48) {
                        f = b ^ c ^ d;
                        g = (3 * i + 5) % 16;
                    } else {
                        f = c ^ (b | ~d);
                        g = (7 * i) % 16;
                    }
                    int tmp = d;
                    d = c;
                    c = b;
                    b = b + Integer.rotateLeft(a + f + K5[i] + m[g], S5[(i / 16) * 4 + i % 4]);
                    a = tmp;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
            } else if (kind == 1) {
                int[] w = new int[80];
                for (int i = 0; i < 16; ++i) {
                    w[i] = be(i * 4);
                }
                for (int i = 16; i < 80; ++i) {
                    w[i] = Integer.rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                }
                int a = h[0];
                int b = h[1];
                int c = h[2];
                int d = h[3];
                int e = h[4];
                for (int i = 0; i < 80; ++i) {
                    int f;
                    int k;
                    if (i < 20) {
                        f = (b & c) | (~b & d);
                        k = 0x5a827999;
                    } else if (i < 40) {
                        f = b ^ c ^ d;
                        k = 0x6ed9eba1;
                    } else if (i < 60) {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8f1bbcdc;
                    } else {
                        f = b ^ c ^ d;
                        k = 0xca62c1d6;
                    }
                    int tmp = Integer.rotateLeft(a, 5) + f + e + k + w[i];
                    e = d;
                    d = c;
                    c = Integer.rotateLeft(b, 30);
                    b = a;
                    a = tmp;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
            } else {
                int[] w = new int[64];
                for (int i = 0; i < 16; ++i) {
                    w[i] = be(i * 4);
                }
                for (int i = 16; i < 64; ++i) {
                    int s0 = Integer.rotateRight(w[i - 15], 7) ^ Integer.rotateRight(w[i - 15], 18) ^ (w[i - 15] >>> 3);
                    int s1 = Integer.rotateRight(w[i - 2], 17) ^ Integer.rotateRight(w[i - 2], 19) ^ (w[i - 2] >>> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                int a = h[0];
                int b = h[1];
                int c = h[2];
                int d = h[3];
                int e = h[4];
                int f = h[5];
                int g = h[6];
                int hh = h[7];
                for (int i = 0; i < 64; ++i) {
                    int s1 = Integer.rotateRight(e, 6) ^ Integer.rotateRight(e, 11) ^ Integer.rotateRight(e, 25);
                    int ch = (e & f) ^ (~e & g);
                    int t1 = hh + s1 + ch + K256[i] + w[i];
                    int s0 = Integer.rotateRight(a, 2) ^ Integer.rotateRight(a, 13) ^ Integer.rotateRight(a, 22);
                    int maj = (a & b) ^ (a & c) ^ (b & c);
                    int t2 = s0 + maj;
                    hh = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
                h[5] += f;
                h[6] += g;
                h[7] += hh;
            }
        }
    }
}
