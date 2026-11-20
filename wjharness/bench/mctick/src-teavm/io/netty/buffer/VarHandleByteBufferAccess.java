/*
 * Copyright 2025 The Netty Project
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
package io.netty.buffer;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/*
 * wjharness mctick (TeaVM arm only): netty 4.2.15's VarHandleByteBufferAccess over ByteBuffer views, because TeaVM
 * has no VarHandle. Never called there (the PlatformDependent shadow reports no VarHandle, so HeapByteBufUtil takes
 * its shift-based paths); it exists so the branches that would call it link. Rewritten; Apache License 2.0.
 */
final class VarHandleByteBufferAccess {
    private VarHandleByteBufferAccess() {
    }

    static short getShortBE(ByteBuffer buffer, int index) {
        return buffer.duplicate().order(ByteOrder.BIG_ENDIAN).getShort(index);
    }

    static void setShortBE(ByteBuffer buffer, int index, int value) {
        buffer.duplicate().order(ByteOrder.BIG_ENDIAN).putShort(index, (short) value);
    }

    static short getShortBE(byte[] memory, int index) {
        return ByteBuffer.wrap(memory).order(ByteOrder.BIG_ENDIAN).getShort(index);
    }

    static void setShortBE(byte[] memory, int index, int value) {
        ByteBuffer.wrap(memory).order(ByteOrder.BIG_ENDIAN).putShort(index, (short) value);
    }

    static short getShortLE(ByteBuffer buffer, int index) {
        return buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN).getShort(index);
    }

    static void setShortLE(ByteBuffer buffer, int index, int value) {
        buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN).putShort(index, (short) value);
    }

    static short getShortLE(byte[] memory, int index) {
        return ByteBuffer.wrap(memory).order(ByteOrder.LITTLE_ENDIAN).getShort(index);
    }

    static void setShortLE(byte[] memory, int index, int value) {
        ByteBuffer.wrap(memory).order(ByteOrder.LITTLE_ENDIAN).putShort(index, (short) value);
    }

    static int getIntBE(ByteBuffer buffer, int index) {
        return buffer.duplicate().order(ByteOrder.BIG_ENDIAN).getInt(index);
    }

    static void setIntBE(ByteBuffer buffer, int index, int value) {
        buffer.duplicate().order(ByteOrder.BIG_ENDIAN).putInt(index, (int) value);
    }

    static int getIntBE(byte[] memory, int index) {
        return ByteBuffer.wrap(memory).order(ByteOrder.BIG_ENDIAN).getInt(index);
    }

    static void setIntBE(byte[] memory, int index, int value) {
        ByteBuffer.wrap(memory).order(ByteOrder.BIG_ENDIAN).putInt(index, (int) value);
    }

    static int getIntLE(ByteBuffer buffer, int index) {
        return buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN).getInt(index);
    }

    static void setIntLE(ByteBuffer buffer, int index, int value) {
        buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN).putInt(index, (int) value);
    }

    static int getIntLE(byte[] memory, int index) {
        return ByteBuffer.wrap(memory).order(ByteOrder.LITTLE_ENDIAN).getInt(index);
    }

    static void setIntLE(byte[] memory, int index, int value) {
        ByteBuffer.wrap(memory).order(ByteOrder.LITTLE_ENDIAN).putInt(index, (int) value);
    }

    static long getLongBE(ByteBuffer buffer, int index) {
        return buffer.duplicate().order(ByteOrder.BIG_ENDIAN).getLong(index);
    }

    static void setLongBE(ByteBuffer buffer, int index, long value) {
        buffer.duplicate().order(ByteOrder.BIG_ENDIAN).putLong(index, (long) value);
    }

    static long getLongBE(byte[] memory, int index) {
        return ByteBuffer.wrap(memory).order(ByteOrder.BIG_ENDIAN).getLong(index);
    }

    static void setLongBE(byte[] memory, int index, long value) {
        ByteBuffer.wrap(memory).order(ByteOrder.BIG_ENDIAN).putLong(index, (long) value);
    }

    static long getLongLE(ByteBuffer buffer, int index) {
        return buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN).getLong(index);
    }

    static void setLongLE(ByteBuffer buffer, int index, long value) {
        buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN).putLong(index, (long) value);
    }

    static long getLongLE(byte[] memory, int index) {
        return ByteBuffer.wrap(memory).order(ByteOrder.LITTLE_ENDIAN).getLong(index);
    }

    static void setLongLE(byte[] memory, int index, long value) {
        ByteBuffer.wrap(memory).order(ByteOrder.LITTLE_ENDIAN).putLong(index, (long) value);
    }
}
