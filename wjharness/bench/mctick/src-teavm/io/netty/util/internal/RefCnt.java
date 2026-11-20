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
package io.netty.util.internal;

import io.netty.util.IllegalReferenceCountException;
import java.util.concurrent.atomic.AtomicIntegerFieldUpdater;
import static io.netty.util.internal.ObjectUtil.checkPositive;

/*
 * wjharness mctick (TeaVM arm only): netty 4.2.15's RefCnt with its AtomicIntegerFieldUpdater implementation and no
 * Unsafe / VarHandle ones, which TeaVM cannot link (it analyses every branch of the original's REF_CNT_IMPL switch).
 * Modified from the original; Apache License 2.0.
 */
public final class RefCnt {
    volatile int value;

    public RefCnt() {
        AtomicRefCnt.init(this);
    }

    public static int refCnt(RefCnt ref) {
        return AtomicRefCnt.refCnt(ref);
    }

    public static void retain(RefCnt ref) {
        AtomicRefCnt.retain(ref);
    }

    public static void retain(RefCnt ref, int increment) {
        AtomicRefCnt.retain(ref, increment);
    }

    public static boolean release(RefCnt ref) {
        return AtomicRefCnt.release(ref);
    }

    public static boolean release(RefCnt ref, int decrement) {
        return AtomicRefCnt.release(ref, decrement);
    }

    public static boolean isLiveNonVolatile(RefCnt ref) {
        return AtomicRefCnt.isLiveNonVolatile(ref);
    }

    public static void setRefCnt(RefCnt ref, int refCnt) {
        AtomicRefCnt.setRefCnt(ref, refCnt);
    }

    public static void resetRefCnt(RefCnt ref) {
        AtomicRefCnt.resetRefCnt(ref);
    }

    static void throwIllegalRefCountOnRelease(int decrement, int curr) {
        throw new IllegalReferenceCountException(curr >>> 1, -(decrement >>> 1));
    }

    private static final class AtomicRefCnt {
        private static final AtomicIntegerFieldUpdater<RefCnt> UPDATER =
                AtomicIntegerFieldUpdater.newUpdater(RefCnt.class, "value");

        static void init(RefCnt instance) {
            UPDATER.set(instance, 2);
        }

        static int refCnt(RefCnt instance) {
            return UPDATER.get(instance) >>> 1;
        }

        static void retain(RefCnt instance) {
            retain0(instance, 2);
        }

        static void retain(RefCnt instance, int increment) {
            retain0(instance, checkPositive(increment, "increment") << 1);
        }

        private static void retain0(RefCnt instance, int increment) {
            // oldRef & 0x80000001 stands for oldRef < 0 || oldRef is odd
            // NOTE: we're optimizing for inlined and constant folded increment here -> which will make
            // Integer.MAX_VALUE - increment to be computed at compile time
            int oldRef = UPDATER.getAndAdd(instance, increment);
            if ((oldRef & 0x80000001) != 0 || oldRef > Integer.MAX_VALUE - increment) {
                UPDATER.getAndAdd(instance, -increment);
                throw new IllegalReferenceCountException(0, increment >>> 1);
            }
        }

        static boolean release(RefCnt instance) {
            return release0(instance, 2);
        }

        static boolean release(RefCnt instance, int decrement) {
            return release0(instance, checkPositive(decrement, "decrement") << 1);
        }

        private static boolean release0(RefCnt instance, int decrement) {
            int curr, next;
            do {
                curr = instance.value;
                if (curr == decrement) {
                    next = 1;
                } else {
                    if (curr < decrement || (curr & 1) == 1) {
                        throwIllegalRefCountOnRelease(decrement, curr);
                    }
                    next = curr - decrement;
                }
            } while (!UPDATER.compareAndSet(instance, curr, next));
            return (next & 1) == 1;
        }

        static void setRefCnt(RefCnt instance, int refCnt) {
            int rawRefCnt = refCnt > 0? refCnt << 1 : 1;
            UPDATER.lazySet(instance, rawRefCnt);
        }

        static void resetRefCnt(RefCnt instance) {
            UPDATER.lazySet(instance, 2);
        }

        static boolean isLiveNonVolatile(RefCnt instance) {
            final int rawCnt = instance.value;
            if (rawCnt == 2) {
                return true;
            }
            return (rawCnt & 1) == 0;
        }
    }
}
