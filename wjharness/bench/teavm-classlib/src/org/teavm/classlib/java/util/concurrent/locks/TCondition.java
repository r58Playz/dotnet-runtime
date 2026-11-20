package org.teavm.classlib.java.util.concurrent.locks;

import java.util.Date;
import java.util.concurrent.TimeUnit;
import wjteavm.Background;

/* A wait on a condition runs one background task (the only way another "thread" can signal) and returns. */
public interface TCondition {
    default void await() throws InterruptedException {
        Background.runOne();
    }

    default void awaitUninterruptibly() {
        Background.runOne();
    }

    default long awaitNanos(long nanosTimeout) throws InterruptedException {
        Background.runOne();
        return 0;
    }

    default boolean await(long time, TimeUnit unit) throws InterruptedException {
        Background.runOne();
        return false;
    }

    default boolean awaitUntil(Date deadline) throws InterruptedException {
        Background.runOne();
        return false;
    }

    default void signal() {
    }

    default void signalAll() {
    }
}
