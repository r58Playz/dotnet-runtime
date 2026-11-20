package org.teavm.classlib.java.util.concurrent;

import wjteavm.Background;

/* java.util.concurrent.ForkJoinPool for one thread: tasks go to the background queue (wjteavm.Background). */
public class TForkJoinPool extends TAbstractExecutorService {
    private static final TForkJoinPool COMMON = new TForkJoinPool();

    public interface ForkJoinWorkerThreadFactory {
        TForkJoinWorkerThread newThread(TForkJoinPool pool);
    }

    public interface ManagedBlocker {
        boolean block() throws InterruptedException;

        boolean isReleasable();
    }

    public static final ForkJoinWorkerThreadFactory defaultForkJoinWorkerThreadFactory = TForkJoinWorkerThread::new;

    private final int parallelism;

    public TForkJoinPool() {
        this(1);
    }

    public TForkJoinPool(int parallelism) {
        this.parallelism = parallelism;
    }

    public TForkJoinPool(int parallelism, ForkJoinWorkerThreadFactory factory, Thread.UncaughtExceptionHandler handler,
            boolean asyncMode) {
        this.parallelism = parallelism;
    }

    public static TForkJoinPool commonPool() {
        return COMMON;
    }

    public static int getCommonPoolParallelism() {
        return 1;
    }

    public int getParallelism() {
        return parallelism;
    }

    public int getPoolSize() {
        return 0;
    }

    public int getActiveCount() {
        return 0;
    }

    public int getQueuedSubmissionCount() {
        return Background.size();
    }

    public boolean isQuiescent() {
        return Background.isEmpty();
    }

    public static void managedBlock(ManagedBlocker blocker) throws InterruptedException {
        while (!blocker.isReleasable()) {
            if (blocker.block()) {
                return;
            }
            if (!Background.runOne()) {
                if (!blocker.isReleasable()) {
                    throw new IllegalStateException("[wjteavm] ForkJoinPool.managedBlock can never finish");
                }
            }
        }
    }
}
