package org.teavm.classlib.java.net;

/* Guava's InetAddresses links to this; no name resolution on TeaVM. */
public final class TInet6Address extends TInetAddress {
    TInet6Address(String host) {
        super(host);
    }

    public static TInet6Address getByAddress(String host, byte[] addr, TNetworkInterface nif) {
        return new TInet6Address(host);
    }

    public static TInet6Address getByAddress(String host, byte[] addr, int scopeId) {
        return new TInet6Address(host);
    }

    public int getScopeId() {
        return 0;
    }
}
