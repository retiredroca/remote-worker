#!/usr/bin/env python3
"""Wire-format vectors for the remote-worker control protocol.

This script is the **normative encoder** for the protocol described in `PROTOCOL.md`.
Nothing else is allowed to be the definition: the Java codec under
`versions/<mc>/<module>/common/` and the C++ agent/relay both have to agree with the bytes
produced here, and `ProtocolVectorsCheck` fails the build when they do not. That is why the
format is hand-rolled and the vectors are generated rather than coming from a Protobuf schema:
with one authoritative encoder, "the two implementations disagree" becomes a test failure
instead of a field-by-field reading exercise.

Each line of the output is one complete message as it appears on the wire:

    ROUNDTRIP <name> <hex>    a well-formed message; decode, re-encode, expect the same bytes
    REJECT    <name> <hex>    malformed input; a conforming decoder MUST refuse it

Usage:
    python tools/protocol_vectors.py [outfile]

Defaults to build/protocol/vectors.txt. The output is generated, not committed: the generator is
the single source of truth, so a committed copy could only ever disagree with it.
"""

import struct
import sys
from pathlib import Path

PROTOCOL_VERSION_MIN = 1
PROTOCOL_VERSION_MAX = 1

# A peer must never announce a payload larger than this. It bounds the allocation a single length
# field can provoke, which is the point: an unbounded length is a remote out-of-memory.
MAX_MESSAGE = 16 * 1024 * 1024

HEADER_SIZE = 8

# --- type registry ---------------------------------------------------------------------------------
# Grouped into ranges by category, so a message can be added later without renumbering its siblings.
#   0x00-0x0F  session and connection
#   0x10-0x1F  media      (agent -> controller, relayed)
#   0x20-0x2F  input      (controller -> agent, relayed)
#   0x30-0x3F  agent control and telemetry
HELLO = 0x01
HELLO_ACK = 0x02
ERROR = 0x03
OPEN_SESSION = 0x04
SESSION_OPENED = 0x05
CLOSE_SESSION = 0x06
PING = 0x07
PONG = 0x08
BYTES = 0x0E
CONFIG = 0x10
FRAME = 0x11
KEYFRAME_REQUEST = 0x12
INPUT_BATCH = 0x20
AGENT_HELLO = 0x30
AGENT_HEARTBEAT = 0x31
AGENT_STATS = 0x32

# --- enumerations ----------------------------------------------------------------------------------
ROLE_CONTROLLER = 1
ROLE_AGENT = 2

ERROR_UNSPECIFIED = 0
ERROR_UNSUPPORTED_VERSION = 1
ERROR_AUTH_FAILED = 2
ERROR_NOT_FOUND = 3
ERROR_BAD_MESSAGE = 4
ERROR_LIMIT_EXCEEDED = 5
ERROR_AGENT_BUSY = 6
ERROR_UNSUPPORTED_CAPTURE = 7
ERROR_DUPLICATE_ENDPOINT = 8

CLOSE_CLIENT = 0
CLOSE_AGENT = 1
CLOSE_ERROR = 2
CLOSE_REPLACED = 3

CAPTURE_UNKNOWN = 0
CAPTURE_INTERACTIVE = 1
CAPTURE_LOCKED = 2

QUALITY_LOW = 0
QUALITY_MEDIUM = 1
QUALITY_HIGH = 2
QUALITY_LOSSLESS = 3

CODEC_H264 = 0
CODEC_H265 = 1
CODEC_AV1 = 2
CODEC_RAW = 3

PROFILE_BASELINE = 0
PROFILE_MAIN = 1
PROFILE_HIGH = 2

PIX_FMT_YUV420 = 0
PIX_FMT_YUV444 = 1

FRAME_FLAG_KEYFRAME = 1 << 0

INPUT_MOVE = 1
INPUT_BUTTON_DOWN = 2
INPUT_BUTTON_UP = 3
INPUT_WHEEL = 4
INPUT_KEY_DOWN = 5
INPUT_KEY_UP = 6
INPUT_TEXT = 7

INPUT_MOD_SHIFT = 1 << 0
INPUT_MOD_CTRL = 1 << 1
INPUT_MOD_ALT = 1 << 2
INPUT_MOD_META = 1 << 3
INPUT_MOD_CAPS = 1 << 4
INPUT_MOD_REPEAT = 1 << 5

INPUT_RECORD_SIZE = 12

# A string is length-prefixed and bounded well below 64 KiB, so u16 carries it. Deliberately small:
# this protocol carries ids and short diagnostics, not file contents.
MAX_STRING = 1024

OS_WINDOWS = 1
OS_LINUX = 2
OS_MACOS = 3

CAP_ATTRIB_INTERACTIVE = 1 << 0
CAP_ATTRIB_LOCKED = 1 << 1
CAP_ATTRIB_MULTI_MONITOR = 1 << 2
CAP_ATTRIB_CONSENT_GATED = 1 << 3

ENC_H264_HW = 1 << 0
ENC_H264_SW = 1 << 1
ENC_AV1_HW = 1 << 2


def check_int(v, lo, hi, what):
    if not isinstance(v, int) or isinstance(v, bool):
        raise TypeError("%s value must be an int, got %r" % (what, v))
    if not lo <= v <= hi:
        raise ValueError("%s value %d out of range [%d, %d]" % (what, v, lo, hi))


# The id an agent is known by, derived from its key (see machine_id in rw/credential.cpp). Used as
# the default so the vectors read like a real endpoint, and so a rename shows up in every vector.
MACHINE_ID = "0123456789abcdef"


class Writer:
    """Growable little-endian byte sink. Mirrors the Java WireWriter field for field."""

    def __init__(self):
        self.buf = bytearray()

    def u8(self, v):
        check_int(v, 0, 0xFF, "u8")
        self.buf.append(v)

    def u16(self, v):
        check_int(v, 0, 0xFFFF, "u16")
        self.buf += v.to_bytes(2, "little")

    def u32(self, v):
        check_int(v, 0, 0xFFFFFFFF, "u32")
        self.buf += v.to_bytes(4, "little")

    def i32(self, v):
        check_int(v, -0x80000000, 0x7FFFFFFF, "i32")
        self.buf += (v & 0xFFFFFFFF).to_bytes(4, "little")

    def u64(self, v):
        check_int(v, 0, 0xFFFFFFFFFFFFFFFF, "u64")
        self.buf += v.to_bytes(8, "little")

    def raw(self, b):
        self.buf += b

    def string(self, s):
        """Length-prefixed UTF-8, length in BYTES.

        Not NUL-terminated: a terminator and a length would be two answers to 'how long is this',
        and they disagree whenever the payload contains a NUL. Length in bytes, not characters, so a
        multi-byte id is measured the way it is stored.
        """
        b = s.encode("utf-8")
        if len(b) > MAX_STRING:
            raise ValueError("string of %d bytes exceeds MAX_STRING (%d)" % (len(b), MAX_STRING))
        self.u16(len(b))
        self.buf += b

    def string_bytes(self, raw, length=None):
        """Write a length-prefixed byte run without validating it as UTF-8.

        Only the reject vectors need this: they have to be able to put malformed bytes where a
        string is declared, which is exactly what a conforming encoder must never produce.
        """
        self.u16(len(raw) if length is None else length)
        self.buf += raw

    def bytes_field(self, b):
        self.u32(len(b))
        self.buf += b

    def rect(self, x, y, w, h):
        # i16 rather than i32: a damage rect is a screen coordinate, never wider than a display,
        # and a frame's damage list can hold thousands of these. See PROTOCOL.md.
        for v in (x, y, w, h):
            check_int(v, -0x8000, 0x7FFF, "rect component")
        for v in (x, y, w, h):
            self.buf += (v & 0xFFFF).to_bytes(2, "little")

    def __len__(self):
        return len(self.buf)


def frame(msg_type, payload, flags=0, reserved=0, length=None):
    """Assemble a complete wire message: 8-byte header then payload.

    `reserved` and `length` are overridable so the reject vectors can build messages a conforming
    encoder would never emit.
    """
    if length is None:
        length = len(payload)
    check_int(length, 0, 0xFFFFFFFF, "length")
    w = Writer()
    w.u8(msg_type)
    w.u8(flags)
    w.u16(reserved)
    w.u32(length)
    w.raw(payload)
    return bytes(w.buf)


def _frame_preamble(frame_index=0, pts=0, width=0, height=0):
    """The part of a FRAME payload that precedes the damage list, so a caller can supply a
    deliberately invalid count."""
    p = Writer()
    p.u32(frame_index)
    p.u64(pts)
    p.u16(width)
    p.u16(height)
    return p


# --- message builders -----------------------------------------------------------------------------
# One per message in the registry. Each returns the complete message bytes.


def msg_hello(instance_id="controller-01", role=ROLE_CONTROLLER):
    p = Writer()
    p.u16(PROTOCOL_VERSION_MIN)
    p.u16(PROTOCOL_VERSION_MAX)
    p.u8(role)
    p.string(instance_id)
    return frame(HELLO, bytes(p.buf))


def msg_hello_ack(instance_id="controller-01", protocol=PROTOCOL_VERSION_MAX,
                  max_message=MAX_MESSAGE, max_w=3840, max_h=2160):
    p = Writer()
    p.u16(protocol)
    p.string(instance_id)
    p.u32(max_message)
    p.u16(max_w)
    p.u16(max_h)
    return frame(HELLO_ACK, bytes(p.buf))


def msg_error(code=ERROR_BAD_MESSAGE, message="rect_count 0 is not encodable"):
    p = Writer()
    p.u16(code)
    p.string(message)
    return frame(ERROR, bytes(p.buf))


def msg_open_session(machine_id=MACHINE_ID, width=1920, height=1080, quality=QUALITY_HIGH,
                     credential=b""):
    p = Writer()
    p.string(machine_id)
    p.u16(width)
    p.u16(height)
    p.u8(quality)
    # The token the agent minted for this controller. Opaque here: the relay forwards it without
    # reading it and the agent is the only thing that checks it, so the relay holds no keys.
    p.bytes_field(credential)
    return frame(OPEN_SESSION, bytes(p.buf))


def msg_session_opened(session_id="s-0000-0001", width=1920, height=1080,
                       capture=CAPTURE_INTERACTIVE):
    p = Writer()
    p.string(session_id)
    p.u16(width)
    p.u16(height)
    p.u8(capture)
    return frame(SESSION_OPENED, bytes(p.buf))


def msg_close_session(reason=CLOSE_CLIENT):
    p = Writer()
    p.u16(reason)
    return frame(CLOSE_SESSION, bytes(p.buf))


def msg_ping(ping_id=0xDEADBEEF, micros=123456789):
    p = Writer()
    p.u32(ping_id)
    p.u64(micros)
    return frame(PING, bytes(p.buf))


def msg_pong(ping_id=0xDEADBEEF, micros=987654321):
    p = Writer()
    p.u32(ping_id)
    p.u64(micros)
    return frame(PONG, bytes(p.buf))


def msg_bytes(payload=b"\x16\x03\x01\x00\x2a"):
    p = Writer()
    p.bytes_field(payload)
    return frame(BYTES, bytes(p.buf))


def msg_config(width=1920, height=1080, codec=CODEC_H264, profile=PROFILE_HIGH,
               bitrate_kbps=3500, fps=30, keyframe_interval=120, pix_fmt=PIX_FMT_YUV420):
    p = Writer()
    p.u16(width)
    p.u16(height)
    p.u8(codec)
    p.u8(profile)
    # u16, not u8: a u8 bitrate caps at 255 kbps, which is below what a full-screen desktop
    # needs even at a low quality target, so the field would have been unable to express the
    # values it exists for. 65 Mbps is comfortably above any LAN rate.
    p.u16(bitrate_kbps)
    p.u8(fps)
    p.u16(keyframe_interval)
    p.u8(pix_fmt)
    return frame(CONFIG, bytes(p.buf))


def msg_frame(frame_index=4096, pts=8_000_000, width=1920, height=1080,
              keyframe=True, rects=((0, 0, 1920, 1080),),
              data=b"\x00\x00\x00\x01\x67\x42\x00\x1e"):
    if not rects:
        raise ValueError("a frame damages at least one rect; see PROTOCOL.md 5.2")
    if not data:
        raise ValueError("a frame carries a non-empty coded picture")
    p = _frame_preamble(frame_index, pts, width, height)
    p.u16(len(rects))
    for r in rects:
        p.rect(*r)
    p.bytes_field(data)
    # The keyframe bit lives in the message header's flags byte, which is defined per message
    # type, rather than in a second flags field inside the payload.
    return frame(FRAME, bytes(p.buf), flags=FRAME_FLAG_KEYFRAME if keyframe else 0)


def msg_keyframe_request():
    return frame(KEYFRAME_REQUEST, b"")


def msg_input_batch(records=None):
    if records is None:
        records = [
            (INPUT_MOVE, 0, 960, 540, 0),
            (INPUT_KEY_DOWN, INPUT_MOD_SHIFT, 0, 0, 0x2A),
            (INPUT_KEY_UP, INPUT_MOD_SHIFT, 0, 0, 0x2A),
            (INPUT_TEXT, 0, 0, 0, ord("A")),
            (INPUT_BUTTON_DOWN, 0, 100, 200, 1),
            (INPUT_BUTTON_UP, 0, 100, 200, 1),
            (INPUT_WHEEL, 0, 100, 200, 0xFFFFFF88),  # -120 as a u32 bit pattern
        ]
    if not records:
        raise ValueError("an input batch carries at least one record")
    p = Writer()
    p.u16(len(records))
    for kind, flags, x, y, data in records:
        p.u8(kind)
        p.u8(flags)
        p.u16(0)
        p.i32(x)
        p.i32(y)
        p.u32(data)
    return frame(INPUT_BATCH, bytes(p.buf))


def msg_agent_hello(machine_id=MACHINE_ID, label="DESKTOP-EXAMPLE", os=OS_WINDOWS,
                    caps=CAP_ATTRIB_INTERACTIVE | CAP_ATTRIB_LOCKED, max_w=3840, max_h=2160,
                    encoders=ENC_H264_HW | ENC_H264_SW):
    p = Writer()
    # machine_id first, then a cosmetic label. The id is derived from the agent's key and is what
    # peers address the machine by; the label is free text, may repeat, and is only ever displayed.
    p.string(machine_id)
    p.string(label)
    p.u8(os)
    p.u16(caps)
    p.u16(max_w)
    p.u16(max_h)
    p.u8(encoders)
    return frame(AGENT_HELLO, bytes(p.buf))


def msg_agent_heartbeat(uptime=86_400, sessions=1):
    p = Writer()
    p.u32(uptime)
    p.u16(sessions)
    return frame(AGENT_HEARTBEAT, bytes(p.buf))


def msg_agent_stats(frames_sent=120_000, frames_dropped=7, bytes_sent=9_812_340_000,
                    encode_ms_avg=4, bitrate_kbps=3400, fps=30, queue_depth=0):
    p = Writer()
    # frames_sent stays u32 (4.5 years at 30 fps); bytes_sent must be u64, because a u32 byte
    # counter wraps at 4.29 GB, which is about eleven minutes of video. A counter that wraps is
    # worse than no counter, because it reads as a plausible number.
    p.u32(frames_sent)
    p.u32(frames_dropped)
    p.u64(bytes_sent)
    p.u16(encode_ms_avg)
    p.u16(bitrate_kbps)
    p.u8(fps)
    p.u8(queue_depth)
    return frame(AGENT_STATS, bytes(p.buf))


# --- the vector set -------------------------------------------------------------------------------


def roundtrips():
    """Well-formed messages covering every field type and every boundary the format has: both
    roles, all three OSes, a keyframe and a delta frame, a multi-rect damage list, a multi-byte
    string, empty and opaque byte payloads, a single-record batch, and both ends of every integer
    width.

    A third element, where present, is a list of `Class.field=value` expectations. These exist
    because byte-for-byte round-tripping is blind to one specific class of error: swapping two
    adjacent fields of the SAME width is a permutation that is its own inverse, so the decoder
    misreads the values and the re-encoder writes them back in the order it now believes, and the
    bytes match perfectly. A `width`/`height` swap is exactly that and round-trip will never see
    it. Pinning the value to the field *name* catches it. Every vector with two adjacent same-width
    scalars carries an expectation; see PROTOCOL.md section 2.4 for what this still does not cover.
    """
    return [
        ("hello_controller", msg_hello(role=ROLE_CONTROLLER)),
        # The id is Cyrillic on purpose: it is 8 characters but 10 UTF-8 bytes, so it fails if a
        # decoder measures the string in chars instead of bytes.
        ("hello_agent_multibyte_id", msg_hello(instance_id="agent-б", role=ROLE_AGENT)),
        ("hello_ack", msg_hello_ack(),
         ["HelloAck.maxWidth=3840", "HelloAck.maxHeight=2160", "HelloAck.maxMessage=16777216"]),
        ("error", msg_error()),
        ("error_long_message", msg_error(
            code=ERROR_AUTH_FAILED,
            message="agent declined: unattended capture is unavailable on this endpoint "
                    "(see PROTOCOL.md 6.2)")),
        ("open_session", msg_open_session(credential=b"rw1-example-token-material"),
         ["OpenSession.width=1920", "OpenSession.height=1080"]),
        # No credential at all: the shape a mod sends before the user has pasted a token in, which
        # the agent must treat as a rejected credential rather than as a legacy request.
        ("open_session_no_credential", msg_open_session(credential=b"")),
        ("open_session_full_length_credential", msg_open_session(
            credential=bytes(range(256)) * 2)),
        ("session_opened_interactive", msg_session_opened(capture=CAPTURE_INTERACTIVE),
         ["SessionOpened.width=1920", "SessionOpened.height=1080"]),
        ("session_opened_locked", msg_session_opened(capture=CAPTURE_LOCKED),
         ["SessionOpened.width=1920", "SessionOpened.height=1080"]),
        ("close_session", msg_close_session(reason=CLOSE_REPLACED)),
        ("ping", msg_ping()),
        ("pong", msg_pong()),
        ("bytes_empty", msg_bytes(b"")),
        ("bytes_opaque", msg_bytes()),
        ("config_h264_high", msg_config(), ["Config.width=1920", "Config.height=1080",
                                            "Config.bitrateKbps=3500"]),
        ("config_raw_lossless", msg_config(codec=CODEC_RAW, profile=PROFILE_MAIN,
                                           bitrate_kbps=0, fps=1, keyframe_interval=1,
                                           pix_fmt=PIX_FMT_YUV444),
         ["Config.width=1920", "Config.height=1080", "Config.bitrateKbps=0"]),
        ("frame_keyframe_full", msg_frame(), ["Frame.width=1920", "Frame.height=1080",
                                              "Frame.frameIndex=4096",
                                              "Frame.flags=1"]),
        ("frame_delta_multi_rect", msg_frame(
            frame_index=4097, pts=8_033_333, keyframe=False,
            rects=[(0, 0, 1920, 24), (37, 108, 640, 480), (-1, -1, 1, 1), (0, 0, 0, 0)],
            data=b"\x00\x00\x00\x01\x65\x88\x84\x00\x31" * 40),
         ["Frame.width=1920", "Frame.height=1080", "Frame.frameIndex=4097",
          "Frame.flags=0"]),
        ("frame_field_maximums", _frame_field_maximums(),
         ["Frame.width=65535", "Frame.height=65535", "Frame.frameIndex=4294967295"]),
        ("frame_field_minimums", _frame_field_minimums(),
         ["Frame.width=0", "Frame.height=0", "Frame.frameIndex=0"]),
        ("input_batch", msg_input_batch(), ["InputBatch.count=7"]),
        ("input_batch_single_record", msg_input_batch(
            records=[(INPUT_MOVE, INPUT_MOD_CTRL | INPUT_MOD_ALT, -1, -1, 0)]),
         ["InputBatch.count=1"]),
        ("agent_hello_windows", msg_agent_hello(),
         ["AgentHello.maxWidth=3840", "AgentHello.maxHeight=2160"]),
        ("agent_hello_linux_x11", msg_agent_hello(
            machine_id="1111222233334444", label="build-box-01", os=OS_LINUX,
            caps=CAP_ATTRIB_INTERACTIVE | CAP_ATTRIB_CONSENT_GATED, encoders=ENC_H264_SW),
         ["AgentHello.os=2", "AgentHello.maxWidth=3840", "AgentHello.maxHeight=2160"]),
        ("agent_hello_macos", msg_agent_hello(
            machine_id="aaaabbbbccccdddd", label="Mac-Studio-über", os=OS_MACOS,
            caps=CAP_ATTRIB_INTERACTIVE | CAP_ATTRIB_CONSENT_GATED, encoders=ENC_H264_HW),
         ["AgentHello.os=3"]),
        # Two endpoints may share a label, because nothing looks a machine up by it.
        ("agent_hello_duplicate_label", msg_agent_hello(
            machine_id="9999aaaabbbbcccc", label="DESKTOP-EXAMPLE", os=OS_WINDOWS)),
        ("agent_hello_empty_label", msg_agent_hello(machine_id="0000111122223333", label="")),
        ("agent_heartbeat", msg_agent_heartbeat()),
        ("agent_stats", msg_agent_stats(), ["AgentStats.bytesSent=9812340000",
                                            "AgentStats.bitrateKbps=3400"]),
        ("keyframe_request", msg_keyframe_request()),
    ]


def _frame_field_maximums():
    """Every scalar field at the top of its range, and a damage rect at every extreme. Catches a
    decoder that widens or narrows a field, and one that clamps instead of overflowing.

    The rect count is deliberately not pushed to its own 0xFFFF maximum: that count is only
    well-formed if 0xFFFF rects are actually present, which is a 512 KiB message that would say
    nothing a 16-rect list does not. The count's out-of-range behaviour is the REJECT vector
    `frame_rect_count_past_end` instead.
    """
    p = _frame_preamble(0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0xFFFF, 0xFFFF)
    p.u16(16)
    for _ in range(16):
        p.rect(0x7FFF, -0x8000, 0x7FFF, -0x8000)
    p.bytes_field(b"\xff" * 8)
    return frame(FRAME, bytes(p.buf), flags=0xFF)


def _frame_field_minimums():
    p = _frame_preamble(0, 0, 0, 0)
    p.u16(1)
    p.rect(0, 0, 0, 0)
    p.bytes_field(b"\x00")
    return frame(FRAME, bytes(p.buf), flags=0)


def rejects():
    """Input a conforming decoder MUST refuse.

    These are the security-relevant cases. Every one is an attempt to make a decoder read past the
    end of a buffer, allocate from an untrusted length, or disagree with its peer about where a
    message ends. A decoder that accepts any of them is a remote crash or a heap overflow.
    """
    hello_payload = msg_hello()[HEADER_SIZE:]

    return [
        # Length field larger than the bytes actually present: the classic overread.
        ("length_exceeds_payload", frame(HELLO, hello_payload, length=4096)),
        # Length field that parses cleanly but leaves bytes unconsumed. The decoder must require
        # exact consumption, or a peer can carry a second message inside the first one's payload.
        ("length_shorter_than_payload", frame(HELLO, hello_payload + b"\x00" * 8,
                                              length=len(hello_payload) - 8)),
        ("trailing_bytes_after_valid_message", msg_hello() + b"\x00"),
        # reserved must be zero. A non-zero value means the peer is speaking a different dialect
        # that we would otherwise misparse in silence, which is what this field exists to catch.
        ("reserved_nonzero", frame(HELLO, hello_payload, reserved=1)),
        # Length beyond the protocol maximum. A decoder that allocates before checking the bound
        # turns one message into an out-of-memory.
        ("length_over_max_message", frame(HELLO, hello_payload, length=MAX_MESSAGE + 1)),
        ("hello_payload_truncated", frame(HELLO, b"")),

        # A string whose length prefix runs past the end of the payload.
        ("agent_hello_string_length_past_end", frame(AGENT_HELLO, struct.pack("<H", 4096))),
        # A string of well-formed length but malformed UTF-8. A lenient decoder would substitute
        # U+FFFD and then re-encode different bytes than it received, so the two peers would
        # disagree about the id with no error anywhere.
        ("agent_hello_string_invalid_utf8", _agent_hello_invalid_utf8()),
        # A damage list claiming 65535 rects with none present: 512 KiB of claimed reads.
        ("frame_rect_count_past_end", frame(FRAME, bytes(_frame_preamble(1, 0, 16, 16).buf)
                                            + struct.pack("<H", 0xFFFF))),
        # Zero damage rects is not encodable: a frame always damages something, at minimum the
        # whole surface. Accepting 0 leaves the controller with nothing to upload.
        ("frame_rect_count_zero", _frame_with_rects(b"", 0)),
        # Zero-length coded picture.
        ("frame_empty_picture", _frame_with_rects(struct.pack("<I", 0), 1)),
        # An empty input batch is a no-op that still costs a round trip.
        ("input_batch_count_zero", frame(INPUT_BATCH, struct.pack("<H", 0))),
        # Count says 4 records, one is present.
        ("input_batch_count_past_end", frame(INPUT_BATCH, struct.pack("<H", 4) + b"\x00" * 12)),
        # A credential whose length prefix runs past the end of the message.
        ("open_session_credential_past_end", _open_session_credential_past_end()),
    ]


def _open_session_credential_past_end():
    """OPEN_SESSION with every fixed field present and a credential length that overruns."""
    p = Writer()
    p.string_bytes(b"0123456789abcdef")
    p.u16(1920)
    p.u16(1080)
    p.u8(QUALITY_HIGH)
    p.u32(4096)  # claims 4096 bytes of token, message ends here
    return frame(OPEN_SESSION, bytes(p.buf))


def _frame_with_rects(tail, rect_count):
    p = _frame_preamble(1, 0, 16, 16)
    p.u16(rect_count)
    p.raw(tail)
    return frame(FRAME, bytes(p.buf), flags=FRAME_FLAG_KEYFRAME)


def _agent_hello_invalid_utf8():
    """AGENT_HELLO whose agent_id is a lone continuation byte followed by an ASCII 'a'.

    0x80 is a UTF-8 continuation byte with no lead byte before it, so the sequence cannot be
    decoded. A lenient decoder turns it into U+FFFD and keeps going.
    """
    p = Writer()
    p.string_bytes(b"\x80a")
    p.u8(OS_WINDOWS)
    p.u16(CAP_ATTRIB_INTERACTIVE)
    p.u16(1920)
    p.u16(1080)
    p.u8(ENC_H264_HW)
    return frame(AGENT_HELLO, bytes(p.buf))


def main(argv):
    out = Path(argv[1]) if len(argv) > 1 else Path("build/protocol/vectors.txt")
    rts = roundtrips()
    rej = rejects()
    lines = [
        "# remote-worker wire-format vectors - generated by tools/protocol_vectors.py",
        "# PROTOCOL.md is the prose; THIS SCRIPT is the normative encoder. Do not hand-edit.",
        "# protocol version %d-%d, max message %d bytes, header %d bytes"
        % (PROTOCOL_VERSION_MIN, PROTOCOL_VERSION_MAX, MAX_MESSAGE, HEADER_SIZE),
        "#",
        "# ROUNDTRIP <name> <hex> [expect=Class.field=v,...]",
        "#     decode, then re-encode must reproduce these bytes exactly. The optional",
        "#     expectations pin scalar values to field NAMES, which is the only way to catch a",
        "#     swap of two same-width fields: such a swap is its own inverse, so the bytes still",
        "#     match. See PROTOCOL.md 2.4.",
        "# REJECT    <name> <hex>",
        "#     a conforming decoder must refuse this input.",
        "",
    ]
    for entry in rts:
        name, data = entry[0], entry[1]
        expect = entry[2] if len(entry) > 2 else None
        line = "ROUNDTRIP %s %s" % (name, data.hex())
        if expect:
            line += " expect=" + ",".join(expect)
        lines.append(line)
    lines.append("")
    for name, data in rej:
        lines.append("REJECT %s %s" % (name, data.hex()))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
    print("wrote %d round-trip and %d reject vectors to %s" % (len(rts), len(rej), out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
