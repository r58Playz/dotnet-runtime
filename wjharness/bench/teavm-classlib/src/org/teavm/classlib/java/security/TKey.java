package org.teavm.classlib.java.security;

public interface TKey extends java.io.Serializable {
    String getAlgorithm();

    String getFormat();

    byte[] getEncoded();
}
