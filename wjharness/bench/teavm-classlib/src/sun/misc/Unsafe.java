package sun.misc;

import java.lang.reflect.Field;

/*
 * No Unsafe on TeaVM: getUnsafe() refuses and theUnsafe is null, so a library that probes for it (JOML's MemUtil, the
 * only user on mctick's path) takes its portable path. The methods exist only so references link; they throw.
 */
public final class Unsafe {
    private static final Unsafe theUnsafe = null;

    private Unsafe() {
    }

    public static Unsafe getUnsafe() {
        throw new SecurityException("Unsafe is not available on TeaVM");
    }

    private static UnsupportedOperationException no() {
        return new UnsupportedOperationException("sun.misc.Unsafe on TeaVM");
    }

    public int arrayBaseOffset(Class<?> c) {
        throw no();
    }

    public int arrayIndexScale(Class<?> c) {
        throw no();
    }

    public long objectFieldOffset(Field f) {
        throw no();
    }

    public double getDouble(long a) {
        throw no();
    }

    public double getDouble(Object o, long off) {
        throw no();
    }

    public float getFloat(long a) {
        throw no();
    }

    public float getFloat(Object o, long off) {
        throw no();
    }

    public int getInt(Object o, long off) {
        throw no();
    }

    public long getLong(long a) {
        throw no();
    }

    public long getLong(Object o, long off) {
        throw no();
    }

    public void putDouble(Object o, long off, double v) {
        throw no();
    }

    public void putFloat(Object o, long off, float v) {
        throw no();
    }

    public void putInt(Object o, long off, int v) {
        throw no();
    }

    public void putLong(Object o, long off, long v) {
        throw no();
    }

    public boolean compareAndSwapObject(Object o, long off, Object expected, Object x) {
        throw no();
    }

    public boolean compareAndSwapInt(Object o, long off, int expected, int x) {
        throw no();
    }

    public boolean compareAndSwapLong(Object o, long off, long expected, long x) {
        throw no();
    }

    public Object getAndSetObject(Object o, long off, Object x) {
        throw no();
    }

    public Object getObject(Object o, long off) {
        throw no();
    }

    public Object getObjectVolatile(Object o, long off) {
        throw no();
    }

    public void putObject(Object o, long off, Object x) {
        throw no();
    }

    public void putObjectVolatile(Object o, long off, Object x) {
        throw no();
    }

    public void putOrderedObject(Object o, long off, Object x) {
        throw no();
    }

    public long staticFieldOffset(Field f) {
        throw no();
    }

    public Object staticFieldBase(Field f) {
        throw no();
    }
}
