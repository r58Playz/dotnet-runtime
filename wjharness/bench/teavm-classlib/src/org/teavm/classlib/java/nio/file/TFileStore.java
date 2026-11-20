package org.teavm.classlib.java.nio.file;

import java.io.IOException;

/* The in-memory file system's one store: the heap. */
public abstract class TFileStore {
    protected TFileStore() {
    }

    public abstract String name();

    public abstract String type();

    public abstract boolean isReadOnly();

    public abstract long getTotalSpace() throws IOException;

    public abstract long getUsableSpace() throws IOException;

    public abstract long getUnallocatedSpace() throws IOException;

    public long getBlockSize() throws IOException {
        return 4096;
    }

    public static TFileStore memory() {
        return new TFileStore() {
            @Override
            public String name() {
                return "memory";
            }

            @Override
            public String type() {
                return "memory";
            }

            @Override
            public boolean isReadOnly() {
                return false;
            }

            @Override
            public long getTotalSpace() {
                return Long.MAX_VALUE;
            }

            @Override
            public long getUsableSpace() {
                return Long.MAX_VALUE;
            }

            @Override
            public long getUnallocatedSpace() {
                return Long.MAX_VALUE;
            }
        };
    }
}
