package org.teavm.classlib.java.util.concurrent.atomic;

import java.util.function.UnaryOperator;
import java.util.function.BinaryOperator;

/* java.util.concurrent.atomic.AtomicReferenceArray for one thread: a plain array. */
public class TAtomicReferenceArray<E> {
    private final E[] array;

    @SuppressWarnings("unchecked")
    public TAtomicReferenceArray(int length) {
        array = (E[]) new Object[length];
    }

    public TAtomicReferenceArray(E[] array) {
        this.array = array.clone();
    }

    public final int length() {
        return array.length;
    }

    public final E get(int i) {
        return array[i];
    }

    public final void set(int i, E v) {
        array[i] = v;
    }

    public final void lazySet(int i, E v) {
        array[i] = v;
    }

    public final E getPlain(int i) {
        return array[i];
    }

    public final void setPlain(int i, E v) {
        array[i] = v;
    }

    public final E getOpaque(int i) {
        return array[i];
    }

    public final void setOpaque(int i, E v) {
        array[i] = v;
    }

    public final E getAcquire(int i) {
        return array[i];
    }

    public final void setRelease(int i, E v) {
        array[i] = v;
    }

    public final E getAndSet(int i, E v) {
        E old = array[i];
        array[i] = v;
        return old;
    }

    public final boolean compareAndSet(int i, E expect, E update) {
        if (array[i] != expect) {
            return false;
        }
        array[i] = update;
        return true;
    }

    public final boolean weakCompareAndSet(int i, E expect, E update) {
        return compareAndSet(i, expect, update);
    }

    public final boolean weakCompareAndSetPlain(int i, E expect, E update) {
        return compareAndSet(i, expect, update);
    }

    public final boolean weakCompareAndSetVolatile(int i, E expect, E update) {
        return compareAndSet(i, expect, update);
    }

    public final E getAndUpdate(int i, UnaryOperator<E> f) {
        E old = array[i];
        array[i] = f.apply(old);
        return old;
    }

    public final E updateAndGet(int i, UnaryOperator<E> f) {
        return array[i] = f.apply(array[i]);
    }

    public final E getAndAccumulate(int i, E x, BinaryOperator<E> f) {
        E old = array[i];
        array[i] = f.apply(old, x);
        return old;
    }

    public final E accumulateAndGet(int i, E x, BinaryOperator<E> f) {
        return array[i] = f.apply(array[i], x);
    }

    public final E compareAndExchange(int i, E expected, E newValue) {
        E witness = array[i];
        if (witness == expected) {
            array[i] = newValue;
        }
        return witness;
    }

    public final E compareAndExchangeAcquire(int i, E expected, E newValue) {
        return compareAndExchange(i, expected, newValue);
    }

    public final E compareAndExchangeRelease(int i, E expected, E newValue) {
        return compareAndExchange(i, expected, newValue);
    }

    @Override
    public String toString() {
        return java.util.Arrays.toString(array);
    }
}
