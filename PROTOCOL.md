# Wire protocol

The control protocol between the three components: the **mod** (the controller, in-game), the
**agent** (on the endpoint being viewed), and the **relay** (a LAN daemon that pairs them and
routes between them).

## 1. What defines this format

**`tools/protocol_vectors.py` is the normative encoder.** This document is the prose; the script is
the definition. `PROTOCOL.md` and the script are edited together, and the Java codec under
`common/` is checked against the script's bytes on every build.

That arrangement is deliberate. The format is hand-rolled rather than a Protobuf schema because a
codegen plugin in the Gradle build was the larger cost, and because three implementations (Java,
C++, C++) can drift apart silently. Instead:

- `python tools/protocol_vectors.py [outfile]` writes the vectors (default
  `build/protocol/vectors.txt`).
- `ProtocolVectorsCheck` decodes and re-encodes every one, and must be refused by the decoder for
  every malformed one. It runs from `check` in both loader builds, so `./gradlew build` runs it.
- `ctest` runs the same vectors against the C++ codec in `relay/`, plus an end-to-end test that
  pairs an agent and a controller through the relay over loopback sockets.

The vectors are generated into `build/` and not committed: the generator is the single source of
truth, so a committed copy could only ever disagree with it.

### Implementations

| where | language | state |
|---|---|---|
| `versions/<mc>/<module>/common/.../protocol/` | Java | codec complete, 40 vectors checked on every build |
| `relay/` | C++20 | codec complete, 40 vectors checked by `ctest`; relay routes and forwards them |
| `relay/`, later `agent/` | C++20 | capture, encode and input injection not started |

Two API notes, because they are the kind of thing that silently produces a wrong result:

- **The header's flags byte has exactly one owner.** On the Java side it is the message class's
  `flags`; in C++ it is `body_flags(const Body&)`, which derives it from the body that defines it.
  Carrying it in both the message and the body means the two can disagree while the bytes still
  round-trip, and the only symptom is a consumer reading the body seeing a keyframe as a delta frame.
- **A `u64` is a bit pattern, not a number.** Java `long` is already all 64 bits, so there is no
  range check to write; compare with `Long.compareUnsigned` and print with
  `Long.toUnsignedString`. `0xFFFFFFFFFFFFFFFF` reads back as `-1`.

## 2. Framing

### 2.1 Header

Every message is an 8-byte header followed by its payload.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     type      |     flags     |            reserved            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            length                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          payload ...                           |
```

| field | width | meaning |
|---|---|---|
| `type` | u8 | message type, see section 4 |
| `flags` | u8 | defined per message type; 0 where a type has no flags |
| `reserved` | u16 | must be 0; a non-zero value is refused |
| `length` | u32 | payload byte count, excluding this header |

`reserved` exists so that a peer speaking a dialect we do not know fails immediately and loudly
rather than being misparsed in silence. `flags` is the opposite: undefined bits are **preserved and
ignored**, so a newer sender can set a bit an older receiver carries through unharmed, and a frame
still round-trips byte-for-byte. Rejecting unknown flags would make every new flag a breaking
change, which is `reserved`'s job instead.

A decoder must require the message region to be consumed **exactly**. `Protocol.parseMessage` takes
a region, parses the header, and refuses unless `HEADER_SIZE + length == region length`, and then
refuses any payload with bytes left over. In a stream the surplus is the next message, so the
framing layer sizes the region from the header first; the requirement is what makes it safe to
validate a received region in one call.

### 2.2 Byte order

All integers are little-endian. Every platform this ships on is little-endian (x86-64, aarch64 on
Windows and Linux, arm64 macOS), so there is no byte-order negotiation and no per-field cost to
being explicit.

### 2.3 Limits

| limit | value | why |
|---|---|---|
| `MAX_MESSAGE` | 16 MiB | bounds the allocation a single length field can provoke. A decoder refuses an over-limit length **before** allocating, so a 4-byte field cannot ask for gigabytes of heap |
| `MAX_STRING` | 1024 bytes | the protocol carries ids and diagnostics, not file contents |

### 2.4 What the conformance check does and does not prove

`ProtocolVectorsCheck` asserts two things per vector: that a well-formed message decodes and
re-encodes to the identical bytes, and that a malformed one is refused with a `ProtocolException`.

That is enough to catch a changed field width, a changed field order between different-width
fields, an added or removed field, a lenient UTF-8 decode, a bounds check that reads past the end
of a buffer, and a decoder that ignores a length.

It is **not** enough to catch a swap of two adjacent fields of the *same* width. Such a permutation
is its own inverse: the decoder misreads `width=1920, height=1080` as `width=1080, height=1920` and
re-encodes them in the order it now believes, so the bytes match perfectly. `width`/`height`,
`x`/`y` and `min`/`max` are exactly this shape. Those are caught instead by pinned expectations:
a vector may carry `expect=Config.width=1920,Config.height=1080`, and the check resolves each
`Class.field` **by name** via reflection and compares the value.

Not covered at all: a permutation *inside* a nested value, such as a `Rect`'s `w` and `h`. Those
rely on this document and on the end-to-end tests, where a mis-swapped damage rectangle shows up
as visibly wrong pixels. Adding a tag to every field would close that too, at the cost that made
the hand-rolled format not worth it.

## 3. Transport and authentication

**No transport encryption, by decision.** Everything on the wire is readable by anything that can
see the traffic. Accepted: this is a LAN-only, client-only mod on a network the user controls.
`rw::ByteStream` is the seam where a `TlsStream` would go, and the design is kept in the two
layers below, in the order they would be worth doing.

### The key is the machine's identity

**The machine id is derived from the key, and the key is the only secret.** The agent has no name of
its own; it has a key, and `machine_id(key)` (16 hex characters) is what peers address it by. A
separate `label` rides along for display only — it may repeat between endpoints, and nothing looks a
machine up by it.

The identity has to be *derived* rather than equal to the key, and that is the whole reason a hash is
involved: the id is **public** (it travels in `AGENT_HELLO` and in every `OPEN_SESSION`) while the
key is **secret**. If they were the same bytes, anyone who read a machine id off the wire could
present it as that machine's credential.

The consequence worth stating plainly: **regenerating a key produces a new identity.** Peers see a
new machine. That is correct rather than unfortunate — after a rotation nothing can prove the machine
is the same one, so claiming otherwise would be a lie. The old pairing stops working, which is what
rotation is for.

### Why the key is random, and not derived from the machine

A key derived from a MAC address, an IP address, a hostname, or any other machine property was
considered and rejected, because each of those is *public* and therefore not a key at all:

- **A MAC address is in every frame on the LAN.** A credential computed from one is readable by
  anything that can see the traffic, with no privilege required. So is an IPv4 address, via ARP,
  DHCP, the packet headers, and the router's own admin page.
- **MAC addresses are forgeable.** Arbitrary Ethernet frames can carry any source address, so even a
  "unique" one is not proof of which machine is on the other end.
- **It would make keys *duplicable*, which is the opposite of the goal.** A cloned VM disk image
  carries the same MAC, so two machines would share a key — a collision that authenticates both. DHCP
  hands the same machine a different address tomorrow, a machine with Wi-Fi and Ethernet has two,
  and IPv6 privacy addresses rotate by design. Each of those would unpair a machine that never
  changed.
- **It does not survive what the user actually asked for.** Regenerating a key on demand has to work
  without invalidating the machine's identity by accident, and a property-derived key cannot offer
  that.

Randomness from the OS CSPRNG is what makes the guarantee real: 160 bits puts the chance of two
machines colliding at about 3e-14 across a billion machines, and regeneration is unconditional. The
way to *also* stop a key being copied to another machine is a hardware secret — a TPM or a Secure
Enclave, which never releases the key — and that is a genuine dependency on optional hardware, so it
is a later item rather than this one.

The relay keeps a `DuplicateEndpoint` guard as a backstop. It is not the mechanism: the id is
already unique by construction. It was a guard in the relay's endpoint registry, which is gone with
the relay, and it stays in the error enum because an id that somehow did collide must be *denied*
rather than silently accepted as the same machine.

**The agent generates the key, and the agent is the only thing that checks it.** The consequences
worth stating, because each is a property that came from the design rather than being bolted on:

- **There is no middleman holding a key.** With the relay gone, no other process ever sees a
  credential it could reuse.
- **Revocation is per machine.** Rotating one agent's key invalidates only that agent, and only
  costs that machine its pairings. A single shared secret would have to be rotated everywhere at
  once, which is a much worse answer to "this one machine is compromised".
- **The user can mint and revoke at will**, from the agent's own CLI: `remote-worker keygen` and
  `remote-worker keygen --rotate`.

### There is no relay

A controller holds a list of endpoints -- each an address plus the key for that machine -- and
connects to each one directly. The agent is the only program that runs on a watched machine, and
there is nothing in the middle for it to talk to.

The relay that used to sit between them still exists in the tree and is still tested, because deleting
it is a separate change from removing it from the product. Nothing ships it.

The visible consequence is that the **agent's id is not negotiated**: the controller has to be told
it, and `remote-worker keygen` prints it next to the key for exactly that reason. A controller that
asks for the wrong id is told so, with both ids in the message, rather than being redirected.

### The flow, all of it in existing messages:

1. `remote-worker keygen` mints a key on the machine being watched: 20 bytes from the OS CSPRNG,
   formatted `rw1_` plus 32 Crockford base32 characters. It is shown **once**, together with the
   `machine_id` derived from it, and written to the agent's key file. The mod holds the same key and
   stores it in the OS keychain.
2. The user configures the endpoint in the mod: the machine's address, plus that machine id and key.
3. The mod connects to `host:port`, sends `HELLO` as a controller, and gets `HELLO_ACK` — which
   names the machine that was actually reached, taken from its own key rather than from anything the
   controller claimed, so a misconfigured address is visible in the log of the machine that was
   actually contacted.
4. The mod sends `OPEN_SESSION` naming that `machine_id` and carrying the key in its `credential`
   field.
5. The agent compares the key in constant time.
   - wrong `machine_id`, or a key that does not match, or no key at all → `ERROR` / `AuthFailed`
   - match, but already serving → `ERROR` / `AgentBusy`
   - match → `SESSION_OPENED`, and the agent starts capturing
6. The connection stays up for the session, carrying `FRAME` messages one way and `INPUT_BATCH` the
   other, and is closed by `CLOSE_SESSION` or by dropping the socket.

`AGENT_HELLO` and `AGENT_HEARTBEAT` exist in the format for an agent that announces itself to a
registry. With no relay there is no registry, so nothing sends them yet; they stay in the type
registry because removing a message type changes the wire format and the vectors, and a type that is
defined but unused is not a cost worth paying for.

The id routes; the key authorises. They are the same object, which is why the id cannot be forged:
a controller that knows a machine's id still cannot reach it without the key, and a controller that
has the key for one machine cannot present another machine's id and be served, because the agent
compares against its own stored key and nothing else.

An empty `credential` is a rejected credential, not a legacy request. There is no version of the
protocol without one.

### Why a wrong key and a wrong machine id are both `AuthFailed`

With the relay gone this got simpler rather than more subtle, and it is worth keeping the reasoning,
because the obvious "improvement" reintroduces a real leak.

The retired relay answered `NotFound` instead of `AuthFailed`, because a `NotFound`/`AuthFailed`
distinction turns a reachable relay into an **enumeration oracle**: a caller with no key could walk
a list of guessed machine names and learn which are real and which are refusing — from the error
code alone, before seeing a screen. That is a map, worth more than any single screen.

Direct connections change the shape of the question but not the answer. The agent is addressed by
`host:port`, so the set of machines is no longer a list the controller walks; a caller who has reached
*this* port already knows *this* machine exists. What it still gains by being careful is the
distinction between "wrong machine id" and "wrong key", which would confirm that a guessed id is
live. So the agent answers `AuthFailed` for both, and puts both ids in the message text -- which
names the real machine *and* says nothing an attacker did not already have. `agent_test` pins it.

`AgentBusy` is likewise only ever sent after a credential has been accepted, so a caller with no key
cannot learn whether a machine is in use.

### What is still unauthenticated

**The connection, before `OPEN_SESSION`.** `HELLO` checks only the protocol range, so any host that
can reach the port can pair as a controller and read the agent's `HELLO_ACK` — which names the
machine. The credential gates the *session*, not the port. This is deliberate: the agent has exactly
one thing to check against, and spending it at connect time would mean one credential per TCP
connection and a key exchange on every reconnect, for a listener that is already refusing to do
anything until it sees a valid key.

The consequence, stated plainly: **an agent on a shared network announces itself to anything that
connects.** TLS over the `ByteStream` seam is what closes that, and it is the first thing to add
rather than the capture backend, because it changes what a port on an untrusted network exposes.

`PING` is answered before authentication, deliberately: a controller has to be able to tell a dead
endpoint from a slow one, and liveness is already implied by the port being open. `agent_test` pins
both of these, so hardening either one changes a named test rather than a comment.

### Key storage, and the one thing deferred

Keys are 160-bit random values, compared with a constant-time compare. `remote-worker keygen` mints
one, prints it **once** alongside the machine id derived from it, and writes it to the agent's key
file with owner-only permissions. Running `keygen` again reports the existing machine id and refuses
to reprint the key; `--rotate` replaces it, which necessarily changes the machine id and so requires
every controller to be paired again.

The agent will not start without a usable key, and says to run `keygen`. An agent that started without
one would be an unauthenticated remote-control port, which is not a default worth shipping.

The controller stores the key in the OS keychain (Windows Credential Manager, macOS Keychain,
libsecret).

The agent storing the token **in the clear** rather than as a hash is the deliberate simplification
here, and it is the piece most worth revisiting: someone who can read the agent's config can then
watch that endpoint. The upgrade is to store `SHA-256(token)` instead and show the token only once,
which is a change to one field of one file. It is not done because this project has taken no crypto
dependency, and a from-scratch SHA-256 is the kind of thing that wants its own test vectors before
it is trusted with a secret.

## 4. Type registry

Grouped into ranges by category, so a message can be added later without renumbering its siblings.

| range | category |
|---|---|
| `0x00`-`0x0F` | session and connection |
| `0x10`-`0x1F` | media, agent to controller, relayed |
| `0x20`-`0x2F` | input, controller to agent, relayed |
| `0x30`-`0x3F` | agent control and telemetry |

| type | name | direction |
|---|---|---|
| `0x01` | `HELLO` | any to relay |
| `0x02` | `HELLO_ACK` | relay to any |
| `0x03` | `ERROR` | any |
| `0x04` | `OPEN_SESSION` | controller to relay |
| `0x05` | `SESSION_OPENED` | relay to controller |
| `0x06` | `CLOSE_SESSION` | controller to relay |
| `0x07` | `PING` | controller and agent, relayed |
| `0x08` | `PONG` | controller and agent, relayed |
| `0x0E` | `BYTES` | controller and agent, relayed, opaque |
| `0x10` | `CONFIG` | agent to controller, relayed |
| `0x11` | `FRAME` | agent to controller, relayed |
| `0x12` | `KEYFRAME_REQUEST` | controller to agent, relayed |
| `0x20` | `INPUT_BATCH` | controller to agent, relayed |
| `0x30` | `AGENT_HELLO` | agent to relay |
| `0x31` | `AGENT_HEARTBEAT` | agent to relay |
| `0x32` | `AGENT_STATS` | agent to relay to controller |

## 5. Messages

Scalars are `u8`/`u16`/`u32`/`u64`/`i16`/`i32`. `string` is `u16` length in **bytes** then UTF-8,
decoded strictly. `bytes` is `u32` length then the raw run. `rect` is four `i16`: `x, y, w, h`.

A `u64` has no Java type. `long` is already all 64 bits, so the Java side carries it as a pattern:
compare with `Long.compareUnsigned`, print with `Long.toUnsignedString`. `0xFFFFFFFFFFFFFFFF` reads
back as `-1`.

### 5.1 `HELLO` / `HELLO_ACK`

```
HELLO       u16 protocol_min, u16 protocol_max, u8 role, string instance_id
HELLO_ACK   u16 protocol, string instance_id, u32 max_message, u16 max_width, u16 max_height
```

`HELLO` is the first message on any connection. A peer whose `[min, max]` does not overlap the
relay's is refused with `ERROR` / `UNSUPPORTED_VERSION`. `max_message` lets the relay lower the
limit for everyone rather than negotiating it per message.

### 5.2 `ERROR`

```
u16 code, string message
```

Codes: 0 unspecified, 1 unsupported version, 2 auth failed, 3 not found, 4 bad message, 5 limit
exceeded, 6 agent busy, 7 unsupported capture. `message` is for a human reading a log and is never
parsed.

### 5.3 `OPEN_SESSION` / `SESSION_OPENED` / `CLOSE_SESSION`

```
OPEN_SESSION     string machine_id, u16 width, u16 height, u8 quality, bytes credential
SESSION_OPENED   string session_id, u16 width, u16 height, u8 capture
CLOSE_SESSION    u16 reason
```

`quality` is a hint, not a command: 0 low, 1 medium, 2 high, 3 lossless. The agent's adaptive rate
controller is free to disagree, and reports what it actually settled on in `CONFIG`.

`machine_id` is the id derived from the endpoint's key, and `credential` is that key — see
section 3. The relay forwards it
without reading it and the agent is the only party that checks it, so an empty one is a rejected
credential rather than a legacy request. The mod's `OpenSession.toString()` deliberately prints the
credential's *length* and not its bytes: a protocol dump in a log or a bug report is exactly where a
token would otherwise end up.

`capture` reports what the endpoint can actually do, taken from `AGENT_HELLO`: 0 unknown,
1 interactive, 2 locked. The controller shows this before a session starts, because a locked
Windows endpoint is the case where capture may not be possible at all (see section 6.2).

`reason`: 0 client, 1 agent, 2 error, 3 replaced by a newer session.

### 5.4 `PING` / `PONG`

```
u32 id, u64 timestamp_micros
```

Not decoration: the round trip is what the adaptive rate controller and the on-screen latency
readout are computed from.

### 5.5 `BYTES`

```
bytes payload
```

Opaque to the relay, which forwards it without reading it. Carries the end-to-end session
(section 3). The relay enforces `MAX_MESSAGE` and nothing else.

### 5.6 `CONFIG`

```
u16 width, u16 height, u8 codec, u8 profile, u16 bitrate_kbps, u8 fps,
u16 keyframe_interval, u8 pix_fmt
```

Sent when the encoder's parameters change, so the controller can show them and size its texture.

`bitrate_kbps` is `u16`, not `u8`: a `u8` bitrate caps at 255 kbps, which is below what a
full-screen desktop needs even at a low quality target, so the field would have been unable to
express the values it exists for. 65 Mbps is comfortably above any LAN rate.

`keyframe_interval` is in frames, and on a clean LAN it should be **long** (seconds, not frames).
Frequent keyframes exist to let a lossy link recover; there is no loss to recover from here, so a
long GOP is a large bandwidth saving at identical visual quality. This is the one place where the
LAN-only scope is a direct win rather than a limitation.

`codec`: 0 H.264, 1 H.265, 2 AV1, 3 raw. `profile`: 0 baseline, 1 main, 2 high. `pix_fmt`: 0 YUV420,
1 YUV444.

### 5.7 `FRAME`

```
u32 frame_index, u64 pts_micros, u16 width, u16 height, u16 rect_count,
rect[rect_count], bytes data
```

`data` is one coded picture. `rect_count` must be at least 1 and `data` must be non-empty; a
decoder refuses both. The header's keyframe flag is bit 0.

**The coded picture covers the whole frame. The rect list is not an encoder partition.** It is the
damage hint, and the controller uses it to upload only the changed part of its texture. Unchanged
regions compress to almost nothing through inter-prediction, so partitioning the encode per
rectangle would cost prediction quality across the tile seams and buy very little bandwidth. A
frame with no change at all is not sent, which is where "smallest bandwidth" is actually won.

`pts_micros` is `u64` because a `u32` wraps after about 71 minutes.

`rect` components are `i16`: a damage rect is a screen coordinate and never wider than a display,
and a frame's damage list can hold thousands of them.

### 5.8 `KEYFRAME_REQUEST`

Empty payload. Sent by the controller when it needs a frame it can decode without a reference: after
a stall, after joining mid-stream, or after a resolution change.

### 5.9 `INPUT_BATCH`

```
u16 count, record[count]      // 12 bytes per record
record: u8 kind, u8 flags, u16 reserved(0), i32 x, i32 y, u32 data
```

Batched because a mouse at its polling rate would otherwise cost one 8-byte header per event, and
the header is sized for the frame payload rather than a 12-byte record.

`kind`: 1 move, 2 button down, 3 button up, 4 wheel, 5 key down, 6 key up, 7 text. `flags` bits:
0 shift, 1 ctrl, 2 alt, 3 meta, 4 caps, 5 repeat.

`data` is the button mask, the wheel delta, the scancode, or **one UTF-8 byte** for `text`.
Non-ASCII text is therefore a run of records, which is a deliberate trade: it keeps the record
fixed-size and keeps a variable-length tail out of the hottest path in the protocol.

`count` must be at least 1, and `reserved` must be 0 in every record. A count is checked against the
bytes remaining **before** the array is allocated, so a 22-byte message cannot make a 65535-element
array appear.

### 5.10 `AGENT_HELLO` / `AGENT_HEARTBEAT` / `AGENT_STATS`

```
AGENT_HELLO       string machine_id, string label, u8 os, u16 capabilities, u16 max_width,
                  u16 max_height, u8 encoders
AGENT_HEARTBEAT   u32 uptime_seconds, u16 active_sessions
AGENT_STATS       u32 frames_sent, u32 frames_dropped, u64 bytes_sent, u16 encode_ms_avg,
                  u16 bitrate_kbps, u8 fps, u8 input_queue_depth
```

`machine_id` is derived from the agent's key; `label` is cosmetic, may repeat, and is never used to
look a machine up. `os`: 1 Windows, 2 Linux, 3 macOS. `capabilities`: bit 0 interactive, bit 1 locked, bit 2
multi-monitor, bit 3 consent-gated. `encoders`: bit 0 H.264 hardware, bit 1 H.264 software,
bit 2 AV1 hardware.

`bytes_sent` is `u64` because a `u32` byte counter wraps at 4.29 GB, about eleven minutes of video.
A counter that wraps is worse than none, because it still reads as a plausible number.
`frames_sent` stays `u32`: that is 4.5 years at 30 fps.

## 6. Agent capability model

### 6.1 Consent-gated platforms

Linux is X11 only. On Wayland, GNOME's Mutter and KDE's KWin do not implement
`zwp_virtual_keyboard_manager_v1`; the supported route is the XDG `RemoteDesktop` portal, which
shows a one-time native consent dialog. An agent on such a desktop sets `CAP_ATTRIB_CONSENT_GATED`
so the controller can say so, rather than the user discovering it when their clicks do not land.
macOS sets the same bit, because capture needs Screen Recording and input needs Accessibility, both
granted per-app and interactively.

### 6.2 Unattended endpoints

`CAP_ATTRIB_LOCKED` claims the agent can capture a locked desktop. **This is unverified on
Windows.** A process in session 1 capturing its own desktop is expected to get no frames once the
workstation is locked, because that is the secure desktop; the alternative is an IddCx mirror
driver, a signed kernel-adjacent driver and a much larger project.

The claim is therefore carried as a capability bit and asserted by the agent, rather than assumed
by the controller. If the spike in M6 shows locked capture returns nothing, the bit goes away and
unattended Windows is reported as unsupported instead of silently delivering a frozen picture.

## 7. Versioning

`HELLO` carries `[protocol_min, protocol_max]`. Version 1 is `1`-`1`. A change that only adds a
message type in an unused slot, or sets a new `flags` bit, is compatible. A change that reorders,
resizes or removes a field is not, and needs the version range moved.

Unknown message types are refused rather than skipped, because a length-prefixed format cannot skip
an unknown payload safely without trusting its internal length.
