package wjbench.mctick;

/*
 * What differs between arms. This is the JVM / IKVM one (HotSpot, ours, IKVM on CoreCLR): Minecraft's own executors
 * and threads run as the game runs them, so idle() has nothing to do. The TeaVM arm has its own (src-teavm/).
 */
final class Platform {
    static final String NAME = "jvm";

    private Platform() {
    }

    /* Before anything else runs (the TeaVM arm builds its file system here). */
    static void init() {
    }

    static String backgroundStats() {
        return "background: Minecraft's own executor threads";
    }

    /* Called where the server would wait for its next tick (prepareLevels, the settle loop). */
    static void idle() {
        Thread.yield();
    }

    /* Run any background work the platform queued instead of running on a thread; returns tasks run. */
    static int drainBackground() {
        return 0;
    }

    static void out(String line) {
        System.out.println(line);
    }

    /* HotSpot needs System.exit (Minecraft's IO pool threads are not daemons). In ikvmcraft's page an exit would take
     * the runtime down with the console still draining, so the ours arm passes -Dmctick.noexit=true and returns. */
    static void exit(int code) {
        System.out.flush();
        if (!Boolean.getBoolean("mctick.noexit")) {
            System.exit(code);
        }
    }
}
