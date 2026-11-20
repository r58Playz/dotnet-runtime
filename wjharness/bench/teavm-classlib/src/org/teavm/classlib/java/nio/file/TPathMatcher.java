package org.teavm.classlib.java.nio.file;

import java.nio.file.Path;

public interface TPathMatcher {
    boolean matches(Path path);
}
