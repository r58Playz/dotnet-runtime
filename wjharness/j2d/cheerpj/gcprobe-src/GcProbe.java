// j2d/cheerpj/gcprobe-src (2026-11-04): what of the JVM's GC / object-model contract survives on CheerpJ, against HotSpot.
// Built with JDK 8's javac (sun.misc.Unsafe in rt.jar) so every engine runs the same bytes. One "[t] key: value" line each.
import java.lang.ref.*;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public class GcProbe {
    static volatile int finalized = 0;
    static class F { byte[] pad = new byte[1024]; protected void finalize() { finalized++; } }
    static class P { int a; long b; Object c; volatile int d; byte e; }
    static void p(String k, Object v) { System.out.println("[t] " + k + ": " + v); }
    static void churn() { for (int i = 0; i < 200; i++) { byte[] g = new byte[64 * 1024]; g[i & 1023] = 1; } }
    static final Object lock = new Object();
    static int shared = 0;
    static boolean ready = false;

    public static void main(String[] args) throws Exception {
        p("java.version", System.getProperty("java.version") + " / " + System.getProperty("java.vm.name"));
        Runtime rt = Runtime.getRuntime();
        p("max/total/free MB", rt.maxMemory() / 1048576 + " / " + rt.totalMemory() / 1048576 + " / " + rt.freeMemory() / 1048576);

        // ---- identity hash
        Object o1 = new Object(), o2 = new Object();
        int h1 = System.identityHashCode(o1);
        p("identityHash stable, == hashCode()", h1 == System.identityHashCode(o1) && h1 == o1.hashCode());
        HashSet<Integer> hs = new HashSet<Integer>();
        for (int i = 0; i < 100000; i++) hs.add(System.identityHashCode(new Object()));
        p("identityHash distinct of 100000", hs.size());

        // ---- references, finalization
        ReferenceQueue<Object> q = new ReferenceQueue<Object>();
        Object w = new byte[1 << 20];
        WeakReference<Object> wr = new WeakReference<Object>(w, q);
        SoftReference<Object> sr = new SoftReference<Object>(new byte[1 << 20]);
        Object ph = new Object();
        PhantomReference<Object> pr = new PhantomReference<Object>(ph, q);
        for (int i = 0; i < 1000; i++) new F();
        w = null; ph = null;
        long t0 = System.nanoTime(); long tw = -1; int enq = 0;
        for (int round = 0; round < 300 && (tw < 0 || enq < 2 || finalized < 1000); round++) {
            System.gc(); churn(); Thread.sleep(10);
            if (tw < 0 && wr.get() == null) tw = (System.nanoTime() - t0) / 1000000;
            while (q.poll() != null) enq++;
        }
        p("WeakReference cleared after ms (-1 = never in 3 s)", tw);
        p("ReferenceQueue enqueued (weak + phantom, of 2)", enq);
        p("SoftReference cleared", sr.get() == null);
        p("finalize() run (of 1000)", finalized);
        WeakHashMap<Object, Integer> whm = new WeakHashMap<Object, Integer>();
        for (int i = 0; i < 1000; i++) whm.put(new Object(), i);
        for (int round = 0; round < 100 && whm.size() > 0; round++) { System.gc(); churn(); Thread.sleep(10); }
        p("WeakHashMap entries left (of 1000)", whm.size());
        // a busy loop that never sleeps: can a weak ref clear while Java does not yield?
        Object w2 = new byte[1 << 20]; WeakReference<Object> wr2 = new WeakReference<Object>(w2); w2 = null;
        boolean busyCleared = false;
        for (int round = 0; round < 3000 && !busyCleared; round++) { System.gc(); churn(); busyCleared = wr2.get() == null; }
        p("WeakReference cleared in a loop that never sleeps", busyCleared);

        // ---- Unsafe
        Field uf = Class.forName("sun.misc.Unsafe").getDeclaredField("theUnsafe"); uf.setAccessible(true);
        sun.misc.Unsafe U = (sun.misc.Unsafe) uf.get(null);
        long offA = U.objectFieldOffset(P.class.getDeclaredField("a")), offB = U.objectFieldOffset(P.class.getDeclaredField("b")),
             offC = U.objectFieldOffset(P.class.getDeclaredField("c")), offD = U.objectFieldOffset(P.class.getDeclaredField("d")),
             offE = U.objectFieldOffset(P.class.getDeclaredField("e"));
        p("field offsets a b c d e", offA + " " + offB + " " + offC + " " + offD + " " + offE);
        P x = new P();
        U.putInt(x, offA, 42); U.putLong(x, offB, 1L << 40); U.putObject(x, offC, "s");
        p("putInt/putLong/putObject by offset", x.a == 42 && x.b == (1L << 40) && "s".equals(x.c));
        p("CAS int ok / fails when stale", U.compareAndSwapInt(x, offD, 0, 7) + " / " + U.compareAndSwapInt(x, offD, 0, 9) + " (d=" + x.d + ")");
        long[] offs = { offA, offD };
        long dyn = offs[Integer.parseInt(System.getProperty("probe.pick", "1"))];   // an offset the JIT cannot see as a constant
        p("getInt at a runtime-chosen offset (expect 7)", U.getInt(x, dyn));
        x.a = 0x01020304;
        String sub;
        try { sub = String.valueOf(U.getByte(x, offA)); } catch (Throwable t) { sub = t.toString(); }
        p("getByte inside an int field (HotSpot LE: 4)", sub);
        try { sub = String.valueOf(U.getInt(x, offB)); } catch (Throwable t) { sub = t.toString(); }
        p("getInt of a long field's low half (HotSpot LE: 0)", sub);
        int[] arr = new int[8]; long base = U.arrayBaseOffset(int[].class), scale = U.arrayIndexScale(int[].class);
        U.putInt(arr, base + 3 * scale, 99);
        p("int[] base/scale, putInt at [3]", base + "/" + scale + ", arr[3]=" + arr[3]);
        try {
            p("step", "allocateMemory"); long addr = U.allocateMemory(64);
            p("step", "putLong at " + addr); U.putLong(addr + 8, 0x1122334455667788L); p("step", "getLong");
            p("allocateMemory + putLong/getLong, getByte (LE: 0x88)", Long.toHexString(U.getLong(addr + 8)) + ", " + Integer.toHexString(U.getByte(addr + 8) & 255));
            U.freeMemory(addr);
        } catch (Throwable t) { p("allocateMemory", t.toString()); }
        p("step", "allocateDirect"); ByteBuffer bb = ByteBuffer.allocateDirect(64); bb.putLong(8, 0x1122334455667788L);
        p("direct ByteBuffer putLong/getLong, isDirect", Long.toHexString(bb.getLong(8)) + ", " + bb.isDirect());

        // ---- atomics, monitors, threads
        AtomicInteger ai = new AtomicInteger(); for (int i = 0; i < 1000000; i++) ai.incrementAndGet();
        ConcurrentHashMap<Integer, Integer> chm = new ConcurrentHashMap<Integer, Integer>(); for (int i = 0; i < 10000; i++) chm.merge(i & 1023, 1, Integer::sum);
        AtomicReferenceArray<String> ara = new AtomicReferenceArray<String>(4);
        p("AtomicInteger 1e6 / CHM 1024 keys sum / ARA CAS", ai.get() + " / " + chm.size() + " " + chm.values().stream().mapToInt(Integer::intValue).sum() + " / " + ara.compareAndSet(2, null, "x") + ara.get(2));
        Thread[] ts = new Thread[4];
        for (int k = 0; k < 4; k++) { ts[k] = new Thread(() -> { for (int i = 0; i < 100000; i++) synchronized (lock) { shared++; } }); ts[k].start(); }
        for (Thread t : ts) t.join();
        p("4 threads x 100000 synchronized increments", shared);
        Thread waiter = new Thread(() -> { synchronized (lock) { while (!ready) try { lock.wait(); } catch (InterruptedException e) { } shared = -1; } });
        waiter.start(); Thread.sleep(20);
        synchronized (lock) { ready = true; lock.notifyAll(); }
        waiter.join(2000);
        p("wait/notifyAll handoff", shared == -1 && !waiter.isAlive());
        p("done", 1);
    }
}
