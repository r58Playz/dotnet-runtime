package org.teavm.classlib.java.util.concurrent;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import wjteavm.Background;

/* Every executor of the single-threaded classlib: execute() posts to wjteavm.Background unless overridden. */
public abstract class TAbstractExecutorService implements TExecutorService {
    private boolean shutdown;

    protected <T> TRunnableFuture<T> newTaskFor(Runnable runnable, T value) {
        return new TFutureTask<>(runnable, value);
    }

    protected <T> TRunnableFuture<T> newTaskFor(Callable<T> callable) {
        return new TFutureTask<>(callable);
    }

    @Override
    public void execute(Runnable command) {
        if (shutdown) {
            throw new TRejectedExecutionException("executor is shut down");
        }
        Background.post(command);
    }

    @Override
    public TFuture<?> submit(Runnable task) {
        TRunnableFuture<Void> f = newTaskFor(task, null);
        execute(f);
        return f;
    }

    @Override
    public <T> TFuture<T> submit(Runnable task, T result) {
        TRunnableFuture<T> f = newTaskFor(task, result);
        execute(f);
        return f;
    }

    @Override
    public <T> TFuture<T> submit(Callable<T> task) {
        TRunnableFuture<T> f = newTaskFor(task);
        execute(f);
        return f;
    }

    @Override
    public <T> List<TFuture<T>> invokeAll(Collection<? extends Callable<T>> tasks) throws InterruptedException {
        List<TFuture<T>> out = new ArrayList<>();
        for (Callable<T> t : tasks) {
            TRunnableFuture<T> f = newTaskFor(t);
            f.run();
            out.add(f);
        }
        return out;
    }

    @Override
    public <T> List<TFuture<T>> invokeAll(Collection<? extends Callable<T>> tasks, long timeout, TimeUnit unit)
            throws InterruptedException {
        return invokeAll(tasks);
    }

    @Override
    public <T> T invokeAny(Collection<? extends Callable<T>> tasks) throws InterruptedException, ExecutionException {
        ExecutionException last = null;
        for (Callable<T> t : tasks) {
            try {
                return t.call();
            } catch (Exception e) {
                last = new ExecutionException(e);
            }
        }
        if (last != null) {
            throw last;
        }
        throw new IllegalArgumentException();
    }

    @Override
    public <T> T invokeAny(Collection<? extends Callable<T>> tasks, long timeout, TimeUnit unit)
            throws InterruptedException, ExecutionException {
        return invokeAny(tasks);
    }

    @Override
    public void shutdown() {
        shutdown = true;
    }

    @Override
    public List<Runnable> shutdownNow() {
        shutdown = true;
        return Collections.emptyList();
    }

    @Override
    public boolean isShutdown() {
        return shutdown;
    }

    @Override
    public boolean isTerminated() {
        return shutdown;
    }

    @Override
    public boolean awaitTermination(long timeout, TimeUnit unit) {
        Background.drain();
        return true;
    }
}
