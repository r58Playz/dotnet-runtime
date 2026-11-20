package wjteavm;

import java.util.ArrayDeque;
import java.util.function.BooleanSupplier;

/*
 * The one "thread pool" of a TeaVM arm: a FIFO of tasks run on the only thread there is.
 *
 * TeaVM's linear-memory backend can run threads only as coroutines, and it coroutine-transforms every method that can
 * reach a suspending (@Async) method: Thread.start/sleep/yield, Object.wait, a contended monitor. That rewrite is not
 * TeaVM's codegen at its best, and on Minecraft it would reach most of the tick. So nothing in this classlib extension
 * starts a thread or suspends: executors post here, and every place that would block (CompletableFuture.join/get,
 * LockSupport.park, a timed BlockingQueue.poll, Future.get) runs queued tasks until its condition holds. Work that
 * would never finish (the condition is false and nothing is queued) throws instead of hanging.
 *
 * The arm's driver decides when queued work runs between ticks (mctick's Platform.drainBackground).
 */
public final class Background {
    private static final ArrayDeque<Runnable> QUEUE = new ArrayDeque<>();
    /* counters for the arm's report */
    public static long posted;
    public static long ran;
    public static int maxDepth;
    private static int depth;

    private Background() {
    }

    public static void post(Runnable task) {
        if (task == null) {
            throw new NullPointerException();
        }
        ++posted;
        QUEUE.add(task);
    }

    public static boolean isEmpty() {
        return QUEUE.isEmpty();
    }

    public static int size() {
        return QUEUE.size();
    }

    /* Run the oldest queued task; false if there was none. */
    public static boolean runOne() {
        Runnable task = QUEUE.poll();
        if (task == null) {
            return false;
        }
        ++ran;
        if (++depth > maxDepth) {
            maxDepth = depth;
        }
        try {
            task.run();
        } catch (Throwable e) {
            // a pool thread would log and carry on; so does this
            System.out.println("[wjteavm] background task threw " + e);
            e.printStackTrace();
        } finally {
            --depth;
        }
        return true;
    }

    public static int drain() {
        int n = 0;
        while (runOne()) {
            ++n;
        }
        return n;
    }

    /*
     * Targets of mctick's single-threaded lowering (bench/mctick/teavm-patch/PatchClasses.java), which rewrites every
     * call to Thread.yield/sleep/join and Object.wait in Minecraft and its libraries into these: with one thread there
     * is nothing to wait for except queued work, and calling TeaVM's own (suspending) versions would make TeaVM
     * coroutine-transform every caller.
     */
    public static void yieldHint() {
        runOne();
    }

    public static void sleep(long millis) {
        runOne();
    }

    public static void sleep(long millis, int nanos) {
        runOne();
    }

    public static void waitOn(Object monitor) {
        runOne();
    }

    public static void waitOn(Object monitor, long millis) {
        runOne();
    }

    public static void waitOn(Object monitor, long millis, int nanos) {
        runOne();
    }

    public static void join(Thread thread) {
    }

    public static void join(Thread thread, long millis) {
    }

    public static void join(Thread thread, long millis, int nanos) {
    }

    /* Run queued tasks until done holds. */
    public static void awaitUntil(BooleanSupplier done, String what) {
        while (!done.getAsBoolean()) {
            if (!runOne()) {
                throw new IllegalStateException("[wjteavm] single-threaded wait can never finish: " + what);
            }
        }
    }
}
