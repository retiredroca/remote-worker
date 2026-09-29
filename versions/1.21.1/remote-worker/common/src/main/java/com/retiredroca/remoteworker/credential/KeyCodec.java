package com.retiredroca.remoteworker.credential;

/**
 * The agent's key format, and the machine id derived from it.
 *
 * <p>This is the Java half of a format the C++ agent also implements ({@code agent/src/credential.cpp}),
 * and the two must agree byte for byte: the mod sends the key's raw bytes in {@code OPEN_SESSION},
 * and a machine id computed differently here would be an id the agent refuses to answer to. The
 * shared test {@code ProtocolVectorsCheck} is where that agreement is checked.
 *
 * <p>A key is 20 bytes of CSPRNG output, written as {@code rw1_} followed by 32 characters of
 * Crockford base32. The id is derived from the key rather than being separate, so it is public
 * while the key stays secret -- see {@code PROTOCOL.md} section 3.
 */
public final class KeyCodec {

    public static final String PREFIX = "rw1_";
    public static final int KEY_BYTES = 20;
    public static final int KEY_CHARS = 32;
    public static final int MACHINE_ID_CHARS = 16;

    /** Crockford base32: the digits and letters minus I, L, O and U, so a misread is detectable. */
    private static final String ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

    private KeyCodec() {}

    /**
     * Decodes a key as the user sees it, with or without the {@code rw1_} prefix, case-insensitively
     * because a key pasted through a chat client may have been lower-cased.
     *
     * @return the key's bytes, or {@code null} if the text is not a well-formed key
     */
    public static byte[] decode(String text) {
        if (text == null) {
            return null;
        }
        String trimmed = text.trim();
        if (trimmed.regionMatches(true, 0, PREFIX, 0, PREFIX.length())) {
            trimmed = trimmed.substring(PREFIX.length());
        }
        if (trimmed.length() != KEY_CHARS) {
            return null;
        }
        byte[] out = new byte[KEY_BYTES];
        int buffer = 0;
        int bits = 0;
        int written = 0;
        for (int i = 0; i < trimmed.length(); i++) {
            int value = valueOf(trimmed.charAt(i));
            if (value < 0) {
                return null;
            }
            buffer = (buffer << 5) | value;
            bits += 5;
            if (bits >= 8) {
                bits -= 8;
                out[written++] = (byte) ((buffer >> bits) & 0xFF);
            }
        }
        if (written != KEY_BYTES) {
            return null;
        }
        // Crockford's ambiguity-free property: I and L are read as 1, and O as 0. The encoder never
        // emits them, so accepting them here only helps a key that was transcribed by hand.
        return out;
    }

    private static int valueOf(char c) {
        char upper = Character.toUpperCase(c);
        int direct = ALPHABET.indexOf(upper);
        if (direct >= 0) {
            return direct;
        }
        if (upper == 'I' || upper == 'L') {
            return 1;
        }
        if (upper == 'O') {
            return 0;
        }
        return -1;
    }

    /** Formats key bytes the way `remote-worker keygen` prints them. */
    public static String encode(byte[] key) {
        if (key == null || key.length != KEY_BYTES) {
            return "";
        }
        StringBuilder out = new StringBuilder(PREFIX);
        int buffer = 0;
        int bits = 0;
        for (byte b : key) {
            buffer = (buffer << 8) | (b & 0xFF);
            bits += 8;
            while (bits >= 5) {
                bits -= 5;
                out.append(ALPHABET.charAt((buffer >> bits) & 0x1F));
            }
        }
        if (bits > 0) {
            out.append(ALPHABET.charAt((buffer << (5 - bits)) & 0x1F));
        }
        return out.toString();
    }

    /**
     * The machine id derived from a key: 16 hex characters, stable for the life of the key.
     *
     * <p>Two 64-bit FNV-1a-style rounds run with different offsets, so the low half is not a trivial
     * function of the high half. This is deliberately not a cryptographic hash: the input is 160 bits
     * of CSPRNG output, over which the construction behaves like a random function, and the only
     * property actually relied on is that two different machines do not collide.
     *
     * <p><b>The offset basis below is not the textbook FNV one.</b> The C++ original uses
     * {@code 1469598103934665603}, which is {@code 0x14650FB0739D0383} -- a digit short of FNV-1a's
     * {@code 14695981039346656037}. That value is already on the wire, in {@code HELLO_ACK} and every
     * {@code OPEN_SESSION} of a released agent, so it is reproduced exactly here rather than
     * corrected. What matters is that the two implementations agree; the name "FNV" describes the
     * shape, not conformance to the published constants.
     */
    public static String machineId(byte[] key) {
        if (key == null || key.length != KEY_BYTES) {
            return "";
        }
        long high = 1469598103934665603L;
        long low = 1099511628211L;
        for (byte b : key) {
            high = (high ^ (b & 0xFF)) * 1099511628211L;
            // The & 0xFF is not cosmetic. The C++ casts to uint8_t *before* the xor, so b + 0x9E
            // wraps at 256; without it the high half still matches and only the low half is wrong,
            // which is exactly the kind of half-right that survives a casual comparison.
            low = (low ^ ((b + 0x9E) & 0xFF)) * 1099511628211L;
        }
        StringBuilder out = new StringBuilder(MACHINE_ID_CHARS);
        for (int i = 0; i < 8; i++) {
            out.append(Character.forDigit((int) ((high >> ((7 - i) * 4)) & 0xF), 16));
        }
        for (int i = 0; i < 8; i++) {
            out.append(Character.forDigit((int) ((low >> ((7 - i) * 4)) & 0xF), 16));
        }
        return out.toString();
    }
}
