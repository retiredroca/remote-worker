package com.retiredroca.remoteworker;

import com.retiredroca.remoteworker.platform.ExampleModPlatform;

/**
 * Loader-neutral entry point. Each loader's entrypoint installs its own {@link ExampleModPlatform}
 * implementation; the shared code then uses {@link #platform()} for loader-specific operations.
 */
public final class ExampleMod {
    public static final String MOD_ID = "remote-worker";

    private static ExampleModPlatform platform;

    private ExampleMod() {}

    public static void setPlatform(ExampleModPlatform platform) {
        ExampleMod.platform = platform;
    }

    public static ExampleModPlatform platform() {
        if (platform == null) {
            throw new IllegalStateException("ExampleMod platform not set - was a loader entrypoint loaded?");
        }
        return platform;
    }
}
