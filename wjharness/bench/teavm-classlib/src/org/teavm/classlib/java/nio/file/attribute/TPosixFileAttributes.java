package org.teavm.classlib.java.nio.file.attribute;

import java.nio.file.attribute.BasicFileAttributes;
import java.util.Set;

/* Referenced by Minecraft's FileUtil and commons-io; the in-memory file system offers no POSIX view. */
public interface TPosixFileAttributes extends BasicFileAttributes {
    Set<TPosixFilePermission> permissions();
}
