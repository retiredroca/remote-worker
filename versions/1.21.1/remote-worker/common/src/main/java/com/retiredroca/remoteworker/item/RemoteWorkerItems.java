package com.retiredroca.remoteworker.item;

import java.util.function.Supplier;

import net.minecraft.world.item.Item;

/**
 * Holds the mod's items, resolved lazily.
 *
 * <p>A supplier rather than an instance, and that is not a stylistic choice. The two loaders
 * register through different mechanisms at different times: Fabric's {@code Registry.register}
 * hands the instance straight back, while NeoForge's {@code DeferredRegister} has not run at all
 * when the mod constructor returns -- registration happens later on the mod event bus. Reading the
 * registry at construction time on NeoForge therefore returns air.
 *
 * <p>So each loader registers and then returns a way to look the item up, and the lookup resolves
 * only after the registry is populated. Nothing downstream has to know which loader it is running
 * on, and none of it can hold the wrong instance.
 *
 * <p>At the moment only {@link #registerTablet} is called: the tablet is reached through the
 * registry by the item id today, so the accessors below have no caller yet. They exist because the
 * lazy supplier is the whole reason this class exists, and the first thing that wants a typed handle
 * rather than a registry lookup is the renderer.
 */
public final class RemoteWorkerItems {

    private static Supplier<Item> tablet;

    private RemoteWorkerItems() {}

    public static void registerTablet(Supplier<Item> tabletItem) {
        if (tablet != null) {
            throw new IllegalStateException(
                    "items registered twice - was a loader entrypoint run twice?");
        }
        tablet = tabletItem;
    }

    /**
     * The tablet as the game holds it.
     *
     * @return the item, or {@code null} if a loader entrypoint has not registered it yet
     */
    public static Item tablet() {
        return tablet == null ? null : tablet.get();
    }

    /** True once a loader has registered and the lookup now resolves. */
    public static boolean isReady() {
        return tablet != null && tablet.get() != null;
    }
}
