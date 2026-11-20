package org.teavm.classlib.java.nio.file.attribute;

import java.io.IOException;

/* Referenced by commons-io's PathUtils; the in-memory file system offers no DOS view (getFileAttributeView: null). */
public interface TDosFileAttributeView extends TFileAttributeView {
    void setReadOnly(boolean value) throws IOException;

    void setHidden(boolean value) throws IOException;

    void setSystem(boolean value) throws IOException;

    void setArchive(boolean value) throws IOException;
}
