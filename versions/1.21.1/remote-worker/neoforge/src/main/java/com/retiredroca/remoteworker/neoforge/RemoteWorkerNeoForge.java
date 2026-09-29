package com.retiredroca.remoteworker.neoforge;

import com.retiredroca.remoteworker.RemoteWorker;
import com.retiredroca.remoteworker.item.RemoteWorkerItems;
import com.retiredroca.remoteworker.item.TabletItem;

import net.minecraft.world.item.Item;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.fml.common.Mod;
import net.neoforged.neoforge.registries.DeferredRegister;

@Mod(RemoteWorker.MOD_ID)
public class RemoteWorkerNeoForge {

    public static final DeferredRegister.Items ITEMS =
            DeferredRegister.createItems(RemoteWorker.MOD_ID);

    public RemoteWorkerNeoForge(IEventBus modEventBus) {
        RemoteWorker.setPlatform(new RemoteWorkerPlatformNeoForge());
        ITEMS.register(modEventBus);
        RemoteWorkerItems.registerTablet(RemoteWorker.platform()
                .registerTabletItem(new TabletItem(new Item.Properties().stacksTo(1))));
    }
}
