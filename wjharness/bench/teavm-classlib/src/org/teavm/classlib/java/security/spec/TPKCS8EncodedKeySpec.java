package org.teavm.classlib.java.security.spec;

public class TPKCS8EncodedKeySpec extends TEncodedKeySpec {
    public TPKCS8EncodedKeySpec(byte[] encodedKey) {
        super(encodedKey);
    }

    @Override
    public final String getFormat() {
        return "PKCS#8";
    }
}
