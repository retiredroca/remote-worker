package com.retiredroca.remoteworker;

import com.retiredroca.remoteworker.platform.RemoteWorkerPlatform;

import net.minecraft.resources.ResourceLocation;

/**
 * Loader-neutral entry point. Each loader's entrypoint installs its own {@link RemoteWorkerPlatform}
 * and calls {@link #bootstrap()}, which registers everything the mod adds to the game.
 *
 * <p>The split exists because the two loaders register through different mechanisms while the item
 * and its behaviour live here. Nothing in this class may touch a loader type.
 */
public final class RemoteWorker {
    public static final String MOD_ID = "remote-worker";

    private static RemoteWorkerPlatform platform;

    private RemoteWorker() {}

    public static void setPlatform(RemoteWorkerPlatform platform) {
        RemoteWorker.platform = platform;
    }

    public static RemoteWorkerPlatform platform() {
        if (platform == null) {
            throw new IllegalStateException(
                    "RemoteWorker platform not set - was a loader entrypoint loaded?");
        }
        return platform;
    }

    public static ResourceLocation id(String path) {
        return ResourceLocation.fromNamespaceAndPath(MOD_ID, path);
    }
}
