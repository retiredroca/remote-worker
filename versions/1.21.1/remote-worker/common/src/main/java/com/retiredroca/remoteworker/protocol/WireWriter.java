package com.retiredroca.remoteworker.protocol;

/**
 * Growable little-endian byte sink.
 *
 * <p>Field-for-field the same shape as the {@code Writer} in {@code tools/protocol_vectors.py}.
 * Out-of-range values throw {@link IllegalArgumentException} rather than truncating: these are
 * programmer errors, caught at the call site that produced them, not wire errors.
 */
public final class WireWriter {
    private static final int INITIAL_CAPACITY = 64;

    private byte[] buf;
    private int len;

    public WireWriter() {
        this(INITIAL_CAPACITY);
    }

    public WireWriter(int capacity) {
        if (capacity < 0) {
            throw new IllegalArgumentException("capacity " + capacity);
        }
        this.buf = new byte[Math.max(capacity, INITIAL_CAPACITY)];
    }

    public int size() {
        return len;
    }

    public byte[] toByteArray() {
        byte[] out = new byte[len];
        System.arraycopy(buf, 0, out, 0, len);
        return out;
    }

    public WireWriter u8(int v) {
        require(v, 0, 0xFF, "u8");
        ensure(1);
        buf[len++] = (byte) v;
        return this;
    }

    public WireWriter u16(int v) {
        require(v, 0, 0xFFFF, "u16");
        ensure(2);
        buf[len++] = (byte) v;
        buf[len++] = (byte) (v >>> 8);
        return this;
    }

    public WireWriter u32(long v) {
        require(v, 0L, 0xFFFFFFFFL, "u32");
        ensure(4);
        buf[len++] = (byte) v;
        buf[len++] = (byte) (v >>> 8);
        buf[len++] = (byte) (v >>> 16);
        buf[len++] = (byte) (v >>> 24);
        return this;
    }

    /**
     * Writes a 64-bit little-endian pattern.
     *
     * <p>No range check, unlike every other method here, and that is deliberate. A u64 field has no
     * Java type: {@code long} is already all 64 bits, so every value it can hold is a valid bit
     * pattern and a range check could only ever reject a value the format allows. Interpret the
     * result with {@link Long#toUnsignedString(long)} or {@link Long#compareUnsigned} rather than
     * treating it as signed -- {@code pts_micros} at 0xFFFFFFFFFFFFFFFF reads back as -1 here.
     */
    public WireWriter u64(long bits) {
        ensure(8);
        for (int i = 0; i < 8; i++) {
            buf[len++] = (byte) (bits >>> (8 * i));
        }
        return this;
    }

    public WireWriter i16(int v) {
        if (v < Short.MIN_VALUE || v > Short.MAX_VALUE) {
            throw new IllegalArgumentException("i16 value " + v + " out of range");
        }
        ensure(2);
        buf[len++] = (byte) v;
        buf[len++] = (byte) (v >>> 8);
        return this;
    }

    public WireWriter i32(int v) {
        ensure(4);
        buf[len++] = (byte) v;
        buf[len++] = (byte) (v >>> 8);
        buf[len++] = (byte) (v >>> 16);
        buf[len++] = (byte) (v >>> 24);
        return this;
    }

    public WireWriter raw(byte[] b) {
        return bytes(b);
    }

    public WireWriter bytes(byte[] b) {
        ensure(b.length);
        System.arraycopy(b, 0, buf, len, b.length);
        len += b.length;
        return this;
    }

    /**
     * Length-prefixed UTF-8, length in bytes.
     *
     * <p>Not NUL-terminated: a terminator and a length would be two answers to "how long is this",
     * and they disagree whenever the payload contains a NUL.
     */
    public WireWriter string(String s) {
        byte[] b = s.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        if (b.length > Protocol.MAX_STRING) {
            throw new IllegalArgumentException("string of " + b.length + " bytes exceeds MAX_STRING "
                    + Protocol.MAX_STRING);
        }
        u16(b.length);
        return bytes(b);
    }

    /** u32 length followed by the bytes. */
    public WireWriter bytesField(byte[] b) {
        u32(b.length);
        return bytes(b);
    }

    private static void require(long v, long lo, long hi, String what) {
        if (v < lo || v > hi) {
            throw new IllegalArgumentException(what + " value " + v + " out of range [" + lo + ", " + hi + "]");
        }
    }

    private void ensure(int extra) {
        if (len + extra > buf.length) {
            int cap = buf.length;
            while (cap < len + extra) {
                cap *= 2;
            }
            // Never grow past what a conforming message may occupy: an unbounded buffer here would
            // reintroduce on the encode side the allocation attack MAX_MESSAGE exists to stop.
            int limit = Protocol.MAX_MESSAGE + Protocol.HEADER_SIZE;
            if (cap > limit) {
                cap = limit;
            }
            if (cap < len + extra) {
                throw new IllegalStateException("message exceeds MAX_MESSAGE at " + (len + extra) + " bytes");
            }
            byte[] next = new byte[cap];
            System.arraycopy(buf, 0, next, 0, len);
            buf = next;
        }
    }
}
