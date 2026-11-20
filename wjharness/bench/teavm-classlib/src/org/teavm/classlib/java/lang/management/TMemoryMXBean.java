package org.teavm.classlib.java.lang.management;

public class TMemoryMXBean {
    public TMemoryUsage getHeapMemoryUsage() {
        Runtime rt = Runtime.getRuntime();
        return new TMemoryUsage(0, rt.totalMemory() - rt.freeMemory(), rt.totalMemory(), rt.totalMemory());
    }

    public TMemoryUsage getNonHeapMemoryUsage() {
        return new TMemoryUsage(0, 0, 0, -1);
    }
}
