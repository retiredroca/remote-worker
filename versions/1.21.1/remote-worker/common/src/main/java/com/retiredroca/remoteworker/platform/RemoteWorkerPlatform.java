package com.retiredroca.remoteworker.platform;

import java.nio.file.Path;

import net.minecraft.world.item.Item;

/**
 * Loader-specific services used by the shared (common) code.
 *
 * <p>Each method here exists because the answer is genuinely different per loader, not to keep
 * loader types out of common code for its own sake. Adding to this interface means adding an
 * implementation to both loaders, and the compiler will say so.
 */
public interface RemoteWorkerPlatform {

    String loaderName();

    boolean isDevelopmentEnvironment();

    /** Where the mod's own files belong: `config/remote-worker/`. */
    Path configDir();

    /**
     * Registers {@code item} as {@code <mod id>:tablet} and returns a way to look it up.
     *
     * <p>A {@link java.util.function.Supplier} rather than the item, because the two loaders register
     * at different times and only one of them can hand the instance back immediately. The shared code
     * must not depend on which, so it is given a lookup and resolves it on first use.
     */
    java.util.function.Supplier<Item> registerTabletItem(Item item);

    /** True when running on a physical client, where the screen and the session logic may be used. */
    boolean isClient();

    /**
     * Reports a problem the player cannot see for themselves. Backed by the loader's own logger
     * rather than System.err, so a message lands in the log the player was already told to send.
     */
    void log(String message);
}
