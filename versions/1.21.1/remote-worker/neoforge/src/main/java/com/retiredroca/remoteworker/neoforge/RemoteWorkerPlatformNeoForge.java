package com.retiredroca.remoteworker.neoforge;

import java.nio.file.Path;
import java.util.function.Supplier;

import com.retiredroca.remoteworker.RemoteWorker;
import com.retiredroca.remoteworker.platform.RemoteWorkerPlatform;

import net.minecraft.world.item.Item;
import net.neoforged.api.distmarker.Dist;
import net.neoforged.fml.loading.FMLEnvironment;

public class RemoteWorkerPlatformNeoForge implements RemoteWorkerPlatform {

    @Override
    public String loaderName() {
        return "NeoForge";
    }

    @Override
    public boolean isDevelopmentEnvironment() {
        return !FMLEnvironment.production;
    }

    @Override
    public Path configDir() {
        // The same vanilla game directory the Fabric side uses, so the config lands in one place and
        // the two loaders cannot drift. See the note in the Fabric implementation.
        return net.minecraft.client.Minecraft.getInstance().gameDirectory.toPath()
                .resolve("config").resolve(RemoteWorker.MOD_ID);
    }

    /**
     * Queues the item and returns a lookup that resolves after registration has run.
     *
     * <p>The registry cannot be read here: {@link RemoteWorkerNeoForge#ITEMS} is only attached to
     * the mod event bus when the constructor returns, and the bus has not run yet, so a lookup done
     * now would find air. The supplier defers it to first use.
     */
    @Override
    public Supplier<Item> registerTabletItem(Item item) {
        RemoteWorkerNeoForge.ITEMS.register("tablet", () -> item);
        return () -> net.minecraft.core.registries.BuiltInRegistries.ITEM
                .get(RemoteWorker.id("tablet"));
    }

    @Override
    public void log(String message) {
        com.mojang.logging.LogUtils.getLogger().warn("[{}] {}", RemoteWorker.MOD_ID, message);
    }

    @Override
    public boolean isClient() {
        return FMLEnvironment.dist == Dist.CLIENT;
    }
}
