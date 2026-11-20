/*
 * Copyright 2012 The Netty Project
 *
 * The Netty Project licenses this file to you under the Apache License,
 * version 2.0 (the "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at:
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
package io.netty.util.internal;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayDeque;
import java.util.Collections;
import java.util.Deque;
import java.util.Map;
import java.util.Queue;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedDeque;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicLong;

/*
 * wjharness mctick (TeaVM arm only): netty 4.2.15's PlatformDependent as a portable facade. TeaVM analyses every
 * branch of the original, which reaches sun.misc.Unsafe, VarHandles, MethodHandles, JFR, jctools' Unsafe queues and
 * the direct-buffer cleaners; none of that links there. This one reports a platform with no Unsafe, no VarHandle and
 * no direct memory (netty then uses heap buffers and its safe code paths), keeps the original's safe helpers verbatim
 * (hashCodeAscii*, equalsSafe, ...), and throws from the raw-memory methods, which nothing reaches once hasUnsafe() is
 * false. Methods only the original's internals used are omitted; TeaVM names any that become reachable.
 * Modified from the original; Apache License 2.0.
 */
public final class PlatformDependent {
    public static final boolean BIG_ENDIAN_NATIVE_ORDER = ByteOrder.nativeOrder() == ByteOrder.BIG_ENDIAN;
    static final int HASH_CODE_ASCII_SEED = 0xc2b2ae35;
    static final int HASH_CODE_C1 = 0xcc9e2d51;
    static final int HASH_CODE_C2 = 0x1b873593;
    private static final Throwable UNSAFE_UNAVAILABILITY_CAUSE = new UnsupportedOperationException("no Unsafe on TeaVM");
    private static final AtomicLong DIRECT_MEMORY_COUNTER = new AtomicLong();

    private PlatformDependent() {
    }

    private static UnsupportedOperationException noRawMemory() {
        return new UnsupportedOperationException("raw memory access on TeaVM");
    }

    public static long byteArrayBaseOffset() {
        return -1;
    }

    public static boolean hasDirectBufferNoCleanerConstructor() {
        return false;
    }

    public static byte[] allocateUninitializedArray(int size) {
        return new byte[size];
    }

    public static boolean isAndroid() {
        return false;
    }

    public static boolean isWindows() {
        return false;
    }

    public static boolean isOsx() {
        return false;
    }

    public static boolean maybeSuperUser() {
        return false;
    }

    public static int javaVersion() {
        return 8;
    }

    public static boolean isVirtualThread(Thread thread) {
        return false;
    }

    public static boolean canEnableTcpNoDelayByDefault() {
        return true;
    }

    public static boolean hasUnsafe() {
        return false;
    }

    public static Throwable getUnsafeUnavailabilityCause() {
        return UNSAFE_UNAVAILABILITY_CAUSE;
    }

    public static boolean isUnaligned() {
        return false;
    }

    public static boolean directBufferPreferred() {
        return false;
    }

    public static boolean isExplicitNoPreferDirect() {
        return true;
    }

    public static boolean canReliabilyFreeDirectBuffers() {
        return false;
    }

    public static long maxDirectMemory() {
        return Long.MAX_VALUE;
    }

    public static long usedDirectMemory() {
        return DIRECT_MEMORY_COUNTER.get();
    }

    public static File tmpdir() {
        return new File("/tmp");
    }

    public static int bitMode() {
        return 32;
    }

    public static int addressSize() {
        return 4;
    }

    public static long allocateMemory(long size) {
        throw noRawMemory();
    }

    public static void freeMemory(long address) {
        throw noRawMemory();
    }

    public static long reallocateMemory(long address, long newSize) {
        throw noRawMemory();
    }

    public static void throwException(Throwable t) {
        PlatformDependent.<RuntimeException>throwException0(t);
    }

    @SuppressWarnings("unchecked")
    private static <E extends Throwable> void throwException0(Throwable t) throws E {
        throw (E) t;
    }

    public static <K, V> ConcurrentMap<K, V> newConcurrentHashMap() {
        return new ConcurrentHashMap<K, V>();
    }

    public static LongCounter newLongCounter() {
        return new LongCounter() {
            private long value;

            @Override
            public void add(long delta) {
                value += delta;
            }

            @Override
            public void increment() {
                ++value;
            }

            @Override
            public void decrement() {
                --value;
            }

            @Override
            public long value() {
                return value;
            }
        };
    }

    public static <K, V> ConcurrentMap<K, V> newConcurrentHashMap(int initialCapacity) {
        return new ConcurrentHashMap<K, V>(initialCapacity);
    }

    public static <K, V> ConcurrentMap<K, V> newConcurrentHashMap(int initialCapacity, float loadFactor) {
        return new ConcurrentHashMap<K, V>(initialCapacity, loadFactor);
    }

    public static <K, V> ConcurrentMap<K, V> newConcurrentHashMap(int initialCapacity, float loadFactor,
            int concurrencyLevel) {
        return new ConcurrentHashMap<K, V>(initialCapacity, loadFactor);
    }

    public static <K, V> ConcurrentMap<K, V> newConcurrentHashMap(Map<? extends K, ? extends V> map) {
        return new ConcurrentHashMap<K, V>(map);
    }

    public static CleanableDirectBuffer allocateDirect(int capacity) {
        throw noRawMemory();
    }

    public static CleanableDirectBuffer allocateDirect(int capacity, boolean useCleaner) {
        throw noRawMemory();
    }

    public static CleanableDirectBuffer reallocateDirect(CleanableDirectBuffer buffer, int capacity) {
        throw noRawMemory();
    }

    public static void freeDirectBuffer(ByteBuffer buffer) {
    }

    public static boolean hasDirectByteBufferAddress(ByteBuffer buffer) {
        return false;
    }

    public static long directBufferAddress(ByteBuffer buffer) {
        throw noRawMemory();
    }

    public static ByteBuffer directBuffer(long memoryAddress, int size) {
        throw noRawMemory();
    }

    public static boolean hasVarHandle() {
        return false;
    }

    public static boolean useVarHandleForMultiByteAccess() {
        return false;
    }

    public static boolean canUnalignedAccess() {
        return false;
    }

    public static Object getObject(Object object, long fieldOffset) {
        throw noRawMemory();
    }

    public static int getVolatileInt(Object object, long fieldOffset) {
        throw noRawMemory();
    }

    public static int getInt(Object object, long fieldOffset) {
        throw noRawMemory();
    }

    public static void putOrderedInt(Object object, long fieldOffset, int value) {
        throw noRawMemory();
    }

    public static int getAndAddInt(Object object, long fieldOffset, int delta) {
        throw noRawMemory();
    }

    public static boolean compareAndSwapInt(Object object, long fieldOffset, int expected, int value) {
        throw noRawMemory();
    }

    public static byte getByte(long address) {
        throw noRawMemory();
    }

    public static short getShort(long address) {
        throw noRawMemory();
    }

    public static int getInt(long address) {
        throw noRawMemory();
    }

    public static long getLong(long address) {
        throw noRawMemory();
    }

    public static byte getByte(byte[] data, int index) {
        return data[index];
    }

    public static byte getByte(byte[] data, long index) {
        return data[(int) index];
    }

    public static short getShort(byte[] data, int index) {
        return getShortSafe(data, index);
    }

    public static int getInt(byte[] data, int index) {
        return getIntSafe(data, index);
    }

    public static int getInt(int[] data, long index) {
        return data[(int) index];
    }

    public static long getLong(byte[] data, int index) {
        return getLongSafe(data, index);
    }

    public static long getLong(long[] data, long index) {
        return data[(int) index];
    }

    public static void putByte(long address, byte value) {
        throw noRawMemory();
    }

    public static void putShort(long address, short value) {
        throw noRawMemory();
    }

    public static void putInt(long address, int value) {
        throw noRawMemory();
    }

    public static void putLong(long address, long value) {
        throw noRawMemory();
    }

    public static void putByte(byte[] data, int index, byte value) {
        data[index] = value;
    }

    public static void putByte(Object data, long offset, byte value) {
        throw noRawMemory();
    }

    public static void putShort(byte[] data, int index, short value) {
        throw noRawMemory();
    }

    public static void putInt(byte[] data, int index, int value) {
        throw noRawMemory();
    }

    public static void putLong(byte[] data, int index, long value) {
        throw noRawMemory();
    }

    public static void putObject(Object o, long offset, Object x) {
        throw noRawMemory();
    }

    public static long objectFieldOffset(Field field) {
        throw noRawMemory();
    }

    public static void copyMemory(long srcAddr, long dstAddr, long length) {
        throw noRawMemory();
    }

    public static void copyMemory(byte[] src, int srcIndex, long dstAddr, long length) {
        throw noRawMemory();
    }

    public static void copyMemory(byte[] src, int srcIndex, byte[] dst, int dstIndex, long length) {
        System.arraycopy(src, srcIndex, dst, dstIndex, (int) length);
    }

    public static void copyMemory(long srcAddr, byte[] dst, int dstIndex, long length) {
        throw noRawMemory();
    }

    public static void setMemory(byte[] dst, int dstIndex, long bytes, byte value) {
        java.util.Arrays.fill(dst, dstIndex, (int) (dstIndex + bytes), value);
    }

    public static void setMemory(long address, long bytes, byte value) {
        throw noRawMemory();
    }

    public static boolean hasAlignDirectByteBuffer() {
        return false;
    }

    public static ByteBuffer alignDirectBuffer(ByteBuffer buffer, int alignment) {
        throw noRawMemory();
    }

    public static long align(long value, int alignment) {
        return (value + alignment - 1) & -alignment;
    }

    public static ByteBuffer offsetSlice(ByteBuffer buffer, int index, int length) {
        ByteBuffer d = buffer.duplicate();
        d.clear().position(index).limit(index + length);
        return d.slice();
    }

    public static ByteBuffer absolutePut(ByteBuffer dst, int dstOffset, byte[] src, int srcOffset, int length) {
        ByteBuffer d = dst.duplicate();
        d.clear().position(dstOffset);
        d.put(src, srcOffset, length);
        return dst;
    }

    public static ByteBuffer absolutePut(ByteBuffer dst, int dstOffset, ByteBuffer src, int srcOffset, int length) {
        ByteBuffer d = dst.duplicate();
        d.clear().position(dstOffset);
        ByteBuffer s = src.duplicate();
        s.clear().position(srcOffset).limit(srcOffset + length);
        d.put(s);
        return dst;
    }

    public static boolean useDirectBufferNoCleaner() {
        return false;
    }

    public static boolean equals(byte[] bytes1, int startPos1, byte[] bytes2, int startPos2, int length) {
        return equalsSafe(bytes1, startPos1, bytes2, startPos2, length);
    }

    public static boolean isZero(byte[] bytes, int startPos, int length) {
        return isZeroSafe(bytes, startPos, length);
    }

    public static int equalsConstantTime(byte[] bytes1, int startPos1, byte[] bytes2, int startPos2, int length) {
        return ConstantTimeUtils.equalsConstantTime(bytes1, startPos1, bytes2, startPos2, length);
    }

    public static int hashCodeAscii(byte[] bytes, int startPos, int length) {
        return hashCodeAsciiSafe(bytes, startPos, length);
    }

    public static <T> Queue<T> newMpscQueue() {
        return new ConcurrentLinkedQueue<T>();
    }

    public static <T> Queue<T> newMpscQueue(int maxCapacity) {
        return new ConcurrentLinkedQueue<T>();
    }

    public static <T> Queue<T> newMpscQueue(int chunkSize, int maxCapacity) {
        return new ConcurrentLinkedQueue<T>();
    }

    public static <T> Queue<T> newSpscQueue() {
        return new ConcurrentLinkedQueue<T>();
    }

    public static <T> Queue<T> newFixedMpscQueue(int capacity) {
        return new ConcurrentLinkedQueue<T>();
    }

    public static <T> Queue<T> newFixedMpscUnpaddedQueue(int capacity) {
        return new ConcurrentLinkedQueue<T>();
    }

    public static <T> Queue<T> newFixedMpmcQueue(int capacity) {
        return new ConcurrentLinkedQueue<T>();
    }

    public static ClassLoader getClassLoader(Class<?> clazz) {
        return clazz.getClassLoader();
    }

    public static ClassLoader getContextClassLoader() {
        return ClassLoader.getSystemClassLoader();
    }

    public static ClassLoader getSystemClassLoader() {
        return ClassLoader.getSystemClassLoader();
    }

    public static <C> Deque<C> newConcurrentDeque() {
        return new ConcurrentLinkedDeque<C>();
    }

    public static Random threadLocalRandom() {
        return java.util.concurrent.ThreadLocalRandom.current();
    }

    public static boolean isJ9Jvm() {
        return false;
    }

    public static boolean isIkvmDotNet() {
        return false;
    }

    public static long estimateMaxDirectMemory() {
        return 0;
    }

    public static String normalizedArch() {
        return "wasm32";
    }

    public static String normalizedOs() {
        return "unknown";
    }

    public static Set<String> normalizedLinuxClassifiers() {
        return Collections.emptySet();
    }

    public static File createTempFile(String prefix, String suffix, File directory) throws IOException {
        return File.createTempFile(prefix, suffix, directory);
    }

    public static boolean isJfrEnabled() {
        return false;
    }

    // ---- the original's safe helpers, verbatim (hashCodeAsciiComputeLong is PlatformDependent0.hashCodeAsciiCompute)

    private static int hashCodeAsciiComputeLong(long value, int hash) {
        return hash * HASH_CODE_C1 +
                hashCodeAsciiSanitize((int) value) * HASH_CODE_C2 +
                (int) ((value & 0x1f1f1f1f00000000L) >>> 32);
    }

    public static int hashCodeAscii(CharSequence bytes) {
        final int length = bytes.length();
        final int remainingBytes = length & 7;
        int hash = HASH_CODE_ASCII_SEED;
        // Benchmarking shows that by just naively looping for inputs 8~31 bytes long we incur a relatively large
        // performance penalty (only achieve about 60% performance of loop which iterates over each char). So because
        // of this we take special provisions to unroll the looping for these conditions.
        if (length >= 32) {
            for (int i = length - 8; i >= remainingBytes; i -= 8) {
                hash = hashCodeAsciiCompute(bytes, i, hash);
            }
        } else if (length >= 8) {
            hash = hashCodeAsciiCompute(bytes, length - 8, hash);
            if (length >= 16) {
                hash = hashCodeAsciiCompute(bytes, length - 16, hash);
                if (length >= 24) {
                    hash = hashCodeAsciiCompute(bytes, length - 24, hash);
                }
            }
        }
        if (remainingBytes == 0) {
            return hash;
        }
        int offset = 0;
        if (remainingBytes != 2 & remainingBytes != 4 & remainingBytes != 6) { // 1, 3, 5, 7
            hash = hash * HASH_CODE_C1 + hashCodeAsciiSanitizeByte(bytes.charAt(0));
            offset = 1;
        }
        if (remainingBytes != 1 & remainingBytes != 4 & remainingBytes != 5) { // 2, 3, 6, 7
            hash = hash * (offset == 0 ? HASH_CODE_C1 : HASH_CODE_C2)
                    + hashCodeAsciiSanitize(hashCodeAsciiSanitizeShort(bytes, offset));
            offset += 2;
        }
        if (remainingBytes >= 4) { // 4, 5, 6, 7
            return hash * ((offset == 0 | offset == 3) ? HASH_CODE_C1 : HASH_CODE_C2)
                    + hashCodeAsciiSanitizeInt(bytes, offset);
        }
        return hash;
    }

    private static int hashCodeAsciiCompute(CharSequence value, int offset, int hash) {
        if (BIG_ENDIAN_NATIVE_ORDER) {
            return hash * HASH_CODE_C1 +
                    // Low order int
                    hashCodeAsciiSanitizeInt(value, offset + 4) * HASH_CODE_C2 +
                    // High order int
                    hashCodeAsciiSanitizeInt(value, offset);
        }
        return hash * HASH_CODE_C1 +
                // Low order int
                hashCodeAsciiSanitizeInt(value, offset) * HASH_CODE_C2 +
                // High order int
                hashCodeAsciiSanitizeInt(value, offset + 4);
    }

    private static int hashCodeAsciiSanitizeInt(CharSequence value, int offset) {
        if (BIG_ENDIAN_NATIVE_ORDER) {
            // mimic a unsafe.getInt call on a big endian machine
            return (value.charAt(offset + 3) & 0x1f) |
                   (value.charAt(offset + 2) & 0x1f) << 8 |
                   (value.charAt(offset + 1) & 0x1f) << 16 |
                   (value.charAt(offset) & 0x1f) << 24;
        }
        return (value.charAt(offset + 3) & 0x1f) << 24 |
               (value.charAt(offset + 2) & 0x1f) << 16 |
               (value.charAt(offset + 1) & 0x1f) << 8 |
               (value.charAt(offset) & 0x1f);
    }

    private static int hashCodeAsciiSanitizeShort(CharSequence value, int offset) {
        if (BIG_ENDIAN_NATIVE_ORDER) {
            // mimic a unsafe.getShort call on a big endian machine
            return (value.charAt(offset + 1) & 0x1f) |
                    (value.charAt(offset) & 0x1f) << 8;
        }
        return (value.charAt(offset + 1) & 0x1f) << 8 |
                (value.charAt(offset) & 0x1f);
    }

    private static int hashCodeAsciiSanitizeByte(char value) {
        return value & 0x1f;
    }

    static int hashCodeAsciiSafe(byte[] bytes, int startPos, int length) {
        int hash = HASH_CODE_ASCII_SEED;
        final int remainingBytes = length & 7;
        final int end = startPos + remainingBytes;
        for (int i = startPos - 8 + length; i >= end; i -= 8) {
            hash = hashCodeAsciiComputeLong(getLongSafe(bytes, i), hash);
        }
        switch(remainingBytes) {
        case 7:
            return ((hash * HASH_CODE_C1 + hashCodeAsciiSanitize(bytes[startPos]))
                          * HASH_CODE_C2 + hashCodeAsciiSanitize(getShortSafe(bytes, startPos + 1)))
                          * HASH_CODE_C1 + hashCodeAsciiSanitize(getIntSafe(bytes, startPos + 3));
        case 6:
            return (hash * HASH_CODE_C1 + hashCodeAsciiSanitize(getShortSafe(bytes, startPos)))
                         * HASH_CODE_C2 + hashCodeAsciiSanitize(getIntSafe(bytes, startPos + 2));
        case 5:
            return (hash * HASH_CODE_C1 + hashCodeAsciiSanitize(bytes[startPos]))
                         * HASH_CODE_C2 + hashCodeAsciiSanitize(getIntSafe(bytes, startPos + 1));
        case 4:
            return hash * HASH_CODE_C1 + hashCodeAsciiSanitize(getIntSafe(bytes, startPos));
        case 3:
            return (hash * HASH_CODE_C1 + hashCodeAsciiSanitize(bytes[startPos]))
                         * HASH_CODE_C2 + hashCodeAsciiSanitize(getShortSafe(bytes, startPos + 1));
        case 2:
            return hash * HASH_CODE_C1 + hashCodeAsciiSanitize(getShortSafe(bytes, startPos));
        case 1:
            return hash * HASH_CODE_C1 + hashCodeAsciiSanitize(bytes[startPos]);
        default:
            return hash;
        }
    }

    private static boolean equalsSafe(byte[] bytes1, int startPos1, byte[] bytes2, int startPos2, int length) {
        final int end = startPos1 + length;
        for (; startPos1 < end; ++startPos1, ++startPos2) {
            if (bytes1[startPos1] != bytes2[startPos2]) {
                return false;
            }
        }
        return true;
    }

    private static boolean isZeroSafe(byte[] bytes, int startPos, int length) {
        final int end = startPos + length;
        for (; startPos < end; ++startPos) {
            if (bytes[startPos] != 0) {
                return false;
            }
        }
        return true;
    }

    private static int getIntSafe(byte[] bytes, int offset) {
        if (BIG_ENDIAN_NATIVE_ORDER) {
            return bytes[offset] << 24 |
                    (bytes[offset + 1] & 0xff) << 16 |
                    (bytes[offset + 2] & 0xff) << 8 |
                    bytes[offset + 3] & 0xff;
        }
        return bytes[offset] & 0xff |
                (bytes[offset + 1] & 0xff) << 8 |
                (bytes[offset + 2] & 0xff) << 16 |
                bytes[offset + 3] << 24;
    }

    private static long getLongSafe(byte[] bytes, int offset) {
        if (BIG_ENDIAN_NATIVE_ORDER) {
            return (long) bytes[offset] << 56 |
                    ((long) bytes[offset + 1] & 0xff) << 48 |
                    ((long) bytes[offset + 2] & 0xff) << 40 |
                    ((long) bytes[offset + 3] & 0xff) << 32 |
                    ((long) bytes[offset + 4] & 0xff) << 24 |
                    ((long) bytes[offset + 5] & 0xff) << 16 |
                    ((long) bytes[offset + 6] & 0xff) <<  8 |
                    (long) bytes[offset + 7] & 0xff;
        }
        return (long) bytes[offset] & 0xff |
                ((long) bytes[offset + 1] & 0xff) << 8 |
                ((long) bytes[offset + 2] & 0xff) << 16 |
                ((long) bytes[offset + 3] & 0xff) << 24 |
                ((long) bytes[offset + 4] & 0xff) << 32 |
                ((long) bytes[offset + 5] & 0xff) << 40 |
                ((long) bytes[offset + 6] & 0xff) << 48 |
                (long) bytes[offset + 7] << 56;
    }

    private static short getShortSafe(byte[] bytes, int offset) {
        if (BIG_ENDIAN_NATIVE_ORDER) {
            return (short) (bytes[offset] << 8 | (bytes[offset + 1] & 0xff));
        }
        return (short) (bytes[offset] & 0xff | (bytes[offset + 1] << 8));
    }

    static int hashCodeAsciiSanitize(int value) {
        return value & 0x1f1f1f1f;
    }

    static int hashCodeAsciiSanitize(short value) {
        return value & 0x1f1f;
    }

    static int hashCodeAsciiSanitize(byte value) {
        return value & 0x1f;
    }
}
