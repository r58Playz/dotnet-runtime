package org.teavm.classlib.java.security.spec;

/* Minecraft's Crypt (chat signing keys) links to these; no key factories on TeaVM. */
public abstract class TEncodedKeySpec implements TKeySpec {
    private final byte[] encoded;

    public TEncodedKeySpec(byte[] encodedKey) {
        this.encoded = encodedKey.clone();
    }

    public byte[] getEncoded() {
        return encoded.clone();
    }

    public abstract String getFormat();
}
