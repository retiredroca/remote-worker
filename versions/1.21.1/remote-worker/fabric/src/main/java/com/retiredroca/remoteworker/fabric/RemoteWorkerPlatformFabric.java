package com.retiredroca.remoteworker.fabric;

import java.nio.file.Path;
import java.util.function.Supplier;

import com.retiredroca.remoteworker.RemoteWorker;
import com.retiredroca.remoteworker.platform.RemoteWorkerPlatform;

import net.fabricmc.api.EnvType;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.item.Item;

public class RemoteWorkerPlatformFabric implements RemoteWorkerPlatform {

    @Override
    public String loaderName() {
        return "Fabric";
    }

    @Override
    public boolean isDevelopmentEnvironment() {
        return FabricLoader.getInstance().isDevelopmentEnvironment();
    }

    @Override
    public Path configDir() {
        // The vanilla game directory, not FabricLoader.getConfigDir(): this is the same path on both
        // loaders, so the shared code has one place to look and the two cannot drift. The mod is
        // client-only, so touching Minecraft here is not a dedicated-server hazard.
        return net.minecraft.client.Minecraft.getInstance().gameDirectory.toPath()
                .resolve("config").resolve(RemoteWorker.MOD_ID);
    }

    @Override
    public Supplier<Item> registerTabletItem(Item item) {
        // Fabric registers immediately and hands the instance back, so the lookup is a constant.
        Item registered = Registry.register(BuiltInRegistries.ITEM, RemoteWorker.id("tablet"), item);
        return () -> registered;
    }

    @Override
    public void log(String message) {
        // The loader's own logger, so the message lands in the log the player was already told to
        // send rather than on a console they may never see.
        org.slf4j.LoggerFactory.getLogger(RemoteWorker.MOD_ID).warn(message);
    }

    @Override
    public boolean isClient() {
        return FabricLoader.getInstance().getEnvironmentType() == EnvType.CLIENT;
    }
}
