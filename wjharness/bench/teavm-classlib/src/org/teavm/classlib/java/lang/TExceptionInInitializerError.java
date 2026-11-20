package org.teavm.classlib.java.lang;

public class TExceptionInInitializerError extends LinkageError {
    private static final long serialVersionUID = 1L;

    public TExceptionInInitializerError() {
    }

    public TExceptionInInitializerError(Throwable thrown) {
        super(null, thrown);
    }

    public TExceptionInInitializerError(String s) {
        super(s);
    }

    public Throwable getException() {
        return getCause();
    }
}
