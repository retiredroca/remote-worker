package com.retiredroca.remoteworker.client;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.Socket;
import com.retiredroca.remoteworker.credential.KeyCodec;
import com.retiredroca.remoteworker.protocol.Messages;
import com.retiredroca.remoteworker.protocol.Protocol;
import com.retiredroca.remoteworker.protocol.ProtocolException;

/**
 * One attempt to open a session with one agent: connect, greet, ask, read the answer.
 *
 * <p>Deliberately synchronous and short-lived. A session would live on the render thread, and doing
 * this inline on that thread would freeze the game for the length of a TCP connect, so a caller
 * runs it on a worker thread.
 *
 * <p>It touches no Minecraft class, so it can be reasoned about -- and eventually tested -- without
 * the game. Every outcome is a {@link Result} rather than an exception, because the only job the
 * caller has is put one honest sentence in front of the player.
 */
public final class AgentClient {

    /** What came back. */
    public record Result(Status status, String agentMachineId, String detail) {
    }

    public enum Status {
        /** The agent authenticated us and opened a session. */
        OPENED,
        /** The agent refused: AuthFailed, SelfConnection, AgentBusy, or a version we do not speak. */
        REFUSED,
        /** The agent authenticated us and then said it cannot capture. */
        NO_CAPTURE,
        /** We never reached the agent, or it stopped talking mid-handshake. */
        UNREACHABLE
    }

    private static final int CONNECT_TIMEOUT_MS = 4000;
    private static final int READ_TIMEOUT_MS = 4000;

    private AgentClient() {}

    /**
     * Attempts a session and returns what happened. Never throws.
     *
     * @param width  the display size being asked for; the agent decides what it can actually give
     */
    public static Result open(Endpoint endpoint, int width, int height) {
        if (endpoint.isLocalAddress()) {
            // Refused here as well as by the agent. A local address is never a remote machine, and
            // saying so immediately is better than a connect that fails, or a SelfConnection round
            // trip. The agent still checks independently -- that is the rule, this is the courtesy.
            return new Result(Status.REFUSED, endpoint.machineId(),
                    "That is this computer. A remote session has to be a different machine.");
        }
        byte[] key = KeyCodec.decode(endpoint.key());
        if (key == null) {
            return new Result(Status.REFUSED, endpoint.machineId(), "That is not a valid key.");
        }

        try (Socket socket = new Socket()) {
            socket.connect(new InetSocketAddress(endpoint.host(), endpoint.port()), CONNECT_TIMEOUT_MS);
            socket.setSoTimeout(READ_TIMEOUT_MS);
            socket.setTcpNoDelay(true);
            DataInputStream in = new DataInputStream(socket.getInputStream());
            DataOutputStream out = new DataOutputStream(socket.getOutputStream());

            Messages.Hello hello = new Messages.Hello(Protocol.VERSION_MIN, Protocol.VERSION_MAX,
                    Protocol.ROLE_CONTROLLER, "minecraft");
            send(out, hello);

            Messages.Message ack = receive(in);
            if (ack.type() != Protocol.TYPE_HELLO_ACK) {
                return new Result(Status.UNREACHABLE, endpoint.machineId(),
                        describe(ack) + " arrived where a handshake answer was expected.");
            }
            Messages.HelloAck helloAck = (Messages.HelloAck) ack;
            String agentId = helloAck.instanceId;
            if (!agentId.equals(endpoint.machineId())) {
                // The agent names itself from its own key, so this is a configuration mismatch and
                // saying so beats letting the OPEN_SESSION come back as a bare AuthFailed.
                return new Result(Status.REFUSED, agentId,
                        "That machine calls itself " + agentId + ", not " + endpoint.machineId()
                                + ". Its key has probably been regenerated.");
            }

            send(out, new Messages.OpenSession(endpoint.machineId(), width, height,
                    Protocol.QUALITY_HIGH, key));

            Messages.Message reply = receive(in);
            if (reply.type() == Protocol.TYPE_SESSION_OPENED) {
                Messages.SessionOpened opened = (Messages.SessionOpened) reply;
                return new Result(Status.OPENED, agentId,
                        "Session open at " + opened.width + "x" + opened.height + ".");
            }
            if (reply.type() == Protocol.TYPE_ERROR) {
                Messages.Error error = (Messages.Error) reply;
                return new Result(classify(error.code), agentId, error.message);
            }
            return new Result(Status.UNREACHABLE, agentId, describe(reply) + " arrived unexpectedly.");
        } catch (ProtocolException e) {
            // Before IOException, because it *is* one: a protocol fault is a different answer from a
            // dead socket, and the player deserves to be told which happened.
            return new Result(Status.UNREACHABLE, endpoint.machineId(),
                    "The agent said something this mod cannot read (" + e.getMessage() + ").");
        } catch (IOException e) {
            return new Result(Status.UNREACHABLE, endpoint.machineId(),
                    "Could not reach " + endpoint.address() + " (" + e.getMessage() + ").");
        }
    }

    private static Status classify(int code) {
        if (code == Protocol.ERROR_UNSUPPORTED_CAPTURE) {
            return Status.NO_CAPTURE;
        }
        return Status.REFUSED;
    }

    private static String describe(Messages.Message message) {
        return "a message of type 0x" + Integer.toHexString(message.type());
    }

    private static void send(DataOutputStream out, Messages.Message message) throws IOException {
        out.write(Protocol.encodeMessage(message));
        out.flush();
    }

    /**
     * Reads one framed message.
     *
     * <p>The header is parsed before the body is read so the declared length decides how much to
     * read, and the check that the declared length is not absurd happens in {@code parseHeader} --
     * before a peer can make this allocate 16 MiB on demand.
     */
    private static Messages.Message receive(DataInputStream in) throws IOException, ProtocolException {
        byte[] headerBytes = in.readNBytes(Protocol.HEADER_SIZE);
        if (headerBytes.length < Protocol.HEADER_SIZE) {
            throw new ProtocolException("the agent closed the connection before sending a message");
        }
        Protocol.Header header = Protocol.parseHeader(headerBytes, 0, Protocol.HEADER_SIZE);
        byte[] region = new byte[Protocol.HEADER_SIZE + header.length];
        System.arraycopy(headerBytes, 0, region, 0, Protocol.HEADER_SIZE);
        if (header.length > 0) {
            byte[] body = in.readNBytes(header.length);
            if (body.length < header.length) {
                throw new ProtocolException("the agent closed the connection mid-message");
            }
            System.arraycopy(body, 0, region, Protocol.HEADER_SIZE, header.length);
        }
        return Protocol.parseMessage(region, 0, region.length).message;
    }
}
