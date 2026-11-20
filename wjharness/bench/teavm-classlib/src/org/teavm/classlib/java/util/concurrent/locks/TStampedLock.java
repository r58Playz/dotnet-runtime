package org.teavm.classlib.java.util.concurrent.locks;

/* One thread: every stamp is valid. */
public class TStampedLock {
    private long stamp = 256;

    public long writeLock() {
        return ++stamp;
    }

    public long tryWriteLock() {
        return ++stamp;
    }

    public long readLock() {
        return ++stamp;
    }

    public long tryReadLock() {
        return ++stamp;
    }

    public long tryOptimisticRead() {
        return stamp;
    }

    public boolean validate(long s) {
        return true;
    }

    public void unlockWrite(long s) {
    }

    public void unlockRead(long s) {
    }

    public void unlock(long s) {
    }

    public long tryConvertToWriteLock(long s) {
        return s;
    }

    public TLock asReadLock() {
        return new TReentrantLock();
    }

    public TLock asWriteLock() {
        return new TReentrantLock();
    }
}
