package com.retiredroca.remoteworker.protocol;

/**
 * Wire-format constants and header framing.
 *
 * <p>The layout and every field width are defined by {@code tools/protocol_vectors.py}, which is
 * the normative encoder; {@code PROTOCOL.md} is the prose. This class is the Java side of that
 * definition and is checked against it by {@code ProtocolVectorsCheck} on every build, so a
 * disagreement here fails the build rather than turning into a stream of subtly wrong bytes.
 *
 * <p>All integers are little-endian. Every platform this ships on is little-endian (x86-64,
 * aarch64 on Windows and Linux, arm64 macOS), so there is no byte-order negotiation and no
 * per-field cost for being explicit about it.
 */
public final class Protocol {
    private Protocol() {
    }

    public static final int VERSION_MIN = 1;
    public static final int VERSION_MAX = 1;

    /**
     * Largest payload a peer may announce.
     *
     * <p>This is the bound that makes a length field safe: the decoder refuses anything larger
     * before allocating, so a single 4-byte field on the wire cannot ask for gigabytes of heap.
     */
    public static final int MAX_MESSAGE = 16 * 1024 * 1024;

    /** Longest string, in UTF-8 bytes. The protocol carries ids and diagnostics, not file data. */
    public static final int MAX_STRING = 1024;

    /** type(1) + flags(1) + reserved(2) + length(4). */
    public static final int HEADER_SIZE = 8;

    // --- type registry -------------------------------------------------------------------------
    // Grouped into ranges by category so a message can be added later without renumbering siblings.
    //   0x00-0x0F  session and connection
    //   0x10-0x1F  media      (agent -> controller, relayed)
    //   0x20-0x2F  input      (controller -> agent, relayed)
    //   0x30-0x3F  agent control and telemetry
    public static final int TYPE_HELLO = 0x01;
    public static final int TYPE_HELLO_ACK = 0x02;
    public static final int TYPE_ERROR = 0x03;
    public static final int TYPE_OPEN_SESSION = 0x04;
    public static final int TYPE_SESSION_OPENED = 0x05;
    public static final int TYPE_CLOSE_SESSION = 0x06;
    public static final int TYPE_PING = 0x07;
    public static final int TYPE_PONG = 0x08;
    public static final int TYPE_BYTES = 0x0E;
    public static final int TYPE_CONFIG = 0x10;
    public static final int TYPE_FRAME = 0x11;
    public static final int TYPE_KEYFRAME_REQUEST = 0x12;
    public static final int TYPE_INPUT_BATCH = 0x20;
    public static final int TYPE_AGENT_HELLO = 0x30;
    public static final int TYPE_AGENT_HEARTBEAT = 0x31;
    public static final int TYPE_AGENT_STATS = 0x32;

    // --- enumerations --------------------------------------------------------------------------

    public static final int ROLE_CONTROLLER = 1;
    public static final int ROLE_AGENT = 2;

    public static final int ERROR_UNSPECIFIED = 0;
    public static final int ERROR_UNSUPPORTED_VERSION = 1;
    public static final int ERROR_AUTH_FAILED = 2;
    public static final int ERROR_NOT_FOUND = 3;
    public static final int ERROR_BAD_MESSAGE = 4;
    public static final int ERROR_LIMIT_EXCEEDED = 5;
    public static final int ERROR_AGENT_BUSY = 6;
    public static final int ERROR_UNSUPPORTED_CAPTURE = 7;

    public static final int CLOSE_CLIENT = 0;
    public static final int CLOSE_AGENT = 1;
    public static final int CLOSE_ERROR = 2;
    public static final int CLOSE_REPLACED = 3;

    public static final int CAPTURE_UNKNOWN = 0;
    public static final int CAPTURE_INTERACTIVE = 1;
    public static final int CAPTURE_LOCKED = 2;

    public static final int QUALITY_LOW = 0;
    public static final int QUALITY_MEDIUM = 1;
    public static final int QUALITY_HIGH = 2;
    public static final int QUALITY_LOSSLESS = 3;

    public static final int CODEC_H264 = 0;
    public static final int CODEC_H265 = 1;
    public static final int CODEC_AV1 = 2;
    public static final int CODEC_RAW = 3;

    public static final int PROFILE_BASELINE = 0;
    public static final int PROFILE_MAIN = 1;
    public static final int PROFILE_HIGH = 2;

    public static final int PIX_FMT_YUV420 = 0;
    public static final int PIX_FMT_YUV444 = 1;

    /** FRAME header flag: the coded picture is an IDR and needs no prior reference. */
    public static final int FRAME_FLAG_KEYFRAME = 1 << 0;

    public static final int INPUT_MOVE = 1;
    public static final int INPUT_BUTTON_DOWN = 2;
    public static final int INPUT_BUTTON_UP = 3;
    public static final int INPUT_WHEEL = 4;
    public static final int INPUT_KEY_DOWN = 5;
    public static final int INPUT_KEY_UP = 6;
    public static final int INPUT_TEXT = 7;

    public static final int INPUT_MOD_SHIFT = 1 << 0;
    public static final int INPUT_MOD_CTRL = 1 << 1;
    public static final int INPUT_MOD_ALT = 1 << 2;
    public static final int INPUT_MOD_META = 1 << 3;
    public static final int INPUT_MOD_CAPS = 1 << 4;
    public static final int INPUT_MOD_REPEAT = 1 << 5;

    /** Bytes per INPUT_BATCH record: kind, flags, reserved(2), x, y, data. */
    public static final int INPUT_RECORD_SIZE = 12;

    public static final int OS_WINDOWS = 1;
    public static final int OS_LINUX = 2;
    public static final int OS_MACOS = 3;

    public static final int CAP_ATTRIB_INTERACTIVE = 1 << 0;
    public static final int CAP_ATTRIB_LOCKED = 1 << 1;
    public static final int CAP_ATTRIB_MULTI_MONITOR = 1 << 2;
    public static final int CAP_ATTRIB_CONSENT_GATED = 1 << 3;

    public static final int ENC_H264_HW = 1 << 0;
    public static final int ENC_H264_SW = 1 << 1;
    public static final int ENC_AV1_HW = 1 << 2;

    /** The 8-byte header in front of every message. */
    public static final class Header {
        public final int type;
        public final int flags;
        public final int length;

        Header(int type, int flags, int length) {
            this.type = type;
            this.flags = flags;
            this.length = length;
        }
    }

    /** A decoded message together with the header it arrived in. */
    public static final class Decoded {
        public final Header header;
        public final Messages.Message message;

        Decoded(Header header, Messages.Message message) {
            this.header = header;
            this.message = message;
        }
    }

    /**
     * Reads a header from the front of a buffer.
     *
     * @param avail bytes readable from {@code off}; the declared length is checked against this, not
     *              against the buffer's own size, so a stream layer can call this before it has
     *              received the whole message.
     * @throws ProtocolException if fewer than {@link #HEADER_SIZE} bytes are available, if
     *                           {@code reserved} is non-zero, or if the declared length exceeds
     *                           either {@link #MAX_MESSAGE} or {@code avail}.
     */
    public static Header parseHeader(byte[] buf, int off, int avail) throws ProtocolException {
        if (avail < HEADER_SIZE) {
            throw new ProtocolException("header needs " + HEADER_SIZE + " bytes, have " + avail);
        }
        int type = buf[off] & 0xFF;
        int flags = buf[off + 1] & 0xFF;
        int reserved = (buf[off + 2] & 0xFF) | ((buf[off + 3] & 0xFF) << 8);
        if (reserved != 0) {
            // A non-zero reserved field means the peer is speaking a dialect we do not know. Failing
            // here is the entire point of the field: guessing would misparse the payload silently.
            throw new ProtocolException("reserved must be 0, got " + reserved);
        }
        long length = readU32(buf, off + 4);
        if (length > MAX_MESSAGE) {
            throw new ProtocolException("length " + length + " exceeds MAX_MESSAGE " + MAX_MESSAGE);
        }
        if (length > avail - HEADER_SIZE) {
            throw new ProtocolException("length " + length + " exceeds " + (avail - HEADER_SIZE)
                    + " available bytes");
        }
        return new Header(type, flags, (int) length);
    }

    /**
     * Decodes exactly one message occupying {@code buf[off, off+len)} in full.
     *
     * <p>The region must be consumed completely: a payload whose declared length leaves bytes over
     * is refused. In a stream that surplus is the next message, so the framing layer sizes the
     * region from the header before calling this; the requirement exists so that this method can
     * be tested against a literal byte string, and so a caller can never quietly ignore the tail.
     */
    public static Decoded parseMessage(byte[] buf, int off, int len) throws ProtocolException {
        Header header = parseHeader(buf, off, len);
        // The region must be exactly one message, not merely start with one. Without this, a
        // buffer holding a valid message followed by anything else decodes the first message and
        // silently ignores the rest -- so a caller that validated a received region this way would
        // accept a message whose declared length did not account for what it was handed.
        if (header.length != len - HEADER_SIZE) {
            throw new ProtocolException("message occupies " + (Protocol.HEADER_SIZE + header.length)
                    + " of " + len + " bytes");
        }
        WireReader r = new WireReader(buf, off + HEADER_SIZE, header.length);
        Messages.Message message = Messages.decode(header.type, header.flags, r);
        r.requireExhausted();
        return new Decoded(header, message);
    }

    /** Encodes one message, header and all. */
    public static byte[] encodeMessage(Messages.Message message) {
        WireWriter payload = new WireWriter();
        message.encode(payload);
        WireWriter out = new WireWriter(HEADER_SIZE + payload.size());
        out.u8(message.type());
        out.u8(message.flags());
        out.u16(0);
        out.u32(payload.size());
        out.bytes(payload.toByteArray());
        return out.toByteArray();
    }

    static long readU32(byte[] buf, int off) {
        return (buf[off] & 0xFFL)
                | ((buf[off + 1] & 0xFFL) << 8)
                | ((buf[off + 2] & 0xFFL) << 16)
                | ((buf[off + 3] & 0xFFL) << 24);
    }
}
