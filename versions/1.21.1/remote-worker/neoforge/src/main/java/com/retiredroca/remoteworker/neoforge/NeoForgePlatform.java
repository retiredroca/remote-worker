package com.retiredroca.remoteworker.neoforge;

import com.retiredroca.remoteworker.platform.ExampleModPlatform;

import net.neoforged.fml.loading.FMLEnvironment;

public class NeoForgePlatform implements ExampleModPlatform {
    @Override
    public String loaderName() {
        return "NeoForge";
    }

    @Override
    public boolean isDevelopmentEnvironment() {
        return !FMLEnvironment.production;
    }
}
