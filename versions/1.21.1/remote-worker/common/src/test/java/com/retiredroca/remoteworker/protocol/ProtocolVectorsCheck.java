package com.retiredroca.remoteworker.protocol;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;

import com.retiredroca.remoteworker.credential.KeyCodec;

/**
 * Checks the shared codec against the vectors from {@code tools/protocol_vectors.py}.
 *
 * <p>Run on every build. The generator is the normative encoder, so the two properties that matter
 * are:
 *
 * <ul>
 *   <li><b>ROUNDTRIP</b> - decode, then re-encode, and get the original bytes back exactly. This is
 *       what catches a field width, an order, or a sign that differs between implementations. It is
 *       byte-for-byte rather than field-by-field, so a decoder that quietly accepted a value the
 *       encoder never produces is still caught, because the re-encode would differ.
 *   <li><b>REJECT</b> - the input must be refused. Every one of these is an attempt to make a
 *       decoder read past the end of a buffer or allocate from an untrusted length, so a decoder
 *       that accepts one is a remote crash rather than a cosmetic mismatch.
 * </ul>
 *
 * <p>Every failure is reported, not just the first: a drifted format usually breaks several vectors
 * at once, and fixing them one run at a time is slower than seeing the whole set.
 */
public final class ProtocolVectorsCheck {
    private static final HexFormat HEX = HexFormat.of();

    private ProtocolVectorsCheck() {
    }

    public static void main(String[] args) {
        if (args.length < 1) {
            System.err.println("usage: ProtocolVectorsCheck <vectors.txt>");
            System.exit(2);
        }
        Path path = Path.of(args[0]);
        if (!Files.isRegularFile(path)) {
            System.err.println("vectors file not found: " + path.toAbsolutePath());
            System.err.println("it is generated; run: python tools/protocol_vectors.py " + args[0]);
            System.exit(2);
        }

        List<String> failures = new ArrayList<>();
        int roundtrips = 0;
        int rejects = 0;

        List<String> lines;
        try {
            lines = Files.readAllLines(path, StandardCharsets.US_ASCII);
        } catch (IOException e) {
            System.err.println("cannot read " + path + ": " + e);
            System.exit(2);
            return;
        }

        for (String line : lines) {
            String trimmed = line.trim();
            if (trimmed.isEmpty() || trimmed.startsWith("#")) {
                continue;
            }
            String[] parts = trimmed.split("\\s+");
            if (parts.length < 3 || parts.length > 4) {
                failures.add("malformed vector line: " + trimmed);
                continue;
            }
            String kind = parts[0];
            String name = parts[1];
            byte[] bytes;
            try {
                bytes = HEX.parseHex(parts[2]);
            } catch (IllegalArgumentException e) {
                failures.add(name + ": not valid hex: " + e.getMessage());
                continue;
            }
            String expect = null;
            if (parts.length == 4) {
                if (!parts[3].startsWith("expect=")) {
                    failures.add(name + ": expected 'expect=' clause, got: " + parts[3]);
                    continue;
                }
                expect = parts[3].substring("expect=".length());
            }

            switch (kind) {
                case "ROUNDTRIP" -> {
                    roundtrips++;
                    checkRoundTrip(name, bytes, expect, failures);
                }
                case "REJECT" -> {
                    rejects++;
                    checkRejected(name, bytes, failures);
                }
                default -> failures.add("unknown vector kind '" + kind + "' for " + name);
            }
        }

        // An empty or truncated file would otherwise "pass" by asserting nothing.
        if (roundtrips == 0) {
            failures.add("no ROUNDTRIP vectors found in " + path);
        }
        if (rejects == 0) {
            failures.add("no REJECT vectors found in " + path);
        }

        System.out.println("protocol vectors: " + roundtrips + " round-trip, " + rejects + " reject");
        if (!failures.isEmpty()) {
            System.err.println();
            System.err.println(failures.size() + " FAILURE(S):");
            for (String failure : failures) {
                System.err.println("  - " + failure);
            }
            System.err.println();
            System.err.println("If the format itself changed on purpose, update");
            System.err.println("tools/protocol_vectors.py and PROTOCOL.md together, then rebuild.");
            System.exit(1);
        }
        System.out.println("protocol conformance OK");
        checkKeyIdentity();
    }

    /**
     * Checks the machine id the mod derives from a key against the value the C++ agent derives from
     * the same key.
     *
     * <p>These are two independent implementations of one wire-visible value: the id travels in
     * HELLO_ACK and in every OPEN_SESSION, so a mod that computed it differently would address a
     * machine by an id its agent refuses to answer to, and the symptom would be "no such endpoint"
     * for a machine the user can see listed. The vectors do not cover it -- they cover the message
     * <i>format</i>, and this is a derivation from a key -- so it is pinned here, against values
     * taken from the C++ implementation.
     *
     * <p>These three ids are the expected outputs of {@code agent/src/credential.cpp}. If a change
     * makes them differ, the C++ side is the one that is already released and on the wire, so it is
     * the Java side that has to match.
     */
    private static void checkKeyIdentity() {
        record Case(String key, String machineId) {
        }
        List<Case> cases = List.of(
                new Case("rw1_BKRQJ734CTGK8F30ACBGBHSWA533ANF8", "bb343d83a840d63b"),
                new Case("rw1_00000000000000000000000000000000", null),
                new Case("rw1_ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ", null));

        List<String> bad = new ArrayList<>();
        for (Case c : cases) {
            byte[] key = KeyCodec.decode(c.key);
            if (key == null) {
                bad.add("could not decode " + c.key);
                continue;
            }
            if (!c.key().equals(KeyCodec.encode(key))) {
                bad.add("re-encoding " + c.key + " gave " + KeyCodec.encode(key));
            }
            String derived = KeyCodec.machineId(key);
            if (c.machineId() != null && !c.machineId().equals(derived)) {
                bad.add("machine id for " + c.key + " is " + derived + ", expected " + c.machineId());
            }
            if (derived.length() != KeyCodec.MACHINE_ID_CHARS) {
                bad.add("machine id for " + c.key + " is " + derived.length() + " chars");
            }
        }
        // Lower case must decode: a key pasted through chat is often lower-cased on the way.
        if (KeyCodec.decode("rw1_bkrqj734ctgk8f30acbgbhswa533anf8") == null) {
            bad.add("a lower-cased key did not decode");
        }
        for (String junk : new String[] {"", "rw1_", "not-a-key", "rw1_!!", "rw1_0000000000000000000000000000000"}) {
            if (KeyCodec.decode(junk) != null) {
                bad.add("accepted a malformed key: '" + junk + "'");
            }
        }
        if (!bad.isEmpty()) {
            System.err.println("key identity FAILURE(S):");
            for (String b : bad) {
                System.err.println("  - " + b);
            }
            System.exit(1);
        }
        System.out.println("key identity OK (id derivation matches the C++ agent)");
    }

    private static void checkRoundTrip(String name, byte[] bytes, String expect,
                                       List<String> failures) {
        Protocol.Decoded decoded;
        try {
            decoded = Protocol.parseMessage(bytes, 0, bytes.length);
        } catch (ProtocolException e) {
            failures.add(name + ": ROUNDTRIP vector was rejected: " + e.getMessage());
            return;
        }
        Protocol.Header header = decoded.header;
        if (header.length != bytes.length - Protocol.HEADER_SIZE) {
            failures.add(name + ": header length " + header.length + " does not match payload of "
                    + (bytes.length - Protocol.HEADER_SIZE) + " bytes");
        }
        if (header.type != decoded.message.type()) {
            failures.add(name + ": header type 0x" + Integer.toHexString(header.type)
                    + " but message reports 0x" + Integer.toHexString(decoded.message.type()));
        }
        if (header.flags != decoded.message.flags()) {
            failures.add(name + ": header flags 0x" + Integer.toHexString(header.flags)
                    + " but message reports 0x" + Integer.toHexString(decoded.message.flags()));
        }
        byte[] reencoded = Protocol.encodeMessage(decoded.message);
        if (!Arrays.equals(bytes, reencoded)) {
            failures.add(name + ": re-encode differs\n"
                    + "      expected " + HEX.formatHex(bytes) + "\n"
                    + "      actual   " + HEX.formatHex(reencoded) + "\n"
                    + "      decoded  " + decoded.message);
            return;
        }
        if (expect != null) {
            checkExpectations(name, expect, decoded.message, failures);
        }
    }

    /**
     * Verifies pinned field values by name.
     *
     * <p>Round-tripping is blind to one specific class of error: swapping two adjacent fields of the
     * same width is a permutation that is its own inverse, so a mis-reading decoder re-encodes the
     * values in the order it now believes and the bytes match perfectly. Pinning a value to a field
     * <em>name</em> is the only way to see that. {@code Class.field=value}, comma-separated; the
     * class prefix is optional and checked when present, so a vector cannot quietly assert a field
     * of a different message than the one it decoded.
     */
    private static void checkExpectations(String name, String expect, Messages.Message message,
                                          List<String> failures) {
        for (String item : expect.split(",")) {
            int eq = item.indexOf('=');
            if (eq < 0) {
                failures.add(name + ": malformed expectation '" + item + "'");
                continue;
            }
            String key = item.substring(0, eq);
            long want;
            try {
                want = Long.parseLong(item.substring(eq + 1));
            } catch (NumberFormatException e) {
                failures.add(name + ": expectation '" + item + "' is not an integer");
                continue;
            }
            int dot = key.indexOf('.');
            String cls = dot < 0 ? "" : key.substring(0, dot);
            String field = dot < 0 ? key : key.substring(dot + 1);
            String simple = message.getClass().getSimpleName();
            if (!cls.isEmpty() && !cls.equals(simple)) {
                failures.add(name + ": expectation names " + cls + " but the vector decoded a "
                        + simple);
                continue;
            }
            Long actual = readMember(message, field);
            if (actual == null) {
                failures.add(name + ": " + simple + " has no readable field or method '" + field + "'");
            } else if (actual.longValue() != want) {
                failures.add(name + ": " + key + " is " + actual + ", expected " + want
                        + "  -- adjacent same-width fields swapped?  " + message);
            }
        }
    }

    /** Reads a public final field, or a public no-arg method, as a number. */
    private static Long readMember(Object target, String name) {
        try {
            java.lang.reflect.Field field = target.getClass().getField(name);
            return ((Number) field.get(target)).longValue();
        } catch (NoSuchFieldException e) {
            // fall through to a method of the same name
        } catch (ReflectiveOperationException e) {
            return null;
        }
        try {
            java.lang.reflect.Method method = target.getClass().getMethod(name);
            return ((Number) method.invoke(target)).longValue();
        } catch (ReflectiveOperationException e) {
            return null;
        }
    }

    private static void checkRejected(String name, byte[] bytes, List<String> failures) {
        try {
            Protocol.Decoded decoded = Protocol.parseMessage(bytes, 0, bytes.length);
            failures.add(name + ": REJECT vector was accepted as " + decoded.message
                    + " - a decoder that reads past the end of a buffer is a remote crash, and one"
                    + " that ignores a trailing length is a way to smuggle a second message");
        } catch (ProtocolException expected) {
            // The point of the vector.
        } catch (RuntimeException e) {
            failures.add(name + ": rejected with " + e.getClass().getName() + " instead of"
                    + " ProtocolException - callers catch ProtocolException, so anything else"
                    + " escapes the connection boundary: " + e);
        }
    }
}
