package com.retiredroca.remoteworker.item;

import com.retiredroca.remoteworker.client.TabletScreen;
import com.retiredroca.remoteworker.RemoteWorker;

import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResultHolder;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;

/**
 * The tablet: the item that opens a remote session.
 *
 * <p>Right-clicking it opens the endpoint screen. Nothing else about the item is interesting, and
 * that is deliberate -- a tablet is not a tool, it is a way in, so the whole behaviour is one use.
 *
 * <p>The screen is a client-side class. It is only referenced from the client branch of
 * {@link #use}, so a dedicated server never loads it. {@link RemoteWorker#platform()}'s
 * {@code isClient()} is what decides that branch, rather than an assumption about which loader is
 * running.
 */
public class TabletItem extends Item {

    public TabletItem(Properties properties) {
        super(properties);
    }

    @Override
    public InteractionResultHolder<ItemStack> use(Level level, Player player, InteractionHand hand) {
        ItemStack held = player.getItemInHand(hand);
        if (level.isClientSide() && RemoteWorker.platform().isClient()) {
            TabletScreen.open();
        }
        // Consume the click either way. PASS here would let the same click also swing or place
        // whatever the player is looking at, which reads as a mis-registered item. Sided success
        // rather than success(): the server half of this is a no-op, and claiming a swing animation
        // on the client for a screen that opened is a small lie the player can see.
        return InteractionResultHolder.sidedSuccess(held, level.isClientSide());
    }

    /**
     * A short, honest tooltip. The screen is where the detail lives.
     *
     * <p>Both parameter types are named rather than imported because in 1.21.1 {@code TooltipContext}
     * is nested in {@code Item} while {@code TooltipFlag} is a top-level class in the same package,
     * which is easy to get backwards and only shows up at compile time.
     */
    @Override
    public void appendHoverText(ItemStack stack, Item.TooltipContext context,
                                java.util.List<net.minecraft.network.chat.Component> tooltip,
                                net.minecraft.world.item.TooltipFlag flag) {
        tooltip.add(net.minecraft.network.chat.Component.translatable("item.remote-worker.tablet.tip"));
    }
}
