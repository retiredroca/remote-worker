package com.retiredroca.remoteworker.client;

import com.retiredroca.remoteworker.RemoteWorker;

/** Where the endpoint file lives, as a string for the screen to show. */
public final class EndpointStoreLocation {
    private EndpointStoreLocation() {}

    public static String hint() {
        return RemoteWorker.platform().configDir().resolve("endpoints.json").toString();
    }
}
