package org.teavm.classlib.java.util.concurrent;

import java.util.concurrent.TimeUnit;
import wjteavm.Background;

public class TCountDownLatch {
    private long count;

    public TCountDownLatch(int count) {
        if (count < 0) {
            throw new IllegalArgumentException();
        }
        this.count = count;
    }

    public void await() throws InterruptedException {
        Background.awaitUntil(() -> count == 0, "CountDownLatch.await");
    }

    public boolean await(long timeout, TimeUnit unit) throws InterruptedException {
        while (count != 0 && Background.runOne()) {
            // run queued work
        }
        return count == 0;
    }

    public void countDown() {
        if (count > 0) {
            --count;
        }
    }

    public long getCount() {
        return count;
    }
}
