package org.teavm.classlib.java.util.concurrent;

import java.util.concurrent.Executor;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Function;

/* java.util.concurrent.CompletionStage (wjharness bench/teavm-classlib). */
public interface TCompletionStage<T> {
    <U> TCompletionStage<U> thenApply(Function<? super T, ? extends U> fn);

    <U> TCompletionStage<U> thenApplyAsync(Function<? super T, ? extends U> fn);

    <U> TCompletionStage<U> thenApplyAsync(Function<? super T, ? extends U> fn, Executor executor);

    TCompletionStage<Void> thenAccept(Consumer<? super T> action);

    TCompletionStage<Void> thenAcceptAsync(Consumer<? super T> action);

    TCompletionStage<Void> thenAcceptAsync(Consumer<? super T> action, Executor executor);

    TCompletionStage<Void> thenRun(Runnable action);

    TCompletionStage<Void> thenRunAsync(Runnable action);

    TCompletionStage<Void> thenRunAsync(Runnable action, Executor executor);

    <U, V> TCompletionStage<V> thenCombine(TCompletionStage<? extends U> other,
            BiFunction<? super T, ? super U, ? extends V> fn);

    <U, V> TCompletionStage<V> thenCombineAsync(TCompletionStage<? extends U> other,
            BiFunction<? super T, ? super U, ? extends V> fn);

    <U, V> TCompletionStage<V> thenCombineAsync(TCompletionStage<? extends U> other,
            BiFunction<? super T, ? super U, ? extends V> fn, Executor executor);

    <U> TCompletionStage<Void> thenAcceptBoth(TCompletionStage<? extends U> other,
            BiConsumer<? super T, ? super U> action);

    <U> TCompletionStage<Void> thenAcceptBothAsync(TCompletionStage<? extends U> other,
            BiConsumer<? super T, ? super U> action);

    <U> TCompletionStage<Void> thenAcceptBothAsync(TCompletionStage<? extends U> other,
            BiConsumer<? super T, ? super U> action, Executor executor);

    TCompletionStage<Void> runAfterBoth(TCompletionStage<?> other, Runnable action);

    TCompletionStage<Void> runAfterBothAsync(TCompletionStage<?> other, Runnable action);

    TCompletionStage<Void> runAfterBothAsync(TCompletionStage<?> other, Runnable action, Executor executor);

    <U> TCompletionStage<U> applyToEither(TCompletionStage<? extends T> other, Function<? super T, U> fn);

    <U> TCompletionStage<U> applyToEitherAsync(TCompletionStage<? extends T> other, Function<? super T, U> fn);

    <U> TCompletionStage<U> applyToEitherAsync(TCompletionStage<? extends T> other, Function<? super T, U> fn,
            Executor executor);

    TCompletionStage<Void> acceptEither(TCompletionStage<? extends T> other, Consumer<? super T> action);

    TCompletionStage<Void> acceptEitherAsync(TCompletionStage<? extends T> other, Consumer<? super T> action);

    TCompletionStage<Void> acceptEitherAsync(TCompletionStage<? extends T> other, Consumer<? super T> action,
            Executor executor);

    TCompletionStage<Void> runAfterEither(TCompletionStage<?> other, Runnable action);

    TCompletionStage<Void> runAfterEitherAsync(TCompletionStage<?> other, Runnable action);

    TCompletionStage<Void> runAfterEitherAsync(TCompletionStage<?> other, Runnable action, Executor executor);

    <U> TCompletionStage<U> thenCompose(Function<? super T, ? extends TCompletionStage<U>> fn);

    <U> TCompletionStage<U> thenComposeAsync(Function<? super T, ? extends TCompletionStage<U>> fn);

    <U> TCompletionStage<U> thenComposeAsync(Function<? super T, ? extends TCompletionStage<U>> fn,
            Executor executor);

    <U> TCompletionStage<U> handle(BiFunction<? super T, Throwable, ? extends U> fn);

    <U> TCompletionStage<U> handleAsync(BiFunction<? super T, Throwable, ? extends U> fn);

    <U> TCompletionStage<U> handleAsync(BiFunction<? super T, Throwable, ? extends U> fn, Executor executor);

    TCompletionStage<T> whenComplete(BiConsumer<? super T, ? super Throwable> action);

    TCompletionStage<T> whenCompleteAsync(BiConsumer<? super T, ? super Throwable> action);

    TCompletionStage<T> whenCompleteAsync(BiConsumer<? super T, ? super Throwable> action, Executor executor);

    TCompletionStage<T> exceptionally(Function<Throwable, ? extends T> fn);

    default TCompletionStage<T> exceptionallyAsync(Function<Throwable, ? extends T> fn) {
        return toCompletableFuture().exceptionallyAsync(fn);
    }

    default TCompletionStage<T> exceptionallyAsync(Function<Throwable, ? extends T> fn, Executor executor) {
        return toCompletableFuture().exceptionallyAsync(fn, executor);
    }

    default TCompletionStage<T> exceptionallyCompose(Function<Throwable, ? extends TCompletionStage<T>> fn) {
        return toCompletableFuture().exceptionallyCompose(fn);
    }

    default TCompletionStage<T> exceptionallyComposeAsync(Function<Throwable, ? extends TCompletionStage<T>> fn) {
        return toCompletableFuture().exceptionallyComposeAsync(fn);
    }

    default TCompletionStage<T> exceptionallyComposeAsync(Function<Throwable, ? extends TCompletionStage<T>> fn,
            Executor executor) {
        return toCompletableFuture().exceptionallyComposeAsync(fn, executor);
    }

    TCompletableFuture<T> toCompletableFuture();
}
