package wjbench.mctick;

import io.netty.channel.ChannelFutureListener;
import java.util.ArrayDeque;
import java.util.function.Consumer;
import net.minecraft.network.Connection;
import net.minecraft.network.DisconnectionDetails;
import net.minecraft.network.PacketListener;
import net.minecraft.network.ProtocolInfo;
import net.minecraft.network.TickablePacketListener;
import net.minecraft.network.protocol.BundlePacket;
import net.minecraft.network.protocol.Packet;
import net.minecraft.network.protocol.PacketFlow;
import net.minecraft.network.protocol.common.ClientboundKeepAlivePacket;
import net.minecraft.network.protocol.common.ServerboundKeepAlivePacket;
import net.minecraft.network.protocol.game.ClientboundChunkBatchFinishedPacket;
import net.minecraft.network.protocol.game.ClientboundLevelChunkWithLightPacket;
import net.minecraft.network.protocol.game.ServerboundChunkBatchReceivedPacket;

/*
 * The bench player's connection: no channel, no pipeline, no netty event loop. Every packet the server sends is
 * counted and dropped, as the integrated server's in-memory connection hands packet objects over without encoding them
 * (the work of BUILDING a packet, chunk serialisation included, stays on the server thread, where the game pays it).
 *
 * The client side is the minimum a real client does that changes what the server does:
 *   - a keep-alive is answered, or the server disconnects the player after 15 s of wall time;
 *   - a chunk batch is acknowledged, or PlayerChunkSender stops sending after the first batch (the rate a real client
 *     asks for depends on how fast it meshes; CHUNKS_PER_TICK is fixed so the work is the same on every arm).
 * Answers are queued and delivered by deliver() before the next tick, which is where a real client's packets land
 * (MinecraftServer.processPacketsAndTick handles them before tickServer).
 *
 * tick() is vanilla's minus the channel: it ticks the listener (ServerGamePacketListenerImpl.tick -> player.doTick).
 * BenchServer.tickConnection calls it at the point of the tick where ServerConnectionListener.tick would.
 */
final class NullConnection extends Connection {
    static final float CHUNKS_PER_TICK = 9.0F;   // ChunkBatchSizeCalculator's starting estimate

    private PacketListener listener;
    private final ArrayDeque<Packet<?>> toServer = new ArrayDeque<>();
    long packetsSent;
    long chunksSent;
    DisconnectionDetails disconnected;

    NullConnection() {
        super(PacketFlow.SERVERBOUND);
    }

    @Override
    public <T extends PacketListener> void setupInboundProtocol(ProtocolInfo<T> protocol, T packetListener) {
        this.listener = packetListener;
    }

    @Override
    public void setupOutboundProtocol(ProtocolInfo<?> protocol) {
    }

    @Override
    public void send(Packet<?> packet) {
        sent(packet);
    }

    @Override
    public void send(Packet<?> packet, ChannelFutureListener channelListener) {
        sent(packet);
    }

    @Override
    public void send(Packet<?> packet, ChannelFutureListener channelListener, boolean flush) {
        sent(packet);
    }

    private void sent(Packet<?> packet) {
        ++packetsSent;
        if (packet instanceof BundlePacket<?> bundle) {
            for (Packet<?> sub : bundle.subPackets()) {
                sent(sub);
            }
        } else if (packet instanceof ClientboundKeepAlivePacket keepAlive) {
            toServer.add(new ServerboundKeepAlivePacket(keepAlive.getId()));
        } else if (packet instanceof ClientboundChunkBatchFinishedPacket) {
            toServer.add(new ServerboundChunkBatchReceivedPacket(CHUNKS_PER_TICK));
        } else if (packet instanceof ClientboundLevelChunkWithLightPacket) {
            ++chunksSent;
        }
    }

    /* Queue a packet as if the client had sent it; delivered by the next deliver(). */
    void fromClient(Packet<?> packet) {
        toServer.add(packet);
    }

    void deliver() {
        Packet<?> packet;
        while ((packet = toServer.poll()) != null) {
            handle(packet, listener);
        }
    }

    @SuppressWarnings("unchecked")
    private static <T extends PacketListener> void handle(Packet<T> packet, PacketListener listener) {
        packet.handle((T) listener);
    }

    @Override
    public void runOnceConnected(Consumer<Connection> action) {
        action.accept(this);
    }

    @Override
    public void flushChannel() {
    }

    @Override
    public void tick() {
        if (listener instanceof TickablePacketListener tickable) {
            tickable.tick();
        }
    }

    @Override
    public boolean isConnected() {
        return disconnected == null;
    }

    @Override
    public boolean isConnecting() {
        return false;
    }

    @Override
    public boolean isMemoryConnection() {
        return true;
    }

    @Override
    public void disconnect(DisconnectionDetails details) {
        if (disconnected == null) {
            disconnected = details;
            Mctick.log("[mctick] DISCONNECTED: " + details.reason().getString());
        }
    }

    @Override
    public void setReadOnly() {
    }

    @Override
    public void handleDisconnection() {
    }

    @Override
    public PacketListener getPacketListener() {
        return listener;
    }
}
