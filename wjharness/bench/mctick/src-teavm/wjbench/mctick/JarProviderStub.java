package wjbench.mctick;

import java.io.IOException;
import java.net.URI;
import java.nio.channels.SeekableByteChannel;
import java.nio.file.AccessMode;
import java.nio.file.CopyOption;
import java.nio.file.DirectoryStream;
import java.nio.file.FileStore;
import java.nio.file.FileSystem;
import java.nio.file.LinkOption;
import java.nio.file.OpenOption;
import java.nio.file.Path;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.FileAttribute;
import java.nio.file.attribute.FileAttributeView;
import java.nio.file.spi.FileSystemProvider;
import java.util.Map;
import java.util.Set;

/*
 * The TeaVM arm's "jar" file system provider, registered through META-INF/services (TeaVM's
 * FileSystemProvider.installedProviders reads it). Minecraft's Util requires one at class initialisation (it opens
 * zipped packs with it); this bench loads no zipped pack, so every operation refuses.
 */
public final class JarProviderStub extends FileSystemProvider {
    private static UnsupportedOperationException no() {
        return new UnsupportedOperationException("zip file systems on TeaVM");
    }

    @Override
    public String getScheme() {
        return "jar";
    }

    @Override
    public FileSystem newFileSystem(URI uri, Map<String, ?> env) {
        throw no();
    }

    @Override
    public FileSystem newFileSystem(Path path, Map<String, ?> env) {
        throw no();
    }

    @Override
    public FileSystem getFileSystem(URI uri) {
        throw no();
    }

    @Override
    public Path getPath(URI uri) {
        throw no();
    }

    @Override
    public SeekableByteChannel newByteChannel(Path path, Set<? extends OpenOption> options, FileAttribute<?>... attrs) {
        throw no();
    }

    @Override
    public DirectoryStream<Path> newDirectoryStream(Path dir, DirectoryStream.Filter<? super Path> filter) {
        throw no();
    }

    @Override
    public void createDirectory(Path dir, FileAttribute<?>... attrs) {
        throw no();
    }

    @Override
    public void delete(Path path) {
        throw no();
    }

    @Override
    public void copy(Path source, Path target, CopyOption... options) {
        throw no();
    }

    @Override
    public void move(Path source, Path target, CopyOption... options) {
        throw no();
    }

    @Override
    public boolean isSameFile(Path path, Path path2) {
        throw no();
    }

    @Override
    public boolean isHidden(Path path) {
        throw no();
    }

    @Override
    public FileStore getFileStore(Path path) {
        throw no();
    }

    @Override
    public void checkAccess(Path path, AccessMode... modes) throws IOException {
        throw no();
    }

    @Override
    public <V extends FileAttributeView> V getFileAttributeView(Path path, Class<V> type, LinkOption... options) {
        throw no();
    }

    @Override
    public <A extends BasicFileAttributes> A readAttributes(Path path, Class<A> type, LinkOption... options) {
        throw no();
    }

    @Override
    public Map<String, Object> readAttributes(Path path, String attributes, LinkOption... options) {
        throw no();
    }

    @Override
    public void setAttribute(Path path, String attribute, Object value, LinkOption... options) {
        throw no();
    }
}
