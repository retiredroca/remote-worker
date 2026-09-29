package com.retiredroca.remoteworker.client;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.retiredroca.remoteworker.RemoteWorker;

/**
 * The configured endpoints, on disk in {@code config/remote-worker/endpoints.json}.
 *
 * <p>A file rather than in-game entry, for now: the screen can read it and connect, but adding an
 * endpoint is a text field's worth of UI that is worth doing properly rather than approximately.
 *
 * <p><b>This file holds keys in the clear.</b> That is the same deliberate simplification the agent
 * makes with its own key file, and it is the piece most worth revisiting: anyone who can read this
 * file can view every machine listed in it. The Windows equivalent of the agent's owner-only key
 * file is the credential store, and the mod should move there the moment a loader offers one.
 */
public final class EndpointStore {

    private static final String FILE_NAME = "endpoints.json";

    private EndpointStore() {}

    private static Path file() {
        return RemoteWorker.platform().configDir().resolve(FILE_NAME);
    }

    /** The configured endpoints. A missing or unreadable file is an empty list, not a crash. */
    public static List<Endpoint> load() {
        Path path = file();
        if (!Files.isRegularFile(path)) {
            return new ArrayList<>();
        }
        try {
            String text = Files.readString(path, StandardCharsets.UTF_8);
            JsonArray array = JsonParser.parseString(text).getAsJsonArray();
            List<Endpoint> out = new ArrayList<>();
            for (var element : array) {
                Endpoint parsed = fromJson(element.getAsJsonObject());
                if (parsed != null) {
                    out.add(parsed);
                }
            }
            return out;
        } catch (IOException | RuntimeException e) {
            // A corrupt file must not stop the game loading. Say so, and carry on with none.
            RemoteWorker.platform().log("could not read " + path + ": " + e.getMessage());
            return new ArrayList<>();
        }
    }

    public static void save(List<Endpoint> endpoints) throws IOException {
        Path path = file();
        Files.createDirectories(path.getParent());
        JsonArray array = new JsonArray();
        for (Endpoint endpoint : endpoints) {
            JsonObject o = new JsonObject();
            o.addProperty("label", endpoint.label());
            o.addProperty("host", endpoint.host());
            o.addProperty("port", endpoint.port());
            o.addProperty("key", endpoint.key());
            array.add(o);
        }
        Files.writeString(path, array.toString() + "\n", StandardCharsets.UTF_8);
    }

    private static Endpoint fromJson(JsonObject o) {
        try {
            // machineId is recomputed from the key rather than read: a stored id that disagrees with
            // its own key is exactly the state that produces a confusing "no such endpoint".
            return Endpoint.parse(o.get("label").getAsString(),
                    o.get("host").getAsString(),
                    o.get("port").getAsString(),
                    o.get("key").getAsString());
        } catch (RuntimeException e) {
            return null;
        }
    }
}
