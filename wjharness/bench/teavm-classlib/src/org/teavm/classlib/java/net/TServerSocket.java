package org.teavm.classlib.java.net;

import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/* No sockets on TeaVM (Minecraft's HttpUtil.getAvailablePort and the chase debug feature link to this). */
public class TServerSocket implements Closeable {
    public TServerSocket() throws IOException {
        throw new IOException("no sockets on TeaVM");
    }

    public TServerSocket(int port) throws IOException {
        throw new IOException("no sockets on TeaVM");
    }

    public TServerSocket(String host, int port) throws IOException {
        throw new IOException("no sockets on TeaVM");
    }

    public TServerSocket(int port, int backlog, TInetAddress bindAddr) throws IOException {
        throw new IOException("no sockets on TeaVM");
    }

    public boolean isClosed() {
        return true;
    }

    public int getLocalPort() {
        return -1;
    }

    public InputStream getInputStream() throws IOException {
        throw new IOException("no sockets on TeaVM");
    }

    public OutputStream getOutputStream() throws IOException {
        throw new IOException("no sockets on TeaVM");
    }

    public TSocket accept() throws IOException {
        throw new IOException("no sockets on TeaVM");
    }

    @Override
    public void close() {
    }
}
