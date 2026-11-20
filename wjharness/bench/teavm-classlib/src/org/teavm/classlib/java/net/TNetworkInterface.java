package org.teavm.classlib.java.net;

/* Guava's InetAddresses links to this; there are no network interfaces on TeaVM. */
public final class TNetworkInterface {
    private TNetworkInterface() {
    }

    public static TNetworkInterface getByName(String name) {
        return null;
    }

    public String getName() {
        return "none";
    }
}
