package com.retiredroca.remoteworker.fabric;

import com.retiredroca.remoteworker.ExampleMod;

import net.fabricmc.api.ModInitializer;

public class ExampleModFabric implements ModInitializer {
    @Override
    public void onInitialize() {
        ExampleMod.setPlatform(new FabricPlatform());
    }
}
