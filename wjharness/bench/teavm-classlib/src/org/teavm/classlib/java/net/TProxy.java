package org.teavm.classlib.java.net;

/* java.net.Proxy: the server is handed NO_PROXY and never connects anywhere. */
public class TProxy {
    public enum Type {
        DIRECT,
        HTTP,
        SOCKS
    }

    public static final TProxy NO_PROXY = new TProxy();

    private final Type type;
    private final TSocketAddress sa;

    private TProxy() {
        type = Type.DIRECT;
        sa = null;
    }

    public TProxy(Type type, TSocketAddress sa) {
        this.type = type;
        this.sa = sa;
    }

    public Type type() {
        return type;
    }

    public TSocketAddress address() {
        return sa;
    }

    @Override
    public String toString() {
        return type == Type.DIRECT ? "DIRECT" : type + " @ " + sa;
    }
}
