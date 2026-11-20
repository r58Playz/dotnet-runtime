package org.teavm.classlib.java.lang.management;

/* For Minecraft's SystemReport (crash reports): heap figures from Runtime, no JVM arguments. */
public final class TManagementFactory {
    private TManagementFactory() {
    }

    public static TMemoryMXBean getMemoryMXBean() {
        return new TMemoryMXBean();
    }

    public static TRuntimeMXBean getRuntimeMXBean() {
        return new TRuntimeMXBean();
    }
}
