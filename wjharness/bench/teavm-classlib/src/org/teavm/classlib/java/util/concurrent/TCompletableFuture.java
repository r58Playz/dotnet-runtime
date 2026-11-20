package org.teavm.classlib.java.util.concurrent;

import java.util.ArrayList;
import java.util.concurrent.CancellationException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;
import wjteavm.Background;

/*
 * java.util.concurrent.CompletableFuture for one thread (wjharness bench/teavm-classlib; see wjteavm.Background).
 *
 * Dependents run when the source completes, on the completing call (the non-Async forms) or posted to their executor
 * (the Async forms; the default executor is the background queue). Exceptional results follow the JDK: a stage that
 * fails because its source failed, or because its function threw, holds a CompletionException; completeExceptionally
 * stores its argument as given; join() throws CompletionException (CancellationException as is), get() throws
 * ExecutionException with the cause. join/get on an incomplete future run background tasks until it completes.
 */
public class TCompletableFuture<T> implements TFuture<T>, TCompletionStage<T> {
    private static final Object NIL = new Object();
    private static final Executor ASYNC = Background::post;

    private static final class Alt {
        final Throwable ex;

        Alt(Throwable ex) {
            this.ex = ex;
        }
    }

    private Object result;
    private ArrayList<Runnable> waiters;

    public TCompletableFuture() {
    }

    // ---- completion

    private boolean completeRaw(Object r) {
        if (result != null) {
            return false;
        }
        result = r;
        ArrayList<Runnable> w = waiters;
        waiters = null;
        if (w != null) {
            for (Runnable r1 : w) {
                r1.run();
            }
        }
        return true;
    }

    final boolean completeValue(Object v) {
        return completeRaw(v == null ? NIL : v);
    }

    final boolean completeThrowable(Throwable x) {
        return completeRaw(new Alt(x instanceof TCompletionException ? x : new TCompletionException(x)));
    }

    public boolean complete(T value) {
        return completeValue(value);
    }

    public boolean completeExceptionally(Throwable ex) {
        if (ex == null) {
            throw new NullPointerException();
        }
        return completeRaw(new Alt(ex));
    }

    public boolean cancel(boolean mayInterruptIfRunning) {
        boolean cancelled = completeRaw(new Alt(new CancellationException()));
        return cancelled || isCancelled();
    }

    public void obtrudeValue(T value) {
        result = value == null ? NIL : value;
    }

    public void obtrudeException(Throwable ex) {
        result = new Alt(ex);
    }

    public TCompletableFuture<T> completeAsync(Supplier<? extends T> supplier) {
        return completeAsync(supplier, ASYNC);
    }

    public TCompletableFuture<T> completeAsync(Supplier<? extends T> supplier, Executor executor) {
        executor.execute(() -> {
            try {
                completeValue(supplier.get());
            } catch (Throwable x) {
                completeThrowable(x);
            }
        });
        return this;
    }

    public TCompletableFuture<T> orTimeout(long timeout, TimeUnit unit) {
        return this;
    }

    public TCompletableFuture<T> completeOnTimeout(T value, long timeout, TimeUnit unit) {
        return this;
    }

    // ---- state

    public boolean isDone() {
        return result != null;
    }

    public boolean isCancelled() {
        return result instanceof Alt && ((Alt) result).ex instanceof CancellationException;
    }

    public boolean isCompletedExceptionally() {
        return result instanceof Alt;
    }

    public int getNumberOfDependents() {
        return waiters == null ? 0 : waiters.size();
    }

    @SuppressWarnings("unchecked")
    private T value(Object r) {
        return r == NIL ? null : (T) r;
    }

    private void await() {
        if (result == null) {
            Background.awaitUntil(() -> result != null, "CompletableFuture");
        }
    }

    public T join() {
        await();
        Object r = result;
        if (r instanceof Alt) {
            Throwable x = ((Alt) r).ex;
            if (x instanceof CancellationException) {
                throw (CancellationException) x;
            }
            throw x instanceof TCompletionException ? (TCompletionException) x : new TCompletionException(x);
        }
        return value(r);
    }

    public T get() throws InterruptedException, ExecutionException {
        await();
        return reportGet();
    }

    public T get(long timeout, TimeUnit unit) throws InterruptedException, ExecutionException, TTimeoutException {
        await();
        return reportGet();
    }

    private T reportGet() throws ExecutionException {
        Object r = result;
        if (r instanceof Alt) {
            Throwable x = ((Alt) r).ex;
            if (x instanceof CancellationException) {
                throw (CancellationException) x;
            }
            Throwable cause = x instanceof TCompletionException && x.getCause() != null ? x.getCause() : x;
            throw new ExecutionException(cause);
        }
        return value(r);
    }

    public T getNow(T valueIfAbsent) {
        return result == null ? valueIfAbsent : join();
    }

    public T resultNow() {
        Object r = result;
        if (r == null || r instanceof Alt) {
            throw new IllegalStateException();
        }
        return value(r);
    }

    public Throwable exceptionNow() {
        Object r = result;
        if (!(r instanceof Alt)) {
            throw new IllegalStateException();
        }
        Throwable x = ((Alt) r).ex;
        return x instanceof TCompletionException && x.getCause() != null ? x.getCause() : x;
    }

    // ---- plumbing

    private void onDone(Executor e, Runnable action) {
        Runnable r = e == null ? action : () -> e.execute(action);
        if (result != null) {
            r.run();
        } else {
            if (waiters == null) {
                waiters = new ArrayList<>(2);
            }
            waiters.add(r);
        }
    }

    private Throwable failure() {
        Object r = result;
        return r instanceof Alt ? ((Alt) r).ex : null;
    }

    /* Copy this future's (completed) outcome into d. */
    private void relayTo(TCompletableFuture<?> d) {
        Object r = result;
        if (r instanceof Alt) {
            d.completeThrowable(((Alt) r).ex);
        } else {
            d.completeRaw(r);
        }
    }

    public <U> TCompletableFuture<U> newIncompleteFuture() {
        return new TCompletableFuture<>();
    }

    public Executor defaultExecutor() {
        return ASYNC;
    }

    public TCompletableFuture<T> toCompletableFuture() {
        return this;
    }

    public TCompletableFuture<T> copy() {
        TCompletableFuture<T> d = newIncompleteFuture();
        onDone(null, () -> relayTo(d));
        return d;
    }

    public TCompletionStage<T> minimalCompletionStage() {
        return copy();
    }

    // ---- uni stages

    private <U> TCompletableFuture<U> uniApply(Executor e, Function<? super T, ? extends U> fn) {
        TCompletableFuture<U> d = newIncompleteFuture();
        onDone(e, () -> {
            Throwable x = failure();
            if (x != null) {
                d.completeThrowable(x);
                return;
            }
            try {
                d.completeValue(fn.apply(value(result)));
            } catch (Throwable t) {
                d.completeThrowable(t);
            }
        });
        return d;
    }

    public <U> TCompletableFuture<U> thenApply(Function<? super T, ? extends U> fn) {
        return uniApply(null, fn);
    }

    public <U> TCompletableFuture<U> thenApplyAsync(Function<? super T, ? extends U> fn) {
        return uniApply(ASYNC, fn);
    }

    public <U> TCompletableFuture<U> thenApplyAsync(Function<? super T, ? extends U> fn, Executor executor) {
        return uniApply(executor, fn);
    }

    public TCompletableFuture<Void> thenAccept(Consumer<? super T> action) {
        return uniApply(null, v -> {
            action.accept(v);
            return null;
        });
    }

    public TCompletableFuture<Void> thenAcceptAsync(Consumer<? super T> action) {
        return thenAcceptAsync(action, ASYNC);
    }

    public TCompletableFuture<Void> thenAcceptAsync(Consumer<? super T> action, Executor executor) {
        return uniApply(executor, v -> {
            action.accept(v);
            return null;
        });
    }

    public TCompletableFuture<Void> thenRun(Runnable action) {
        return uniApply(null, v -> {
            action.run();
            return null;
        });
    }

    public TCompletableFuture<Void> thenRunAsync(Runnable action) {
        return thenRunAsync(action, ASYNC);
    }

    public TCompletableFuture<Void> thenRunAsync(Runnable action, Executor executor) {
        return uniApply(executor, v -> {
            action.run();
            return null;
        });
    }

    private <U> TCompletableFuture<U> uniCompose(Executor e, Function<? super T, ? extends TCompletionStage<U>> fn) {
        TCompletableFuture<U> d = newIncompleteFuture();
        onDone(e, () -> {
            Throwable x = failure();
            if (x != null) {
                d.completeThrowable(x);
                return;
            }
            try {
                TCompletableFuture<U> c = fn.apply(value(result)).toCompletableFuture();
                c.onDone(null, () -> c.relayTo(d));
            } catch (Throwable t) {
                d.completeThrowable(t);
            }
        });
        return d;
    }

    public <U> TCompletableFuture<U> thenCompose(Function<? super T, ? extends TCompletionStage<U>> fn) {
        return uniCompose(null, fn);
    }

    public <U> TCompletableFuture<U> thenComposeAsync(Function<? super T, ? extends TCompletionStage<U>> fn) {
        return uniCompose(ASYNC, fn);
    }

    public <U> TCompletableFuture<U> thenComposeAsync(Function<? super T, ? extends TCompletionStage<U>> fn,
            Executor executor) {
        return uniCompose(executor, fn);
    }

    private <U> TCompletableFuture<U> uniHandle(Executor e, BiFunction<? super T, Throwable, ? extends U> fn) {
        TCompletableFuture<U> d = newIncompleteFuture();
        onDone(e, () -> {
            Throwable x = failure();
            try {
                d.completeValue(fn.apply(x == null ? value(result) : null, x));
            } catch (Throwable t) {
                d.completeThrowable(t);
            }
        });
        return d;
    }

    public <U> TCompletableFuture<U> handle(BiFunction<? super T, Throwable, ? extends U> fn) {
        return uniHandle(null, fn);
    }

    public <U> TCompletableFuture<U> handleAsync(BiFunction<? super T, Throwable, ? extends U> fn) {
        return uniHandle(ASYNC, fn);
    }

    public <U> TCompletableFuture<U> handleAsync(BiFunction<? super T, Throwable, ? extends U> fn, Executor executor) {
        return uniHandle(executor, fn);
    }

    private TCompletableFuture<T> uniWhenComplete(Executor e, BiConsumer<? super T, ? super Throwable> action) {
        TCompletableFuture<T> d = newIncompleteFuture();
        onDone(e, () -> {
            Throwable x = failure();
            try {
                action.accept(x == null ? value(result) : null, x);
            } catch (Throwable t) {
                if (x == null) {
                    d.completeThrowable(t);
                    return;
                }
                if (x != t) {
                    x.addSuppressed(t);
                }
            }
            relayTo(d);
        });
        return d;
    }

    public TCompletableFuture<T> whenComplete(BiConsumer<? super T, ? super Throwable> action) {
        return uniWhenComplete(null, action);
    }

    public TCompletableFuture<T> whenCompleteAsync(BiConsumer<? super T, ? super Throwable> action) {
        return uniWhenComplete(ASYNC, action);
    }

    public TCompletableFuture<T> whenCompleteAsync(BiConsumer<? super T, ? super Throwable> action, Executor executor) {
        return uniWhenComplete(executor, action);
    }

    private TCompletableFuture<T> uniExceptionally(Executor e, Function<Throwable, ? extends T> fn) {
        TCompletableFuture<T> d = newIncompleteFuture();
        onDone(e, () -> {
            Throwable x = failure();
            if (x == null) {
                relayTo(d);
                return;
            }
            try {
                d.completeValue(fn.apply(x));
            } catch (Throwable t) {
                d.completeThrowable(t);
            }
        });
        return d;
    }

    public TCompletableFuture<T> exceptionally(Function<Throwable, ? extends T> fn) {
        return uniExceptionally(null, fn);
    }

    public TCompletableFuture<T> exceptionallyAsync(Function<Throwable, ? extends T> fn) {
        return uniExceptionally(ASYNC, fn);
    }

    public TCompletableFuture<T> exceptionallyAsync(Function<Throwable, ? extends T> fn, Executor executor) {
        return uniExceptionally(executor, fn);
    }

    private TCompletableFuture<T> uniExceptionallyCompose(Executor e,
            Function<Throwable, ? extends TCompletionStage<T>> fn) {
        TCompletableFuture<T> d = newIncompleteFuture();
        onDone(e, () -> {
            Throwable x = failure();
            if (x == null) {
                relayTo(d);
                return;
            }
            try {
                TCompletableFuture<T> c = fn.apply(x).toCompletableFuture();
                c.onDone(null, () -> c.relayTo(d));
            } catch (Throwable t) {
                d.completeThrowable(t);
            }
        });
        return d;
    }

    public TCompletableFuture<T> exceptionallyCompose(Function<Throwable, ? extends TCompletionStage<T>> fn) {
        return uniExceptionallyCompose(null, fn);
    }

    public TCompletableFuture<T> exceptionallyComposeAsync(Function<Throwable, ? extends TCompletionStage<T>> fn) {
        return uniExceptionallyCompose(ASYNC, fn);
    }

    public TCompletableFuture<T> exceptionallyComposeAsync(Function<Throwable, ? extends TCompletionStage<T>> fn,
            Executor executor) {
        return uniExceptionallyCompose(executor, fn);
    }

    // ---- bi stages: both

    private <U, V> TCompletableFuture<V> biApply(Executor e, TCompletionStage<? extends U> other,
            BiFunction<? super T, ? super U, ? extends V> fn) {
        TCompletableFuture<? extends U> o = other.toCompletableFuture();
        TCompletableFuture<V> d = newIncompleteFuture();
        onDone(null, () -> o.onDone(e, () -> {
            Throwable x = failure();
            if (x == null) {
                x = o.failure();
            }
            if (x != null) {
                d.completeThrowable(x);
                return;
            }
            try {
                d.completeValue(fn.apply(value(result), o.value(o.result)));
            } catch (Throwable t) {
                d.completeThrowable(t);
            }
        }));
        return d;
    }

    public <U, V> TCompletableFuture<V> thenCombine(TCompletionStage<? extends U> other,
            BiFunction<? super T, ? super U, ? extends V> fn) {
        return biApply(null, other, fn);
    }

    public <U, V> TCompletableFuture<V> thenCombineAsync(TCompletionStage<? extends U> other,
            BiFunction<? super T, ? super U, ? extends V> fn) {
        return biApply(ASYNC, other, fn);
    }

    public <U, V> TCompletableFuture<V> thenCombineAsync(TCompletionStage<? extends U> other,
            BiFunction<? super T, ? super U, ? extends V> fn, Executor executor) {
        return biApply(executor, other, fn);
    }

    public <U> TCompletableFuture<Void> thenAcceptBoth(TCompletionStage<? extends U> other,
            BiConsumer<? super T, ? super U> action) {
        return biApply(null, other, (a, b) -> {
            action.accept(a, b);
            return null;
        });
    }

    public <U> TCompletableFuture<Void> thenAcceptBothAsync(TCompletionStage<? extends U> other,
            BiConsumer<? super T, ? super U> action) {
        return thenAcceptBothAsync(other, action, ASYNC);
    }

    public <U> TCompletableFuture<Void> thenAcceptBothAsync(TCompletionStage<? extends U> other,
            BiConsumer<? super T, ? super U> action, Executor executor) {
        return biApply(executor, other, (a, b) -> {
            action.accept(a, b);
            return null;
        });
    }

    public TCompletableFuture<Void> runAfterBoth(TCompletionStage<?> other, Runnable action) {
        return biApply(null, other, (a, b) -> {
            action.run();
            return null;
        });
    }

    public TCompletableFuture<Void> runAfterBothAsync(TCompletionStage<?> other, Runnable action) {
        return runAfterBothAsync(other, action, ASYNC);
    }

    public TCompletableFuture<Void> runAfterBothAsync(TCompletionStage<?> other, Runnable action, Executor executor) {
        return biApply(executor, other, (a, b) -> {
            action.run();
            return null;
        });
    }

    // ---- bi stages: either

    private <U> TCompletableFuture<U> orApply(Executor e, TCompletionStage<? extends T> other,
            Function<? super T, U> fn) {
        @SuppressWarnings("unchecked")
        TCompletableFuture<T> o = (TCompletableFuture<T>) other.toCompletableFuture();
        TCompletableFuture<U> d = newIncompleteFuture();
        Runnable fire = () -> {
            if (d.result != null) {
                return;
            }
            TCompletableFuture<T> src = result != null ? this : o;
            Runnable body = () -> {
                Throwable x = src.failure();
                if (x != null) {
                    d.completeThrowable(x);
                    return;
                }
                try {
                    d.completeValue(fn.apply(src.value(src.result)));
                } catch (Throwable t) {
                    d.completeThrowable(t);
                }
            };
            if (e == null) {
                body.run();
            } else {
                e.execute(body);
            }
        };
        onDone(null, fire);
        o.onDone(null, fire);
        return d;
    }

    public <U> TCompletableFuture<U> applyToEither(TCompletionStage<? extends T> other, Function<? super T, U> fn) {
        return orApply(null, other, fn);
    }

    public <U> TCompletableFuture<U> applyToEitherAsync(TCompletionStage<? extends T> other,
            Function<? super T, U> fn) {
        return orApply(ASYNC, other, fn);
    }

    public <U> TCompletableFuture<U> applyToEitherAsync(TCompletionStage<? extends T> other,
            Function<? super T, U> fn, Executor executor) {
        return orApply(executor, other, fn);
    }

    public TCompletableFuture<Void> acceptEither(TCompletionStage<? extends T> other, Consumer<? super T> action) {
        return orApply(null, other, v -> {
            action.accept(v);
            return null;
        });
    }

    public TCompletableFuture<Void> acceptEitherAsync(TCompletionStage<? extends T> other,
            Consumer<? super T> action) {
        return acceptEitherAsync(other, action, ASYNC);
    }

    public TCompletableFuture<Void> acceptEitherAsync(TCompletionStage<? extends T> other,
            Consumer<? super T> action, Executor executor) {
        return orApply(executor, other, v -> {
            action.accept(v);
            return null;
        });
    }

    @SuppressWarnings("unchecked")
    public TCompletableFuture<Void> runAfterEither(TCompletionStage<?> other, Runnable action) {
        return orApply(null, (TCompletionStage<T>) other, v -> {
            action.run();
            return null;
        });
    }

    public TCompletableFuture<Void> runAfterEitherAsync(TCompletionStage<?> other, Runnable action) {
        return runAfterEitherAsync(other, action, ASYNC);
    }

    @SuppressWarnings("unchecked")
    public TCompletableFuture<Void> runAfterEitherAsync(TCompletionStage<?> other, Runnable action,
            Executor executor) {
        return orApply(executor, (TCompletionStage<T>) other, v -> {
            action.run();
            return null;
        });
    }

    // ---- statics

    public static <U> TCompletableFuture<U> supplyAsync(Supplier<U> supplier) {
        return supplyAsync(supplier, ASYNC);
    }

    public static <U> TCompletableFuture<U> supplyAsync(Supplier<U> supplier, Executor executor) {
        TCompletableFuture<U> d = new TCompletableFuture<>();
        executor.execute(() -> {
            try {
                d.completeValue(supplier.get());
            } catch (Throwable t) {
                d.completeThrowable(t);
            }
        });
        return d;
    }

    public static TCompletableFuture<Void> runAsync(Runnable runnable) {
        return runAsync(runnable, ASYNC);
    }

    public static TCompletableFuture<Void> runAsync(Runnable runnable, Executor executor) {
        TCompletableFuture<Void> d = new TCompletableFuture<>();
        executor.execute(() -> {
            try {
                runnable.run();
                d.completeValue(null);
            } catch (Throwable t) {
                d.completeThrowable(t);
            }
        });
        return d;
    }

    public static <U> TCompletableFuture<U> completedFuture(U value) {
        TCompletableFuture<U> d = new TCompletableFuture<>();
        d.completeValue(value);
        return d;
    }

    public static <U> TCompletionStage<U> completedStage(U value) {
        return completedFuture(value);
    }

    public static <U> TCompletableFuture<U> failedFuture(Throwable ex) {
        TCompletableFuture<U> d = new TCompletableFuture<>();
        d.completeExceptionally(ex);
        return d;
    }

    public static <U> TCompletionStage<U> failedStage(Throwable ex) {
        return failedFuture(ex);
    }

    public static TCompletableFuture<Void> allOf(TCompletableFuture<?>... cfs) {
        TCompletableFuture<Void> d = new TCompletableFuture<>();
        if (cfs.length == 0) {
            d.completeValue(null);
            return d;
        }
        int[] pending = { cfs.length };
        for (TCompletableFuture<?> cf : cfs) {
            cf.onDone(null, () -> {
                if (--pending[0] == 0) {
                    Throwable x = null;
                    for (TCompletableFuture<?> c : cfs) {
                        x = c.failure();
                        if (x != null) {
                            break;
                        }
                    }
                    if (x != null) {
                        d.completeThrowable(x);
                    } else {
                        d.completeValue(null);
                    }
                }
            });
        }
        return d;
    }

    public static TCompletableFuture<Object> anyOf(TCompletableFuture<?>... cfs) {
        TCompletableFuture<Object> d = new TCompletableFuture<>();
        for (TCompletableFuture<?> cf : cfs) {
            cf.onDone(null, () -> cf.relayTo(d));
        }
        return d;
    }

    public static Executor delayedExecutor(long delay, TimeUnit unit) {
        return ASYNC;
    }

    public static Executor delayedExecutor(long delay, TimeUnit unit, Executor executor) {
        return executor;
    }

    @Override
    public String toString() {
        Object r = result;
        return super.toString() + (r == null ? "[Incomplete]" : r instanceof Alt ? "[Completed exceptionally: "
                + ((Alt) r).ex + "]" : "[Completed normally]");
    }
}
