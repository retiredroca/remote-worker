package com.retiredroca.remoteworker.client;

import com.retiredroca.remoteworker.credential.KeyCodec;

/**
 * One machine that can be viewed: where it is, and the key that authorises us to view it.
 *
 * <p>Immutable. Changing an endpoint means replacing it, because a half-edited endpoint is a
 * configuration that cannot be reasoned about and the screen would have to show which fields are
 * valid.
 */
public record Endpoint(String label, String host, int port, String machineId, String key) {

    /** The default port the agent listens on. */
    public static final int DEFAULT_PORT = 47311;

    public Endpoint {
        if (host == null || host.isBlank()) {
            throw new IllegalArgumentException("an endpoint needs a host");
        }
        if (port < 1 || port > 65535) {
            throw new IllegalArgumentException("port out of range: " + port);
        }
    }

    /**
     * Builds an endpoint from what a user typed, or returns why it cannot be used.
     *
     * <p>The machine id is <b>derived from the key</b> rather than accepted from the user. That is
     * the same derivation the agent uses, so an endpoint cannot be configured with an id that
     * disagrees with its own key -- which would otherwise be a session that fails as "no such
     * endpoint" for a machine the user can see listed.
     */
    public static Endpoint parse(String label, String host, String port, String key) {
        String cleanHost = host == null ? "" : host.trim();
        if (cleanHost.isEmpty()) {
            return null;
        }
        int resolvedPort;
        try {
            resolvedPort = port == null || port.isBlank() ? DEFAULT_PORT : Integer.parseInt(port.trim());
        } catch (NumberFormatException e) {
            return null;
        }
        byte[] keyBytes = KeyCodec.decode(key);
        if (keyBytes == null) {
            return null;
        }
        String derived = KeyCodec.machineId(keyBytes);
        String cleanLabel = label == null || label.isBlank() ? cleanHost : label.trim();
        try {
            return new Endpoint(cleanLabel, cleanHost, resolvedPort, derived, key.trim());
        } catch (IllegalArgumentException e) {
            return null;
        }
    }

    /**
     * True when this endpoint names this machine.
     *
     * <p>Checked here so the screen can say why before a connection is attempted, and independently
     * checked by the agent, which is the one that can be sure. This copy is a courtesy; the agent's
     * refusal is the rule.
     */
    public boolean isLocalAddress() {
        String h = host.trim().toLowerCase(java.util.Locale.ROOT);
        return h.equals("localhost") || h.equals("127.0.0.1") || h.equals("::1")
                || h.equals("0.0.0.0") || h.startsWith("127.");
    }

    public String address() {
        return host + ":" + port;
    }
}
