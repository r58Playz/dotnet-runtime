package org.teavm.classlib.java.util.concurrent;

/* Exists for subclasses (Minecraft's Util.makeExecutor); TForkJoinPool never asks its factory for one. */
public class TForkJoinWorkerThread extends Thread {
    private final TForkJoinPool pool;

    protected TForkJoinWorkerThread(TForkJoinPool pool) {
        this.pool = pool;
    }

    protected TForkJoinWorkerThread(ThreadGroup group, TForkJoinPool pool, boolean preserveThreadLocals) {
        this.pool = pool;
    }

    public TForkJoinPool getPool() {
        return pool;
    }

    public int getPoolIndex() {
        return 0;
    }

    protected void onStart() {
    }

    protected void onTermination(Throwable exception) {
    }
}
