package org.teavm.classlib.java.util.concurrent;

import java.util.concurrent.TimeUnit;
import wjteavm.Background;

/* A permit count; acquire() without a permit runs background work until one is released, or fails. */
public class TSemaphore {
    private int permits;

    public TSemaphore(int permits) {
        this.permits = permits;
    }

    public TSemaphore(int permits, boolean fair) {
        this.permits = permits;
    }

    public void acquire() throws InterruptedException {
        acquire(1);
    }

    public void acquire(int n) throws InterruptedException {
        Background.awaitUntil(() -> permits >= n, "Semaphore.acquire");
        permits -= n;
    }

    public void acquireUninterruptibly() {
        acquireUninterruptibly(1);
    }

    public void acquireUninterruptibly(int n) {
        Background.awaitUntil(() -> permits >= n, "Semaphore.acquire");
        permits -= n;
    }

    public boolean tryAcquire() {
        return tryAcquire(1);
    }

    public boolean tryAcquire(int n) {
        if (permits < n) {
            return false;
        }
        permits -= n;
        return true;
    }

    public boolean tryAcquire(long timeout, TimeUnit unit) {
        return tryAcquire(1);
    }

    public boolean tryAcquire(int n, long timeout, TimeUnit unit) {
        return tryAcquire(n);
    }

    public void release() {
        ++permits;
    }

    public void release(int n) {
        permits += n;
    }

    public int availablePermits() {
        return permits;
    }

    public int drainPermits() {
        int p = permits;
        permits = 0;
        return p;
    }

    public boolean isFair() {
        return false;
    }

    public final boolean hasQueuedThreads() {
        return false;
    }

    public final int getQueueLength() {
        return 0;
    }
}
