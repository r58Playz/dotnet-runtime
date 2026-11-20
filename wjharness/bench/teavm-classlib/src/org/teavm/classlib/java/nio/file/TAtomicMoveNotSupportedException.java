package org.teavm.classlib.java.nio.file;

import java.nio.file.FileSystemException;

public class TAtomicMoveNotSupportedException extends FileSystemException {
    private static final long serialVersionUID = 1L;

    public TAtomicMoveNotSupportedException(String source, String target, String reason) {
        super(source, target, reason);
    }
}
