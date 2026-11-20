package org.teavm.classlib.java.lang.management;

import java.util.Collections;
import java.util.List;

public class TRuntimeMXBean {
    public List<String> getInputArguments() {
        return Collections.emptyList();
    }

    public long getUptime() {
        return 0;
    }

    public String getName() {
        return "teavm";
    }
}
