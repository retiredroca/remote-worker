package com.retiredroca.remoteworker.fabric;

import com.retiredroca.remoteworker.RemoteWorker;
import com.retiredroca.remoteworker.item.RemoteWorkerItems;
import com.retiredroca.remoteworker.item.TabletItem;

import net.fabricmc.api.ModInitializer;
import net.minecraft.world.item.Item;

public class RemoteWorkerFabric implements ModInitializer {
    @Override
    public void onInitialize() {
        RemoteWorker.setPlatform(new RemoteWorkerPlatformFabric());
        RemoteWorkerItems.registerTablet(RemoteWorker.platform()
                .registerTabletItem(new TabletItem(new Item.Properties().stacksTo(1))));
    }
}
