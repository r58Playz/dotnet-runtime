package org.teavm.classlib.java.security.spec;

public class TX509EncodedKeySpec extends TEncodedKeySpec {
    public TX509EncodedKeySpec(byte[] encodedKey) {
        super(encodedKey);
    }

    @Override
    public final String getFormat() {
        return "X.509";
    }
}
