package org.teavm.classlib.java.util.concurrent.locks;

import java.util.concurrent.TimeUnit;

/* One thread: a hold count. Unlocking a lock not held throws, as on the JDK. */
public class TReentrantLock implements TLock {
    private int holds;

    public TReentrantLock() {
    }

    public TReentrantLock(boolean fair) {
    }

    @Override
    public void lock() {
        ++holds;
    }

    @Override
    public void lockInterruptibly() {
        ++holds;
    }

    @Override
    public boolean tryLock() {
        ++holds;
        return true;
    }

    @Override
    public boolean tryLock(long timeout, TimeUnit unit) {
        ++holds;
        return true;
    }

    @Override
    public void unlock() {
        if (holds == 0) {
            throw new IllegalMonitorStateException();
        }
        --holds;
    }

    @Override
    public TCondition newCondition() {
        return new TCondition() {
        };
    }

    public int getHoldCount() {
        return holds;
    }

    public boolean isHeldByCurrentThread() {
        return holds > 0;
    }

    public boolean isLocked() {
        return holds > 0;
    }

    public final boolean isFair() {
        return false;
    }

    public final boolean hasQueuedThreads() {
        return false;
    }

    public final boolean hasQueuedThread(Thread thread) {
        return false;
    }

    public final int getQueueLength() {
        return 0;
    }

    public boolean hasWaiters(TCondition condition) {
        return false;
    }

    public int getWaitQueueLength(TCondition condition) {
        return 0;
    }
}
