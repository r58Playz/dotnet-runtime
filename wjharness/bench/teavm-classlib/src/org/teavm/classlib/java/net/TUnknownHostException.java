package org.teavm.classlib.java.net;

import java.io.IOException;

public class TUnknownHostException extends IOException {
    private static final long serialVersionUID = 1L;

    public TUnknownHostException() {
    }

    public TUnknownHostException(String message) {
        super(message);
    }
}
