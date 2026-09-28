package com.retiredroca.remoteworker.protocol;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

/**
 * Bounds-checked little-endian reader over a fixed region of a byte array.
 *
 * <p>Every read checks the region first and throws {@link ProtocolException} rather than returning
 * a short value. That is the whole job of this class: the reject vectors exist to prove that no
 * field read can walk off the end of a message, because a peer chooses those length fields.
 */
public final class WireReader {
    private final byte[] buf;
    private int pos;
    private final int end;

    public WireReader(byte[] buf) {
        this(buf, 0, buf.length);
    }

    public WireReader(byte[] buf, int off, int length) {
        if (off < 0 || length < 0 || off + length > buf.length) {
            throw new IllegalArgumentException("region [" + off + "," + (off + length)
                    + ") outside buffer of " + buf.length);
        }
        this.buf = buf;
        this.pos = off;
        this.end = off + length;
    }

    public int remaining() {
        return end - pos;
    }

    public int u8() throws ProtocolException {
        require(1);
        return buf[pos++] & 0xFF;
    }

    public int u16() throws ProtocolException {
        require(2);
        int v = (buf[pos] & 0xFF) | ((buf[pos + 1] & 0xFF) << 8);
        pos += 2;
        return v;
    }

    public long u32() throws ProtocolException {
        require(4);
        long v = Protocol.readU32(buf, pos);
        pos += 4;
        return v;
    }

    /**
     * Reads a u64 as a Java {@code long}, which is a 64-bit pattern rather than a signed number.
     * Compare with {@link Long#compareUnsigned} and print with {@link Long#toUnsignedString}.
     */
    public long u64() throws ProtocolException {
        require(8);
        long v = 0L;
        for (int i = 0; i < 8; i++) {
            v |= (buf[pos + i] & 0xFFL) << (8 * i);
        }
        pos += 8;
        return v;
    }

    public int i16() throws ProtocolException {
        require(2);
        int v = (short) ((buf[pos] & 0xFF) | ((buf[pos + 1] & 0xFF) << 8));
        pos += 2;
        return v;
    }

    public int i32() throws ProtocolException {
        require(4);
        int v = (buf[pos] & 0xFF)
                | ((buf[pos + 1] & 0xFF) << 8)
                | ((buf[pos + 2] & 0xFF) << 16)
                | ((buf[pos + 3] & 0xFF) << 24);
        pos += 4;
        return v;
    }

    public byte[] bytes(int n) throws ProtocolException {
        if (n < 0) {
            throw new ProtocolException("negative length " + n);
        }
        require(n);
        byte[] out = new byte[n];
        System.arraycopy(buf, pos, out, 0, n);
        pos += n;
        return out;
    }

    /** A u32-length-prefixed byte run, bounded by {@link Protocol#MAX_MESSAGE}. */
    public byte[] bytesField() throws ProtocolException {
        long n = u32();
        if (n > remaining()) {
            throw new ProtocolException("byte run of " + n + " exceeds " + remaining() + " remaining");
        }
        return bytes((int) n);
    }

    /**
     * A u16-length-prefixed UTF-8 string, decoded strictly.
     *
     * <p>Strict, not lenient: a malformed sequence must be refused rather than replaced with
     * U+FFFD, because a lenient decode would re-encode to <em>different</em> bytes than it
     * received. The two peers would then disagree about the id with no error raised anywhere.
     */
    public String string() throws ProtocolException {
        int n = u16();
        if (n > Protocol.MAX_STRING) {
            throw new ProtocolException("string length " + n + " exceeds MAX_STRING "
                    + Protocol.MAX_STRING);
        }
        if (n > remaining()) {
            throw new ProtocolException("string of " + n + " bytes exceeds " + remaining() + " remaining");
        }
        byte[] raw = bytes(n);
        try {
            return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(raw))
                    .toString();
        } catch (CharacterCodingException e) {
            throw new ProtocolException("string is not valid UTF-8", e);
        }
    }

    /** Refuses trailing bytes. See {@link Protocol#parseMessage}. */
    public void requireExhausted() throws ProtocolException {
        if (remaining() != 0) {
            throw new ProtocolException(remaining() + " trailing byte(s) after message");
        }
    }

    private void require(int n) throws ProtocolException {
        if (n > remaining()) {
            throw new ProtocolException("read of " + n + " exceeds " + remaining() + " remaining");
        }
    }
}
