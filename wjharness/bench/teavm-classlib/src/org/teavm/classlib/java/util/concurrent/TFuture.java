package org.teavm.classlib.java.util.concurrent;

import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

/* java.util.concurrent.Future (wjharness bench/teavm-classlib: single-threaded, see wjteavm.Background). */
public interface TFuture<V> {
    boolean cancel(boolean mayInterruptIfRunning);

    boolean isCancelled();

    boolean isDone();

    V get() throws InterruptedException, ExecutionException;

    V get(long timeout, TimeUnit unit) throws InterruptedException, ExecutionException, TTimeoutException;
}
