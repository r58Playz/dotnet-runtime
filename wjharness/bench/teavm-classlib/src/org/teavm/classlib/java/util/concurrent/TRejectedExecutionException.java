package org.teavm.classlib.java.util.concurrent;

public class TRejectedExecutionException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    public TRejectedExecutionException() {
    }

    public TRejectedExecutionException(String message) {
        super(message);
    }

    public TRejectedExecutionException(String message, Throwable cause) {
        super(message, cause);
    }

    public TRejectedExecutionException(Throwable cause) {
        super(cause);
    }
}
