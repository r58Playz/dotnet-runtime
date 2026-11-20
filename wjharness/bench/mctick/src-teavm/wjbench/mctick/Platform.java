package wjbench.mctick;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.slf4j.Logger;
import org.teavm.interop.Address;
import org.teavm.interop.Import;
import wjteavm.Background;

/*
 * The TeaVM arm (linear-memory wasm, one thread):
 *  - init() unpacks the data zip the page fetched (mctick.html: mctick-data.zip, read through the mctick.blob* imports)
 *    into TeaVM's in-memory file system: res/ = the class-path resources Minecraft reads (data/, the lang files,
 *    version.json; TeaVM patch 0003 serves getResource* from /res/), world/<save>/ = the save. It sets mctick.world and
 *    mctick.work to match.
 *  - Minecraft's executors are the background queue (bench/teavm-classlib, wjteavm.Background): drainBackground() runs
 *    it where the JVM arms' pool threads would have run the same tasks concurrently.
 */
public final class Platform {
    static final String NAME = "teavm";
    /* stdout as the program starts; Minecraft's Bootstrap.wrapStreams later replaces System.out with a logger */
    static final java.io.PrintStream STDOUT = System.out;

    private Platform() {
    }

    @Import(module = "mctick", name = "blobLength")
    private static native int blobLength();

    @Import(module = "mctick", name = "blobRead")
    private static native void blobRead(int offset, Address dst, int length);

    static void init() throws IOException {
        long t0 = System.nanoTime();
        int n = blobLength();
        byte[] blob = new byte[n];
        blobRead(0, Address.ofData(blob), n);
        int files = 0;
        String save = null;
        try (ZipInputStream zip = new ZipInputStream(new ByteArrayInputStream(blob))) {
            byte[] buf = new byte[65536];
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null) {
                File f = new File("/" + e.getName());
                if (e.isDirectory()) {
                    f.mkdirs();
                    continue;
                }
                f.getParentFile().mkdirs();
                try (OutputStream out = new FileOutputStream(f)) {
                    int r;
                    while ((r = zip.read(buf)) > 0) {
                        out.write(buf, 0, r);
                    }
                }
                ++files;
                String name = e.getName();
                if (save == null && name.startsWith("world/") && name.endsWith("/level.dat")) {
                    save = name.substring(0, name.length() - "/level.dat".length());
                }
            }
        }
        if (save == null) {
            throw new IOException("mctick-data.zip holds no world/<save>/level.dat");
        }
        System.setProperty("mctick.world", "/" + save);
        System.setProperty("mctick.work", "/work");
        out("[mctick] teavm: unpacked " + files + " files (" + n / 1024 + " KiB zip) into the in-memory file system in "
                + (System.nanoTime() - t0) / 1000000L + " ms");
    }

    static void idle() {
        Background.runOne();
    }

    static int drainBackground() {
        return Background.drain();
    }

    static String backgroundStats() {
        return "background tasks posted " + Background.posted + ", ran " + Background.ran + ", max depth "
                + Background.maxDepth;
    }

    public static Logger logger(String name) {
        return new ConsoleLogger(name);
    }

    static void out(String line) {
        System.out.println(line);
    }

    static void exit(int code) {
    }
}
