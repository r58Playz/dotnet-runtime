package org.teavm.classlib.java.security;

import java.security.GeneralSecurityException;
import java.security.NoSuchAlgorithmException;
import org.teavm.classlib.java.security.spec.TKeySpec;

/* No key factories on TeaVM (Minecraft's Crypt links to this for chat signing keys, which an offline bench has none of). */
public class TKeyFactory {
    private TKeyFactory() {
    }

    public static TKeyFactory getInstance(String algorithm) throws NoSuchAlgorithmException {
        throw new NoSuchAlgorithmException(algorithm + " KeyFactory not available");
    }

    public final TPublicKey generatePublic(TKeySpec keySpec) throws GeneralSecurityException {
        throw new GeneralSecurityException("no key factories on TeaVM");
    }

    public final TPrivateKey generatePrivate(TKeySpec keySpec) throws GeneralSecurityException {
        throw new GeneralSecurityException("no key factories on TeaVM");
    }
}
