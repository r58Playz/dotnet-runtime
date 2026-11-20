package org.teavm.classlib.java.util.concurrent;

public class TCompletionException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    protected TCompletionException() {
    }

    protected TCompletionException(String message) {
        super(message);
    }

    public TCompletionException(String message, Throwable cause) {
        super(message, cause);
    }

    public TCompletionException(Throwable cause) {
        super(cause);
    }
}
