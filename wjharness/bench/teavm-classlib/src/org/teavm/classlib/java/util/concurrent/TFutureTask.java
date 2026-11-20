package org.teavm.classlib.java.util.concurrent;

import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

/* java.util.concurrent.FutureTask; get() on an unfinished task runs background work until it has run. */
public class TFutureTask<V> implements TRunnableFuture<V>, TScheduledFuture<V> {
    private Callable<V> callable;
    private final TCompletableFuture<V> outcome = new TCompletableFuture<>();

    public TFutureTask(Callable<V> callable) {
        if (callable == null) {
            throw new NullPointerException();
        }
        this.callable = callable;
    }

    public TFutureTask(Runnable runnable, V result) {
        this(() -> {
            runnable.run();
            return result;
        });
    }

    @Override
    public void run() {
        Callable<V> c = callable;
        if (c == null || outcome.isDone()) {
            return;
        }
        callable = null;
        try {
            set(c.call());
        } catch (Throwable t) {
            setException(t);
        }
        done();
    }

    protected void done() {
    }

    protected void set(V v) {
        outcome.complete(v);
    }

    protected void setException(Throwable t) {
        outcome.completeExceptionally(t);
    }

    @Override
    public boolean cancel(boolean mayInterruptIfRunning) {
        callable = null;
        return outcome.cancel(mayInterruptIfRunning);
    }

    @Override
    public boolean isCancelled() {
        return outcome.isCancelled();
    }

    @Override
    public boolean isDone() {
        return outcome.isDone();
    }

    @Override
    public V get() throws InterruptedException, ExecutionException {
        return outcome.get();
    }

    @Override
    public V get(long timeout, TimeUnit unit) throws InterruptedException, ExecutionException, TTimeoutException {
        return outcome.get();
    }

    @Override
    public long getDelay(TimeUnit unit) {
        return 0;
    }

    @Override
    public int compareTo(TDelayed o) {
        return 0;
    }
}
