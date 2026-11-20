package org.teavm.classlib.java.lang;

import java.io.InputStream;
import java.io.OutputStream;

/* Runtime.exec always throws on TeaVM (wjharness patch 0003), so no Process is ever made. */
public abstract class TProcess {
    public abstract OutputStream getOutputStream();

    public abstract InputStream getInputStream();

    public abstract InputStream getErrorStream();

    public abstract int waitFor() throws InterruptedException;

    public abstract int exitValue();

    public abstract void destroy();
}
