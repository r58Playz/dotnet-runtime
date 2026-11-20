package org.teavm.classlib.java.util.concurrent.locks;

import wjteavm.Background;

/* park runs one background task and returns (a park may return spuriously, so callers re-check). */
public final class TLockSupport {
    private TLockSupport() {
    }

    public static void park() {
        Background.runOne();
    }

    public static void park(Object blocker) {
        Background.runOne();
    }

    public static void parkNanos(long nanos) {
        Background.runOne();
    }

    public static void parkNanos(Object blocker, long nanos) {
        Background.runOne();
    }

    public static void parkUntil(long deadline) {
        Background.runOne();
    }

    public static void parkUntil(Object blocker, long deadline) {
        Background.runOne();
    }

    public static void unpark(Thread thread) {
    }

    public static Object getBlocker(Thread t) {
        return null;
    }

    public static void setCurrentBlocker(Object blocker) {
    }
}
