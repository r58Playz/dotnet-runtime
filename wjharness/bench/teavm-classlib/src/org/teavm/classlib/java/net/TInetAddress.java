package org.teavm.classlib.java.net;

import java.net.UnknownHostException;

/* Referenced by Gson's TypeAdapters (an InetAddress adapter); no name resolution on TeaVM. */
public class TInetAddress {
    private final String host;

    TInetAddress(String host) {
        this.host = host;
    }

    public static TInetAddress getByName(String host) throws UnknownHostException {
        throw new UnknownHostException(host);
    }

    public static TInetAddress[] getAllByName(String host) throws UnknownHostException {
        throw new UnknownHostException(host);
    }

    public static TInetAddress getLocalHost() throws UnknownHostException {
        throw new UnknownHostException("localhost");
    }

    public static TInetAddress getByAddress(byte[] addr) throws UnknownHostException {
        if (addr.length != 4 && addr.length != 16) {
            throw new UnknownHostException("addr is of illegal length");
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < addr.length; ++i) {
            if (i > 0) {
                sb.append(addr.length == 4 ? "." : ":");
            }
            sb.append(addr.length == 4 ? Integer.toString(addr[i] & 255) : Integer.toHexString(addr[i] & 255));
        }
        return addr.length == 4 ? new TInetAddress(sb.toString()) : new TInet6Address(sb.toString());
    }

    public static TInetAddress getByAddress(String host, byte[] addr) throws UnknownHostException {
        return getByAddress(addr);
    }

    public byte[] getAddress() {
        return new byte[4];
    }

    public static TInetAddress getLoopbackAddress() {
        return new TInetAddress("127.0.0.1");
    }

    public String getHostAddress() {
        return host;
    }

    public String getHostName() {
        return host;
    }

    @Override
    public String toString() {
        return "/" + host;
    }
}
