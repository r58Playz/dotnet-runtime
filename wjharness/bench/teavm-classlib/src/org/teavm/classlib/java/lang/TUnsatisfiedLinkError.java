package org.teavm.classlib.java.lang;

public class TUnsatisfiedLinkError extends LinkageError {
    private static final long serialVersionUID = 1L;

    public TUnsatisfiedLinkError() {
    }

    public TUnsatisfiedLinkError(String s) {
        super(s);
    }
}
