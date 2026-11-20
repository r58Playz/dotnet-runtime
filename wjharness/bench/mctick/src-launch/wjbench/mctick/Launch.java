package wjbench.mctick;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/*
 * mctick.jar's Main-Class, for the ours arm (ikvmcraft ?bench&jar=mctick.jar, which puts only this jar on the class
 * path). It builds a class loader over the seed profile's own copy of 26.2 -- <root>/versions/26.2/client.jar and the
 * libraries in classpath.txt (a resource of this jar, written by build.sh from the stage's class path) under
 * <root>/libraries -- with this jar first, and runs Mctick.main through it, so Minecraft's classes and the bench's
 * come from one loader, as on HotSpot.
 *
 * System properties (ikvmcraft passes ?jprop.<k>=<v>): mctick.root (default /libsdl/ikvmcraft), mctick.version (26.2),
 * plus Mctick's own (mctick.world, mctick.work, the schedule). A missing jar is reported by name before anything runs.
 */
public final class Launch {
    private Launch() {
    }

    public static void main(String[] args) throws Exception {
        String root = System.getProperty("mctick.root", "/libsdl/ikvmcraft");
        String version = System.getProperty("mctick.version", "26.2");
        File self = new File(Launch.class.getProtectionDomain().getCodeSource().getLocation().toURI());
        List<URL> urls = new ArrayList<>();
        urls.add(self.toURI().toURL());
        List<String> missing = new ArrayList<>();
        add(urls, missing, new File(root + "/versions/" + version + "/client.jar"));
        try (InputStream in = Launch.class.getResourceAsStream("/wjbench/mctick/classpath.txt");
             BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (!line.isEmpty() && !line.startsWith("#")) {
                    add(urls, missing, new File(root + "/libraries/" + line));
                }
            }
        }
        if (!missing.isEmpty()) {
            System.out.println("[mctick] LOAD FAILED: missing " + missing);
            return;
        }
        System.out.println("[mctick] launch: " + urls.size() + " class path entries under " + root);
        URLClassLoader loader = new URLClassLoader(urls.toArray(new URL[0]), ClassLoader.getPlatformClassLoader());
        Thread.currentThread().setContextClassLoader(loader);
        Method main = Class.forName("wjbench.mctick.Mctick", true, loader).getMethod("main", String[].class);
        main.invoke(null, (Object) args);
    }

    private static void add(List<URL> urls, List<String> missing, File f) throws Exception {
        if (f.isFile()) {
            urls.add(f.toURI().toURL());
        } else {
            missing.add(f.getPath());
        }
    }
}
