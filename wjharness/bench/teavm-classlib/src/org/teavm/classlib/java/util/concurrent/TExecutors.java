package org.teavm.classlib.java.util.concurrent;

import java.util.concurrent.Callable;
import java.util.concurrent.TimeUnit;
import wjteavm.Background;

/* Every pool is the background queue; a ThreadFactory is accepted and never called (no thread may start). */
public final class TExecutors {
    private TExecutors() {
    }

    static final class Pool extends TAbstractExecutorService implements TScheduledExecutorService {
        @Override
        public TScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            TFutureTask<Void> f = new TFutureTask<>(command, null);
            execute(f);
            return f;
        }

        @Override
        public <V> TScheduledFuture<V> schedule(Callable<V> callable, long delay, TimeUnit unit) {
            TFutureTask<V> f = new TFutureTask<>(callable);
            execute(f);
            return f;
        }

        @Override
        public TScheduledFuture<?> scheduleAtFixedRate(Runnable command, long initialDelay, long period,
                TimeUnit unit) {
            return schedule(command, initialDelay, unit);
        }

        @Override
        public TScheduledFuture<?> scheduleWithFixedDelay(Runnable command, long initialDelay, long delay,
                TimeUnit unit) {
            return schedule(command, initialDelay, unit);
        }
    }

    public static TExecutorService newCachedThreadPool() {
        return new Pool();
    }

    public static TExecutorService newCachedThreadPool(TThreadFactory threadFactory) {
        return new Pool();
    }

    public static TExecutorService newFixedThreadPool(int n) {
        return new Pool();
    }

    public static TExecutorService newFixedThreadPool(int n, TThreadFactory threadFactory) {
        return new Pool();
    }

    public static TExecutorService newSingleThreadExecutor() {
        return new Pool();
    }

    public static TExecutorService newSingleThreadExecutor(TThreadFactory threadFactory) {
        return new Pool();
    }

    public static TExecutorService newWorkStealingPool() {
        return new Pool();
    }

    public static TExecutorService newWorkStealingPool(int parallelism) {
        return new Pool();
    }

    public static TExecutorService newThreadPerTaskExecutor(TThreadFactory threadFactory) {
        return new Pool();
    }

    public static TExecutorService newVirtualThreadPerTaskExecutor() {
        return new Pool();
    }

    public static TScheduledExecutorService newSingleThreadScheduledExecutor() {
        return new Pool();
    }

    public static TScheduledExecutorService newSingleThreadScheduledExecutor(TThreadFactory threadFactory) {
        return new Pool();
    }

    public static TScheduledExecutorService newScheduledThreadPool(int corePoolSize) {
        return new Pool();
    }

    public static TScheduledExecutorService newScheduledThreadPool(int corePoolSize, TThreadFactory threadFactory) {
        return new Pool();
    }

    public static TExecutorService unconfigurableExecutorService(TExecutorService executor) {
        return executor;
    }

    public static TThreadFactory defaultThreadFactory() {
        return Thread::new;
    }

    public static <T> Callable<T> callable(Runnable task, T result) {
        return () -> {
            task.run();
            return result;
        };
    }

    public static Callable<Object> callable(Runnable task) {
        return callable(task, null);
    }

    static boolean drainOnce() {
        return Background.runOne();
    }
}
