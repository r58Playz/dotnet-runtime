package org.teavm.classlib.java.util.concurrent;

import java.util.concurrent.TimeUnit;

public interface TDelayed extends Comparable<TDelayed> {
    long getDelay(TimeUnit unit);
}
