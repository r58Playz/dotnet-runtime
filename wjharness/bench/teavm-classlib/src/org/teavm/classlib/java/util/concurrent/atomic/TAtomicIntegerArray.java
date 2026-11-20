package org.teavm.classlib.java.util.concurrent.atomic;

import java.util.function.IntUnaryOperator;
import java.util.function.IntBinaryOperator;

/* java.util.concurrent.atomic.AtomicIntegerArray for one thread: a plain array. */
public class TAtomicIntegerArray {
    private final int[] array;

    public TAtomicIntegerArray(int length) {
        array = new int[length];
    }

    public TAtomicIntegerArray(int[] array) {
        this.array = array.clone();
    }

    public final int length() {
        return array.length;
    }

    public final int get(int i) {
        return array[i];
    }

    public final void set(int i, int v) {
        array[i] = v;
    }

    public final void lazySet(int i, int v) {
        array[i] = v;
    }

    public final int getPlain(int i) {
        return array[i];
    }

    public final void setPlain(int i, int v) {
        array[i] = v;
    }

    public final int getOpaque(int i) {
        return array[i];
    }

    public final void setOpaque(int i, int v) {
        array[i] = v;
    }

    public final int getAcquire(int i) {
        return array[i];
    }

    public final void setRelease(int i, int v) {
        array[i] = v;
    }

    public final int getAndSet(int i, int v) {
        int old = array[i];
        array[i] = v;
        return old;
    }

    public final boolean compareAndSet(int i, int expect, int update) {
        if (array[i] != expect) {
            return false;
        }
        array[i] = update;
        return true;
    }

    public final boolean weakCompareAndSet(int i, int expect, int update) {
        return compareAndSet(i, expect, update);
    }

    public final boolean weakCompareAndSetPlain(int i, int expect, int update) {
        return compareAndSet(i, expect, update);
    }

    public final boolean weakCompareAndSetVolatile(int i, int expect, int update) {
        return compareAndSet(i, expect, update);
    }

    public final int getAndUpdate(int i, IntUnaryOperator f) {
        int old = array[i];
        array[i] = f.applyAsInt(old);
        return old;
    }

    public final int updateAndGet(int i, IntUnaryOperator f) {
        return array[i] = f.applyAsInt(array[i]);
    }

    public final int getAndAccumulate(int i, int x, IntBinaryOperator f) {
        int old = array[i];
        array[i] = f.applyAsInt(old, x);
        return old;
    }

    public final int accumulateAndGet(int i, int x, IntBinaryOperator f) {
        return array[i] = f.applyAsInt(array[i], x);
    }

    public final int getAndIncrement(int i) {
        return array[i]++;
    }

    public final int getAndDecrement(int i) {
        return array[i]--;
    }

    public final int getAndAdd(int i, int delta) {
        int old = array[i];
        array[i] += delta;
        return old;
    }

    public final int incrementAndGet(int i) {
        return ++array[i];
    }

    public final int decrementAndGet(int i) {
        return --array[i];
    }

    public final int addAndGet(int i, int delta) {
        return array[i] += delta;
    }

    public final int compareAndExchange(int i, int expected, int newValue) {
        int witness = array[i];
        if (witness == expected) {
            array[i] = newValue;
        }
        return witness;
    }

    public final int compareAndExchangeAcquire(int i, int expected, int newValue) {
        return compareAndExchange(i, expected, newValue);
    }

    public final int compareAndExchangeRelease(int i, int expected, int newValue) {
        return compareAndExchange(i, expected, newValue);
    }

    @Override
    public String toString() {
        return java.util.Arrays.toString(array);
    }
}
