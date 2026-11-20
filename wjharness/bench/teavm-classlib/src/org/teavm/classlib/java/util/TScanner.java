package org.teavm.classlib.java.util;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.NoSuchElementException;

/* java.util.Scanner over a whole string, whitespace-delimited (Minecraft's chase debug client links to it). */
public final class TScanner implements Closeable {
    private final String text;
    private int pos;

    public TScanner(String source) {
        this.text = source;
    }

    public TScanner(Readable source) {
        StringBuilder sb = new StringBuilder();
        java.nio.CharBuffer buf = java.nio.CharBuffer.allocate(4096);
        try {
            while (source.read(buf) > 0) {
                buf.flip();
                sb.append(buf);
                buf.clear();
            }
        } catch (IOException e) {
            // what was read so far is the input
        }
        this.text = sb.toString();
    }

    public TScanner(InputStream in) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        try {
            int r;
            while ((r = in.read(buf)) > 0) {
                out.write(buf, 0, r);
            }
        } catch (IOException e) {
            // what was read so far is the input
        }
        this.text = new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    private void skipSpace() {
        while (pos < text.length() && Character.isWhitespace(text.charAt(pos))) {
            ++pos;
        }
    }

    public boolean hasNext() {
        skipSpace();
        return pos < text.length();
    }

    public String next() {
        skipSpace();
        if (pos >= text.length()) {
            throw new NoSuchElementException();
        }
        int start = pos;
        while (pos < text.length() && !Character.isWhitespace(text.charAt(pos))) {
            ++pos;
        }
        return text.substring(start, pos);
    }

    public int nextInt() {
        return Integer.parseInt(next());
    }

    public long nextLong() {
        return Long.parseLong(next());
    }

    public float nextFloat() {
        return Float.parseFloat(next());
    }

    public double nextDouble() {
        return Double.parseDouble(next());
    }

    public boolean nextBoolean() {
        return Boolean.parseBoolean(next());
    }

    public boolean hasNextLine() {
        return pos < text.length();
    }

    public String nextLine() {
        if (pos >= text.length()) {
            throw new NoSuchElementException();
        }
        int nl = text.indexOf('\n', pos);
        String line = nl < 0 ? text.substring(pos) : text.substring(pos, nl);
        pos = nl < 0 ? text.length() : nl + 1;
        return line.endsWith("\r") ? line.substring(0, line.length() - 1) : line;
    }

    public TScanner useLocale(TLocale locale) {
        return this;
    }

    @Override
    public void close() {
    }
}
