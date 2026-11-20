package org.teavm.classlib.java.util.concurrent.atomic;

import java.util.function.LongUnaryOperator;
import java.util.function.LongBinaryOperator;

/* java.util.concurrent.atomic.AtomicLongArray for one thread: a plain array. */
public class TAtomicLongArray {
    private final long[] array;

    public TAtomicLongArray(int length) {
        array = new long[length];
    }

    public TAtomicLongArray(long[] array) {
        this.array = array.clone();
    }

    public final int length() {
        return array.length;
    }

    public final long get(int i) {
        return array[i];
    }

    public final void set(int i, long v) {
        array[i] = v;
    }

    public final void lazySet(int i, long v) {
        array[i] = v;
    }

    public final long getPlain(int i) {
        return array[i];
    }

    public final void setPlain(int i, long v) {
        array[i] = v;
    }

    public final long getOpaque(int i) {
        return array[i];
    }

    public final void setOpaque(int i, long v) {
        array[i] = v;
    }

    public final long getAcquire(int i) {
        return array[i];
    }

    public final void setRelease(int i, long v) {
        array[i] = v;
    }

    public final long getAndSet(int i, long v) {
        long old = array[i];
        array[i] = v;
        return old;
    }

    public final boolean compareAndSet(int i, long expect, long update) {
        if (array[i] != expect) {
            return false;
        }
        array[i] = update;
        return true;
    }

    public final boolean weakCompareAndSet(int i, long expect, long update) {
        return compareAndSet(i, expect, update);
    }

    public final boolean weakCompareAndSetPlain(int i, long expect, long update) {
        return compareAndSet(i, expect, update);
    }

    public final boolean weakCompareAndSetVolatile(int i, long expect, long update) {
        return compareAndSet(i, expect, update);
    }

    public final long getAndUpdate(int i, LongUnaryOperator f) {
        long old = array[i];
        array[i] = f.applyAsLong(old);
        return old;
    }

    public final long updateAndGet(int i, LongUnaryOperator f) {
        return array[i] = f.applyAsLong(array[i]);
    }

    public final long getAndAccumulate(int i, long x, LongBinaryOperator f) {
        long old = array[i];
        array[i] = f.applyAsLong(old, x);
        return old;
    }

    public final long accumulateAndGet(int i, long x, LongBinaryOperator f) {
        return array[i] = f.applyAsLong(array[i], x);
    }

    public final long getAndIncrement(int i) {
        return array[i]++;
    }

    public final long getAndDecrement(int i) {
        return array[i]--;
    }

    public final long getAndAdd(int i, long delta) {
        long old = array[i];
        array[i] += delta;
        return old;
    }

    public final long incrementAndGet(int i) {
        return ++array[i];
    }

    public final long decrementAndGet(int i) {
        return --array[i];
    }

    public final long addAndGet(int i, long delta) {
        return array[i] += delta;
    }

    public final long compareAndExchange(int i, long expected, long newValue) {
        long witness = array[i];
        if (witness == expected) {
            array[i] = newValue;
        }
        return witness;
    }

    public final long compareAndExchangeAcquire(int i, long expected, long newValue) {
        return compareAndExchange(i, expected, newValue);
    }

    public final long compareAndExchangeRelease(int i, long expected, long newValue) {
        return compareAndExchange(i, expected, newValue);
    }

    @Override
    public String toString() {
        return java.util.Arrays.toString(array);
    }
}
