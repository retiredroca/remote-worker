package com.retiredroca.remoteworker.fabric;

import com.retiredroca.remoteworker.platform.ExampleModPlatform;

import net.fabricmc.loader.api.FabricLoader;

public class FabricPlatform implements ExampleModPlatform {
    @Override
    public String loaderName() {
        return "Fabric";
    }

    @Override
    public boolean isDevelopmentEnvironment() {
        return FabricLoader.getInstance().isDevelopmentEnvironment();
    }
}
