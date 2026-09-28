package com.retiredroca.remoteworker.neoforge;

import com.retiredroca.remoteworker.ExampleMod;

import net.neoforged.fml.common.Mod;

@Mod(ExampleMod.MOD_ID)
public class ExampleModNeoForge {
    public ExampleModNeoForge() {
        ExampleMod.setPlatform(new NeoForgePlatform());
    }
}
