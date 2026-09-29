package com.retiredroca.remoteworker.protocol;

import java.util.Arrays;

/**
 * Every message in the wire format, and the dispatcher.
 *
 * <p>One file on purpose: this is the protocol's schema in one readable place, and the C++ agent
 * is written against it. Field order and width here are the same as the
 * {@code msg_*} builders in {@code tools/protocol_vectors.py}; the round-trip check compares
 * against those bytes, so the two cannot drift apart unnoticed.
 */
public final class Messages {
    private Messages() {
    }

    /** One message. {@link #flags()} is the header's flags byte, which is defined per type. */
    public interface Message {
        int type();

        int flags();

        void encode(WireWriter out);
    }

    public static Message decode(int type, int flags, WireReader r) throws ProtocolException {
        return switch (type) {
            case Protocol.TYPE_HELLO -> Hello.decode(r);
            case Protocol.TYPE_HELLO_ACK -> HelloAck.decode(r);
            case Protocol.TYPE_ERROR -> Error.decode(r);
            case Protocol.TYPE_OPEN_SESSION -> OpenSession.decode(r);
            case Protocol.TYPE_SESSION_OPENED -> SessionOpened.decode(r);
            case Protocol.TYPE_CLOSE_SESSION -> CloseSession.decode(r);
            case Protocol.TYPE_PING -> Ping.decode(r);
            case Protocol.TYPE_PONG -> Pong.decode(r);
            case Protocol.TYPE_BYTES -> BytePayload.decode(r);
            case Protocol.TYPE_CONFIG -> Config.decode(r);
            case Protocol.TYPE_FRAME -> Frame.decode(r, flags);
            case Protocol.TYPE_KEYFRAME_REQUEST -> KeyframeRequest.INSTANCE;
            case Protocol.TYPE_INPUT_BATCH -> InputBatch.decode(r);
            case Protocol.TYPE_AGENT_HELLO -> AgentHello.decode(r);
            case Protocol.TYPE_AGENT_HEARTBEAT -> AgentHeartbeat.decode(r);
            case Protocol.TYPE_AGENT_STATS -> AgentStats.decode(r);
            default -> throw new ProtocolException("unknown message type 0x" + Integer.toHexString(type));
        };
    }

    // --- session and connection -----------------------------------------------------------------

    /** First message on any connection. */
    public static final class Hello implements Message {
        public final int protocolMin;
        public final int protocolMax;
        public final int role;
        public final String instanceId;

        public Hello(int protocolMin, int protocolMax, int role, String instanceId) {
            this.protocolMin = protocolMin;
            this.protocolMax = protocolMax;
            this.role = role;
            this.instanceId = instanceId;
        }

        static Hello decode(WireReader r) throws ProtocolException {
            int min = r.u16();
            int max = r.u16();
            int role = r.u8();
            return new Hello(min, max, role, r.string());
        }

        @Override
        public int type() {
            return Protocol.TYPE_HELLO;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u16(protocolMin).u16(protocolMax).u8(role).string(instanceId);
        }

        @Override
        public String toString() {
            return "Hello{" + protocolMin + "-" + protocolMax + ", role=" + role
                    + ", id=" + instanceId + "}";
        }
    }

    public static final class HelloAck implements Message {
        public final int protocol;
        public final String instanceId;
        public final long maxMessage;
        public final int maxWidth;
        public final int maxHeight;

        public HelloAck(int protocol, String instanceId, long maxMessage, int maxWidth, int maxHeight) {
            this.protocol = protocol;
            this.instanceId = instanceId;
            this.maxMessage = maxMessage;
            this.maxWidth = maxWidth;
            this.maxHeight = maxHeight;
        }

        static HelloAck decode(WireReader r) throws ProtocolException {
            int protocol = r.u16();
            String id = r.string();
            long max = r.u32();
            return new HelloAck(protocol, id, max, r.u16(), r.u16());
        }

        @Override
        public int type() {
            return Protocol.TYPE_HELLO_ACK;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u16(protocol).string(instanceId).u32(maxMessage).u16(maxWidth).u16(maxHeight);
        }

        @Override
        public String toString() {
            return "HelloAck{v" + protocol + ", id=" + instanceId + ", max=" + maxMessage
                    + ", " + maxWidth + "x" + maxHeight + "}";
        }
    }

    public static final class Error implements Message {
        public final int code;
        public final String message;

        public Error(int code, String message) {
            this.code = code;
            this.message = message;
        }

        static Error decode(WireReader r) throws ProtocolException {
            int code = r.u16();
            return new Error(code, r.string());
        }

        @Override
        public int type() {
            return Protocol.TYPE_ERROR;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u16(code).string(message);
        }

        @Override
        public String toString() {
            return "Error{" + code + ", " + message + "}";
        }
    }

    public static final class OpenSession implements Message {
        /**
         * The machine being asked for, addressed by the id derived from its key. Not a display
         * name: regenerating a key changes this, which is correct, because after a rotation nothing
         * can prove the machine is the same one.
         */
        public final String machineId;
        public final int width;
        public final int height;
        public final int quality;
        /**
         * The token the agent minted for this controller. Only the agent reads it, to decide whether
         * this controller is acceptable; every other party treats it as opaque bytes.
         */
        public final byte[] credential;

        public OpenSession(String machineId, int width, int height, int quality) {
            this(machineId, width, height, quality, new byte[0]);
        }

        public OpenSession(String machineId, int width, int height, int quality, byte[] credential) {
            this.machineId = machineId;
            this.width = width;
            this.height = height;
            this.quality = quality;
            this.credential = credential;
        }

        static OpenSession decode(WireReader r) throws ProtocolException {
            String id = r.string();
            int w = r.u16();
            int h = r.u16();
            int quality = r.u8();
            return new OpenSession(id, w, h, quality, r.bytesField());
        }

        @Override
        public int type() {
            return Protocol.TYPE_OPEN_SESSION;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.string(machineId).u16(width).u16(height).u8(quality).bytesField(credential);
        }

        @Override
        public String toString() {
            // Deliberately does not print the credential: a protocol dump in a log or a bug report
            // is exactly where a token would otherwise end up.
            return "OpenSession{" + machineId + ", " + width + "x" + height + ", q=" + quality
                    + ", credential=" + credential.length + " bytes}";
        }
    }

    public static final class SessionOpened implements Message {
        public final String sessionId;
        public final int width;
        public final int height;
        public final int capture;

        public SessionOpened(String sessionId, int width, int height, int capture) {
            this.sessionId = sessionId;
            this.width = width;
            this.height = height;
            this.capture = capture;
        }

        static SessionOpened decode(WireReader r) throws ProtocolException {
            String id = r.string();
            int w = r.u16();
            int h = r.u16();
            return new SessionOpened(id, w, h, r.u8());
        }

        @Override
        public int type() {
            return Protocol.TYPE_SESSION_OPENED;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.string(sessionId).u16(width).u16(height).u8(capture);
        }

        @Override
        public String toString() {
            return "SessionOpened{" + sessionId + ", " + width + "x" + height
                    + ", capture=" + capture + "}";
        }
    }

    public static final class CloseSession implements Message {
        public final int reason;

        public CloseSession(int reason) {
            this.reason = reason;
        }

        static CloseSession decode(WireReader r) throws ProtocolException {
            return new CloseSession(r.u16());
        }

        @Override
        public int type() {
            return Protocol.TYPE_CLOSE_SESSION;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u16(reason);
        }

        @Override
        public String toString() {
            return "CloseSession{reason=" + reason + "}";
        }
    }

    public static final class Ping implements Message {
        public final long id;
        public final long micros;

        public Ping(long id, long micros) {
            this.id = id;
            this.micros = micros;
        }

        static Ping decode(WireReader r) throws ProtocolException {
            long id = r.u32();
            return new Ping(id, r.u64());
        }

        @Override
        public int type() {
            return Protocol.TYPE_PING;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u32(id).u64(micros);
        }

        @Override
        public String toString() {
            return "Ping{" + id + "}";
        }
    }

    public static final class Pong implements Message {
        public final long id;
        public final long micros;

        public Pong(long id, long micros) {
            this.id = id;
            this.micros = micros;
        }

        static Pong decode(WireReader r) throws ProtocolException {
            long id = r.u32();
            return new Pong(id, r.u64());
        }

        @Override
        public int type() {
            return Protocol.TYPE_PONG;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u32(id).u64(micros);
        }

        @Override
        public String toString() {
            return "Pong{" + id + "}";
        }
    }

    /**
     * An opaque run: carried, length-checked, not interpreted.
     *
     * <p>This carries the end-to-end session between controller and agent, so the receiver can route
     * and meter a connection it cannot decrypt. See PROTOCOL.md section 3.
     */
    public static final class BytePayload implements Message {
        public final byte[] payload;

        public BytePayload(byte[] payload) {
            this.payload = payload;
        }

        static BytePayload decode(WireReader r) throws ProtocolException {
            return new BytePayload(r.bytesField());
        }

        @Override
        public int type() {
            return Protocol.TYPE_BYTES;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.bytesField(payload);
        }

        @Override
        public String toString() {
            return "BytePayload{" + payload.length + " bytes}";
        }
    }

    // --- media -----------------------------------------------------------------------------------

    /** The encoder's current parameters, pushed when they change. */
    public static final class Config implements Message {
        public final int width;
        public final int height;
        public final int codec;
        public final int profile;
        public final int bitrateKbps;
        public final int fps;
        public final int keyframeInterval;
        public final int pixFmt;

        public Config(int width, int height, int codec, int profile, int bitrateKbps, int fps,
                      int keyframeInterval, int pixFmt) {
            this.width = width;
            this.height = height;
            this.codec = codec;
            this.profile = profile;
            this.bitrateKbps = bitrateKbps;
            this.fps = fps;
            this.keyframeInterval = keyframeInterval;
            this.pixFmt = pixFmt;
        }

        static Config decode(WireReader r) throws ProtocolException {
            int width = r.u16();
            int height = r.u16();
            int codec = r.u8();
            int profile = r.u8();
            int bitrate = r.u16();
            int fps = r.u8();
            int interval = r.u16();
            return new Config(width, height, codec, profile, bitrate, fps, interval, r.u8());
        }

        @Override
        public int type() {
            return Protocol.TYPE_CONFIG;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u16(width).u16(height).u8(codec).u8(profile).u16(bitrateKbps).u8(fps)
                    .u16(keyframeInterval).u8(pixFmt);
        }

        @Override
        public String toString() {
            return "Config{" + width + "x" + height + ", codec=" + codec + ", " + bitrateKbps
                    + " kbps, " + fps + " fps, gop=" + keyframeInterval + "}";
        }
    }

    /** One coded picture plus the damage list the controller uses for a partial texture upload. */
    public static final class Frame implements Message {
        /** A damage rectangle in surface coordinates. i16 on the wire. */
        public static final class Rect {
            public final int x;
            public final int y;
            public final int w;
            public final int h;

            public Rect(int x, int y, int w, int h) {
                this.x = x;
                this.y = y;
                this.w = w;
                this.h = h;
            }

            @Override
            public String toString() {
                return "(" + x + "," + y + " " + w + "x" + h + ")";
            }
        }

        public final long frameIndex;
        /** Unsigned 64-bit; see {@link WireReader#u64()}. A u32 would wrap after ~71 minutes. */
        public final long ptsMicros;
        public final int width;
        public final int height;
        /**
         * The header's flags byte, kept verbatim. Bits other than
         * {@link Protocol#FRAME_FLAG_KEYFRAME} are undefined today and are preserved but ignored:
         * dropping them would make a frame not round-trip, and rejecting them would turn every new
         * flag into a breaking change, which is what the header's {@code reserved} field is for.
         */
        public final int flags;
        public final Rect[] rects;
        public final byte[] data;

        public Frame(long frameIndex, long ptsMicros, int width, int height, int flags,
                     Rect[] rects, byte[] data) {
            this.frameIndex = frameIndex;
            this.ptsMicros = ptsMicros;
            this.width = width;
            this.height = height;
            this.flags = flags;
            this.rects = rects;
            this.data = data;
        }

        public Frame(long frameIndex, long ptsMicros, int width, int height, boolean keyframe,
                     Rect[] rects, byte[] data) {
            this(frameIndex, ptsMicros, width, height,
                    keyframe ? Protocol.FRAME_FLAG_KEYFRAME : 0, rects, data);
        }

        public boolean isKeyframe() {
            return (flags & Protocol.FRAME_FLAG_KEYFRAME) != 0;
        }

        static Frame decode(WireReader r, int flags) throws ProtocolException {
            long index = r.u32();
            long pts = r.u64();
            int width = r.u16();
            int height = r.u16();
            int count = r.u16();
            if (count == 0) {
                // A frame always damages something, at minimum the whole surface. Accepting 0 would
                // leave the controller with nothing to upload.
                throw new ProtocolException("FRAME rect_count must be at least 1");
            }
            // Check before allocating, not after: a 26-byte message must not be able to make a
            // 65535-element array appear.
            if ((long) count * 8L > r.remaining()) {
                throw new ProtocolException("FRAME rect_count " + count + " needs " + ((long) count * 8L)
                        + " bytes, " + r.remaining() + " remain");
            }
            Rect[] rects = new Rect[count];
            for (int i = 0; i < count; i++) {
                rects[i] = new Rect(r.i16(), r.i16(), r.i16(), r.i16());
            }
            byte[] data = r.bytesField();
            if (data.length == 0) {
                throw new ProtocolException("FRAME picture must not be empty");
            }
            return new Frame(index, pts, width, height, flags, rects, data);
        }

        @Override
        public int type() {
            return Protocol.TYPE_FRAME;
        }

        @Override
        public int flags() {
            return flags;
        }

        @Override
        public void encode(WireWriter out) {
            out.u32(frameIndex).u64(ptsMicros).u16(width).u16(height).u16(rects.length);
            for (Rect rect : rects) {
                out.i16(rect.x).i16(rect.y).i16(rect.w).i16(rect.h);
            }
            out.bytesField(data);
        }

        @Override
        public String toString() {
            return "Frame{#" + frameIndex + ", " + width + "x" + height + ", " + rects.length
                    + " rects, " + data.length + " bytes, flags=0x"
                    + Integer.toHexString(flags) + (isKeyframe() ? " key" : " delta") + "}";
        }
    }

    public static final class KeyframeRequest implements Message {
        public static final KeyframeRequest INSTANCE = new KeyframeRequest();

        private KeyframeRequest() {
        }

        @Override
        public int type() {
            return Protocol.TYPE_KEYFRAME_REQUEST;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
        }

        @Override
        public String toString() {
            return "KeyframeRequest{}";
        }
    }

    // --- input -----------------------------------------------------------------------------------

    /**
     * A run of input events.
     *
     * <p>Batched because a mouse at its polling rate would otherwise cost one 8-byte header per
     * event: the header is sized for the frame payload, not for a 12-byte record.
     */
    public static final class InputBatch implements Message {
        /** One event. Fixed 12 bytes so the array can be read with an index and no parsing. */
        public static final class Record {
            public final int kind;
            public final int flags;
            public final int x;
            public final int y;
            public final long data;

            public Record(int kind, int flags, int x, int y, long data) {
                this.kind = kind;
                this.flags = flags;
                this.x = x;
                this.y = y;
                this.data = data;
            }

            @Override
            public String toString() {
                return "kind=" + kind + " @" + x + "," + y + " data=" + data;
            }
        }

        public final Record[] records;

        public InputBatch(Record[] records) {
            this.records = records;
        }

        /** Number of records. */
        public int count() {
            return records.length;
        }

        static InputBatch decode(WireReader r) throws ProtocolException {
            int count = r.u16();
            if (count == 0) {
                // An empty batch is a no-op that still costs a round trip.
                throw new ProtocolException("INPUT_BATCH count must be at least 1");
            }
            if ((long) count * Protocol.INPUT_RECORD_SIZE > r.remaining()) {
                throw new ProtocolException("INPUT_BATCH count " + count + " needs "
                        + ((long) count * Protocol.INPUT_RECORD_SIZE) + " bytes, " + r.remaining()
                        + " remain");
            }
            Record[] records = new Record[count];
            for (int i = 0; i < count; i++) {
                int kind = r.u8();
                int flags = r.u8();
                int reserved = r.u16();
                if (reserved != 0) {
                    throw new ProtocolException("input record reserved must be 0, got " + reserved);
                }
                int x = r.i32();
                int y = r.i32();
                records[i] = new Record(kind, flags, x, y, r.u32());
            }
            return new InputBatch(records);
        }

        @Override
        public int type() {
            return Protocol.TYPE_INPUT_BATCH;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u16(records.length);
            for (Record record : records) {
                out.u8(record.kind).u8(record.flags).u16(0)
                        .i32(record.x).i32(record.y).u32(record.data);
            }
        }

        @Override
        public String toString() {
            return "InputBatch{" + records.length + " records " + Arrays.toString(records) + "}";
        }
    }

    // --- agent control and telemetry ---------------------------------------------------------------

    public static final class AgentHello implements Message {
        /**
         * Derived from the agent's key, and what peers address this machine by.
         */
        public final String machineId;
        /**
         * Free text for the user interface. May repeat between endpoints, and nothing looks a machine
         * up by it: an id is derived from a key, so two machines are both called
         * "DESKTOP-EXAMPLE" without colliding.
         */
        public final String label;
        public final int os;
        public final int capabilities;
        public final int maxWidth;
        public final int maxHeight;
        public final int encoders;

        public AgentHello(String machineId, String label, int os, int capabilities, int maxWidth,
                          int maxHeight, int encoders) {
            this.machineId = machineId;
            this.label = label;
            this.os = os;
            this.capabilities = capabilities;
            this.maxWidth = maxWidth;
            this.maxHeight = maxHeight;
            this.encoders = encoders;
        }

        static AgentHello decode(WireReader r) throws ProtocolException {
            String machineId = r.string();
            String label = r.string();
            int os = r.u8();
            int caps = r.u16();
            int w = r.u16();
            int h = r.u16();
            return new AgentHello(machineId, label, os, caps, w, h, r.u8());
        }

        @Override
        public int type() {
            return Protocol.TYPE_AGENT_HELLO;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.string(machineId).string(label).u8(os).u16(capabilities).u16(maxWidth)
                    .u16(maxHeight).u8(encoders);
        }

        @Override
        public String toString() {
            return "AgentHello{" + machineId + " (" + label + "), os=" + os + ", caps=0x" + Integer.toHexString(capabilities)
                    + ", encoders=0x" + Integer.toHexString(encoders) + "}";
        }
    }

    public static final class AgentHeartbeat implements Message {
        public final long uptimeSeconds;
        public final int activeSessions;

        public AgentHeartbeat(long uptimeSeconds, int activeSessions) {
            this.uptimeSeconds = uptimeSeconds;
            this.activeSessions = activeSessions;
        }

        static AgentHeartbeat decode(WireReader r) throws ProtocolException {
            long uptime = r.u32();
            return new AgentHeartbeat(uptime, r.u16());
        }

        @Override
        public int type() {
            return Protocol.TYPE_AGENT_HEARTBEAT;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u32(uptimeSeconds).u16(activeSessions);
        }

        @Override
        public String toString() {
            return "AgentHeartbeat{up " + uptimeSeconds + "s, " + activeSessions + " sessions}";
        }
    }

    public static final class AgentStats implements Message {
        public final long framesSent;
        public final long framesDropped;
        /** Unsigned 64-bit; see {@link WireReader#u64()}. */
        public final long bytesSent;
        public final int encodeMsAvg;
        public final int bitrateKbps;
        public final int fps;
        public final int inputQueueDepth;

        public AgentStats(long framesSent, long framesDropped, long bytesSent, int encodeMsAvg,
                          int bitrateKbps, int fps, int inputQueueDepth) {
            this.framesSent = framesSent;
            this.framesDropped = framesDropped;
            this.bytesSent = bytesSent;
            this.encodeMsAvg = encodeMsAvg;
            this.bitrateKbps = bitrateKbps;
            this.fps = fps;
            this.inputQueueDepth = inputQueueDepth;
        }

        static AgentStats decode(WireReader r) throws ProtocolException {
            long sent = r.u32();
            long dropped = r.u32();
            // u64: a u32 byte counter wraps at 4.29 GB, about eleven minutes of video. A counter
            // that wraps is worse than none, because it still reads as a plausible number.
            long bytes = r.u64();
            int encodeMs = r.u16();
            int bitrate = r.u16();
            return new AgentStats(sent, dropped, bytes, encodeMs, bitrate, r.u8(), r.u8());
        }

        @Override
        public int type() {
            return Protocol.TYPE_AGENT_STATS;
        }

        @Override
        public int flags() {
            return 0;
        }

        @Override
        public void encode(WireWriter out) {
            out.u32(framesSent).u32(framesDropped).u64(bytesSent).u16(encodeMsAvg)
                    .u16(bitrateKbps).u8(fps).u8(inputQueueDepth);
        }

        @Override
        public String toString() {
            return "AgentStats{" + framesSent + " frames, " + framesDropped + " dropped, "
                    + bytesSent + " bytes, " + fps + " fps, " + bitrateKbps + " kbps, encode "
                    + encodeMsAvg + "ms, q=" + inputQueueDepth + "}";
        }
    }
}
