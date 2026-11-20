package org.teavm.classlib.java.security.cert;

/* Referenced by netty; nothing on mctick's path makes one. */
public abstract class TCertificate {
    private final String type;

    protected TCertificate(String type) {
        this.type = type;
    }

    public final String getType() {
        return type;
    }

    public abstract byte[] getEncoded();
}
