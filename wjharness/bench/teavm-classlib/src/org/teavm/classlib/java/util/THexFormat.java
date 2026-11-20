package org.teavm.classlib.java.util;

/* java.util.HexFormat (the parts Minecraft 26.2 and its libraries use). */
public final class THexFormat {
    private static final char[] LOWER = "0123456789abcdef".toCharArray();
    private static final char[] UPPER = "0123456789ABCDEF".toCharArray();
    private static final THexFormat HEX = new THexFormat("", "", "", false);

    private final String delimiter;
    private final String prefix;
    private final String suffix;
    private final boolean upper;

    private THexFormat(String delimiter, String prefix, String suffix, boolean upper) {
        this.delimiter = delimiter;
        this.prefix = prefix;
        this.suffix = suffix;
        this.upper = upper;
    }

    public static THexFormat of() {
        return HEX;
    }

    public static THexFormat ofDelimiter(String delimiter) {
        return new THexFormat(delimiter, "", "", false);
    }

    public THexFormat withDelimiter(String d) {
        return new THexFormat(d, prefix, suffix, upper);
    }

    public THexFormat withPrefix(String p) {
        return new THexFormat(delimiter, p, suffix, upper);
    }

    public THexFormat withSuffix(String s) {
        return new THexFormat(delimiter, prefix, s, upper);
    }

    public THexFormat withUpperCase() {
        return new THexFormat(delimiter, prefix, suffix, true);
    }

    public THexFormat withLowerCase() {
        return new THexFormat(delimiter, prefix, suffix, false);
    }

    public String delimiter() {
        return delimiter;
    }

    public String prefix() {
        return prefix;
    }

    public String suffix() {
        return suffix;
    }

    public boolean isUpperCase() {
        return upper;
    }

    public String formatHex(byte[] bytes) {
        return formatHex(bytes, 0, bytes.length);
    }

    public String formatHex(byte[] bytes, int from, int to) {
        StringBuilder sb = new StringBuilder();
        char[] digits = upper ? UPPER : LOWER;
        for (int i = from; i < to; ++i) {
            if (i > from) {
                sb.append(delimiter);
            }
            sb.append(prefix).append(digits[(bytes[i] >> 4) & 15]).append(digits[bytes[i] & 15]).append(suffix);
        }
        return sb.toString();
    }

    public byte[] parseHex(CharSequence s) {
        String str = s.toString();
        int stride = prefix.length() + 2 + suffix.length() + delimiter.length();
        int n = str.isEmpty() ? 0 : (str.length() + delimiter.length()) / stride;
        byte[] out = new byte[n];
        for (int i = 0; i < n; ++i) {
            int at = i * stride + prefix.length();
            out[i] = (byte) ((fromHexDigit(str.charAt(at)) << 4) | fromHexDigit(str.charAt(at + 1)));
        }
        return out;
    }

    public char toLowHexDigit(int value) {
        return (upper ? UPPER : LOWER)[value & 15];
    }

    public char toHighHexDigit(int value) {
        return (upper ? UPPER : LOWER)[(value >> 4) & 15];
    }

    public String toHexDigits(byte value) {
        return digits(value & 0xFFL, 2);
    }

    public String toHexDigits(char value) {
        return digits(value, 4);
    }

    public String toHexDigits(short value) {
        return digits(value & 0xFFFFL, 4);
    }

    public String toHexDigits(int value) {
        return digits(value & 0xFFFFFFFFL, 8);
    }

    public String toHexDigits(long value) {
        return digits(value, 16);
    }

    public String toHexDigits(long value, int digits) {
        if (digits < 0 || digits > 16) {
            throw new IllegalArgumentException();
        }
        return digits(value, digits);
    }

    private String digits(long value, int n) {
        char[] out = new char[n];
        char[] digits = upper ? UPPER : LOWER;
        for (int i = n - 1; i >= 0; --i) {
            out[i] = digits[(int) (value & 15)];
            value >>>= 4;
        }
        return new String(out);
    }

    public static boolean isHexDigit(int ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    public static int fromHexDigit(int ch) {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        throw new NumberFormatException("not a hexadecimal digit: \"" + (char) ch + "\"");
    }

    public static int fromHexDigits(CharSequence s) {
        return fromHexDigits(s, 0, s.length());
    }

    public static int fromHexDigits(CharSequence s, int from, int to) {
        if (to - from > 8) {
            throw new IllegalArgumentException("string length greater than 8");
        }
        int v = 0;
        for (int i = from; i < to; ++i) {
            v = (v << 4) | fromHexDigit(s.charAt(i));
        }
        return v;
    }

    public static long fromHexDigitsToLong(CharSequence s) {
        return fromHexDigitsToLong(s, 0, s.length());
    }

    public static long fromHexDigitsToLong(CharSequence s, int from, int to) {
        if (to - from > 16) {
            throw new IllegalArgumentException("string length greater than 16");
        }
        long v = 0;
        for (int i = from; i < to; ++i) {
            v = (v << 4) | fromHexDigit(s.charAt(i));
        }
        return v;
    }
}
