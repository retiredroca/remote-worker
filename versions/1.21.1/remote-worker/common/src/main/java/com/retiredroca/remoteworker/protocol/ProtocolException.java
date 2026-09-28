package com.retiredroca.remoteworker.protocol;

/**
 * Thrown when a peer sends something that does not conform to the wire format.
 *
 * <p>This is an {@link java.io.IOException} on purpose. Malformed input is expected on a socket
 * that anyone on the network can reach, so a decoder has no choice but to fail, and a checked
 * exception makes "what does the caller do about it" a question the compiler asks. Nothing in this
 * package catches it: the connection boundary is the only place that can decide whether to drop the
 * peer, and swallowing it anywhere below that turns a protocol error into a corrupted session.
 */
public class ProtocolException extends java.io.IOException {
    private static final long serialVersionUID = 1L;

    public ProtocolException(String message) {
        super(message);
    }

    public ProtocolException(String message, Throwable cause) {
        super(message, cause);
    }
}
