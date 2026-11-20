package org.teavm.classlib.java.nio.channels;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.SeekableByteChannel;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.OpenOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.FileAttribute;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/*
 * java.nio.channels.FileChannel over TeaVM's in-memory file system (through java.io.RandomAccessFile). Locks always
 * succeed (one process, one thread). No memory mapping. Used by Minecraft's RegionFile and DirectoryLock.
 */
public abstract class TFileChannel implements SeekableByteChannel {
    protected TFileChannel() {
    }

    public static TFileChannel open(Path path, OpenOption... options) throws IOException {
        return open(path, new HashSet<>(Arrays.asList(options)));
    }

    public static TFileChannel open(Path path, Set<? extends OpenOption> options, FileAttribute<?>... attrs)
            throws IOException {
        boolean write = options.contains(StandardOpenOption.WRITE) || options.contains(StandardOpenOption.APPEND);
        boolean create = options.contains(StandardOpenOption.CREATE) || options.contains(StandardOpenOption.CREATE_NEW);
        boolean exists = Files.exists(path);
        if (options.contains(StandardOpenOption.CREATE_NEW) && exists) {
            throw new java.nio.file.FileAlreadyExistsException(path.toString());
        }
        if (!exists && !(write && create)) {
            throw new NoSuchFileException(path.toString());
        }
        RandomAccessFile raf = new RandomAccessFile(path.toString(), write ? "rw" : "r");
        if (write && options.contains(StandardOpenOption.TRUNCATE_EXISTING)) {
            raf.setLength(0);
        }
        Impl ch = new Impl(raf, write);
        if (options.contains(StandardOpenOption.APPEND)) {
            ch.position(raf.length());
        }
        return ch;
    }

    /* RandomAccessFile.getChannel (wjharness TeaVM patch 0003). */
    public static TFileChannel forRandomAccessFile(RandomAccessFile raf, boolean writable) {
        return new Impl(raf, writable);
    }

    @Override
    public abstract int read(ByteBuffer dst) throws IOException;

    public abstract int read(ByteBuffer dst, long position) throws IOException;

    public long read(ByteBuffer[] dsts) throws IOException {
        long n = 0;
        for (ByteBuffer b : dsts) {
            int r = read(b);
            if (r < 0) {
                return n == 0 ? -1 : n;
            }
            n += r;
        }
        return n;
    }

    @Override
    public abstract int write(ByteBuffer src) throws IOException;

    public abstract int write(ByteBuffer src, long position) throws IOException;

    public long write(ByteBuffer[] srcs) throws IOException {
        long n = 0;
        for (ByteBuffer b : srcs) {
            n += write(b);
        }
        return n;
    }

    @Override
    public abstract long position() throws IOException;

    @Override
    public abstract TFileChannel position(long newPosition) throws IOException;

    @Override
    public abstract long size() throws IOException;

    @Override
    public abstract TFileChannel truncate(long size) throws IOException;

    public abstract void force(boolean metaData) throws IOException;

    public TFileLock lock() throws IOException {
        return lock(0, Long.MAX_VALUE, false);
    }

    public TFileLock lock(long position, long size, boolean shared) throws IOException {
        return new TFileLock(this, position, size, shared);
    }

    public TFileLock tryLock() throws IOException {
        return lock();
    }

    public TFileLock tryLock(long position, long size, boolean shared) throws IOException {
        return lock(position, size, shared);
    }

    public long transferTo(long position, long count, java.nio.channels.WritableByteChannel target)
            throws IOException {
        ByteBuffer b = ByteBuffer.allocate((int) Math.min(count, Math.max(0, size() - position)));
        read(b, position);
        b.flip();
        return target.write(b);
    }

    static final class Impl extends TFileChannel {
        private final RandomAccessFile raf;
        private final boolean writable;
        private boolean open = true;

        Impl(RandomAccessFile raf, boolean writable) {
            this.raf = raf;
            this.writable = writable;
        }

        private void check() throws IOException {
            if (!open) {
                throw new ClosedChannelException();
            }
        }

        @Override
        public int read(ByteBuffer dst) throws IOException {
            check();
            long p = raf.getFilePointer();
            int n = read(dst, p);
            if (n > 0) {
                raf.seek(p + n);
            }
            return n;
        }

        @Override
        public int read(ByteBuffer dst, long position) throws IOException {
            check();
            long len = raf.length();
            if (position >= len) {
                return dst.hasRemaining() ? -1 : 0;
            }
            int n = (int) Math.min(dst.remaining(), len - position);
            long save = raf.getFilePointer();
            raf.seek(position);
            if (dst.hasArray()) {
                raf.readFully(dst.array(), dst.arrayOffset() + dst.position(), n);
                dst.position(dst.position() + n);
            } else {
                byte[] tmp = new byte[n];
                raf.readFully(tmp);
                dst.put(tmp);
            }
            raf.seek(save);
            return n;
        }

        @Override
        public int write(ByteBuffer src) throws IOException {
            check();
            long p = raf.getFilePointer();
            int n = write(src, p);
            raf.seek(p + n);
            return n;
        }

        @Override
        public int write(ByteBuffer src, long position) throws IOException {
            check();
            if (!writable) {
                throw new java.nio.channels.NonWritableChannelException();
            }
            int n = src.remaining();
            long save = raf.getFilePointer();
            raf.seek(position);
            if (src.hasArray()) {
                raf.write(src.array(), src.arrayOffset() + src.position(), n);
                src.position(src.position() + n);
            } else {
                byte[] tmp = new byte[n];
                src.get(tmp);
                raf.write(tmp);
            }
            raf.seek(save);
            return n;
        }

        @Override
        public long position() throws IOException {
            check();
            return raf.getFilePointer();
        }

        @Override
        public TFileChannel position(long newPosition) throws IOException {
            check();
            raf.seek(newPosition);
            return this;
        }

        @Override
        public long size() throws IOException {
            check();
            return raf.length();
        }

        @Override
        public TFileChannel truncate(long size) throws IOException {
            check();
            if (size < raf.length()) {
                raf.setLength(size);
            }
            return this;
        }

        @Override
        public void force(boolean metaData) {
        }

        @Override
        public boolean isOpen() {
            return open;
        }

        @Override
        public void close() throws IOException {
            if (open) {
                open = false;
                raf.close();
            }
        }
    }
}
