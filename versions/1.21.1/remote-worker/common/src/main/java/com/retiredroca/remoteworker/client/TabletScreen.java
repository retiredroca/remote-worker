package com.retiredroca.remoteworker.client;

import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;

import com.retiredroca.remoteworker.RemoteWorker;

import net.minecraft.ChatFormatting;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.GuiGraphics;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.MultiLineLabel;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.network.chat.Component;

/**
 * What the tablet opens: the configured machines, and the result of trying one.
 *
 * <p>The screen reports the truth, including the failure modes, because that is the only useful
 * thing it can do right now. An agent that authenticates and then says it has no capture backend is
 * a diagnosis; the same thing showing as a black rectangle would be a bug report.
 */
public class TabletScreen extends Screen {

    /**
     * The connect attempts run here, never on the render thread. A TCP connect to an address that
     * drops packets blocks for the full timeout, and on the render thread that is a frozen game
     * with no way to close the screen.
     */
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "remote-worker-connect");
        thread.setDaemon(true);
        return thread;
    });

    private static final AtomicReference<AgentClient.Result> PENDING = new AtomicReference<>();

    private final List<Endpoint> endpoints = EndpointStore.load();
    private AgentClient.Result result;
    private String selectedLabel;

    private TabletScreen() {
        super(Component.translatable("screen.remote-worker.tablet"));
    }

    /** Opens the screen. The single entry point the item uses. */
    public static void open() {
        Minecraft.getInstance().setScreen(new TabletScreen());
    }

    @Override
    protected void init() {
        // Collect a result that arrived while the screen was closed, rather than losing it.
        AgentClient.Result arrived = PENDING.getAndSet(null);
        if (arrived != null) {
            this.result = arrived;
        }

        int centre = width / 2;
        int y = 60;
        int rowHeight = 24;
        int row = 0;

        for (Endpoint endpoint : endpoints) {
            if (row >= 6) {
                break;  // a screen that scrolls is a screen with a scroll bar; this is not that
            }
            int top = y + row * rowHeight;
            boolean selected = endpoint.label().equals(selectedLabel);
            addRenderableWidget(Button.builder(
                            Component.literal(endpoint.label()), button -> select(endpoint))
                    .bounds(centre - 150, top, 130, 20)
                    .build());
            addRenderableWidget(Button.builder(
                            Component.translatable("screen.remote-worker.connect"),
                            button -> connect(endpoint))
                    .bounds(centre - 14, top, 100, 20)
                    .build());
            if (endpoint.isLocalAddress()) {
                addRenderableWidget(Button.builder(
                                Component.literal("this PC"),
                                button -> {
                                })
                        .bounds(centre + 92, top, 58, 20)
                        .build());
            }
            row++;
        }

        if (endpoints.isEmpty()) {
            addRenderableWidget(Button.builder(Component.literal(""), button -> {
            }).bounds(0, 0, 0, 0).build());
        }

        addRenderableWidget(Button.builder(Component.translatable("gui.done"), button -> onClose())
                .bounds(centre - 100, height - 40, 200, 20)
                .build());
    }

    private void select(Endpoint endpoint) {
        this.selectedLabel = endpoint.label();
        this.result = null;
        rebuild();
    }

    private void connect(Endpoint endpoint) {
        if (endpoint.isLocalAddress()) {
            // Refused in the UI as well as in the agent, so the reason is visible before any socket
            // is opened. The agent's own check is the rule; this is the explanation.
            this.result = new AgentClient.Result(AgentClient.Status.REFUSED, endpoint.machineId(),
                    "That is this computer. A remote session has to be a different machine.");
            rebuild();
            return;
        }
        this.result = null;
        this.selectedLabel = endpoint.label();
        rebuild();
        int screenWidth = width;
        WORKER.execute(() -> {
            AgentClient.Result outcome = AgentClient.open(endpoint, screenWidth, 1080);
            PENDING.set(outcome);
            // Repaint from the worker: setScreen is not needed, the screen is still up.
            Minecraft.getInstance().execute(TabletScreen.this::rebuild);
        });
    }

    private void rebuild() {
        this.rebuildWidgets();
    }

    @Override
    public void render(GuiGraphics graphics, int mouseX, int mouseY, float partialTick) {
        super.render(graphics, mouseX, mouseY, partialTick);
        graphics.drawCenteredString(font, title, width / 2, 18, 0xFFFFFF);

        if (endpoints.isEmpty()) {
            graphics.drawCenteredString(font,
                    Component.translatable("screen.remote-worker.no_endpoints"), width / 2, 60, 0xAAAAAA);
            graphics.drawCenteredString(font,
                    Component.literal(EndpointStoreLocation.hint()), width / 2, 74, 0x888888);
            return;
        }

        int y = 60;
        for (Endpoint endpoint : endpoints) {
            boolean selected = endpoint.label().equals(selectedLabel);
            graphics.drawString(font, endpoint.address() + "  " + endpoint.machineId(),
                    8, y + 6, selected ? 0xFFFFFF : 0x999999);
            y += 24;
        }

        if (result != null) {
            int colour = switch (result.status()) {
                case OPENED -> 0x55FF55;
                case NO_CAPTURE -> 0xFFAA00;
                case REFUSED -> 0xFF5555;
                case UNREACHABLE -> 0xFF5555;
            };
            Component headline = Component.translatable(switch (result.status()) {
                case OPENED -> "screen.remote-worker.status.opened";
                case NO_CAPTURE -> "screen.remote-worker.status.no_capture";
                case REFUSED -> "screen.remote-worker.status.refused";
                case UNREACHABLE -> "screen.remote-worker.status.unreachable";
            }).withStyle(ChatFormatting.WHITE);
            graphics.drawCenteredString(font, headline, width / 2, height - 76, colour);
            graphics.drawCenteredString(font, Component.literal(result.detail()),
                    width / 2, height - 64, colour);
        }
    }

    @Override
    public void onClose() {
        Minecraft.getInstance().setScreen(null);
    }
}
