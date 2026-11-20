package wjbench.mctick;

import java.net.Proxy;
import java.util.Optional;
import java.util.function.BooleanSupplier;
import net.minecraft.SystemReport;
import net.minecraft.gizmos.GizmoCollector;
import net.minecraft.gizmos.Gizmos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.Services;
import net.minecraft.server.WorldStem;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.progress.LoggingLevelLoadListener;
import net.minecraft.server.notifications.EmptyNotificationService;
import net.minecraft.server.notifications.NotificationManager;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.permissions.LevelBasedPermissionSet;
import net.minecraft.server.permissions.PermissionSet;
import net.minecraft.server.players.NameAndId;
import net.minecraft.server.players.PlayerList;
import net.minecraft.util.datafix.DataFixers;
import net.minecraft.util.debugchart.LocalSampleLogger;
import net.minecraft.util.debugchart.SampleLogger;
import net.minecraft.world.level.storage.LevelStorageSource;

/*
 * A MinecraftServer with no network listener and no run loop of its own: Mctick drives it one tick at a time on the
 * thread that constructed it. Modelled on vanilla's GameTestServer (also headless), with the integrated server's
 * answers where they differ (not dedicated, not published, the joining player owns it). View and simulation distance
 * are the 262n seed's client options (12 and 12), which the integrated server copies into its PlayerList.
 */
final class BenchServer extends MinecraftServer {
    private final LocalSampleLogger sampleLogger = new LocalSampleLogger(4);
    private final int viewDistance;
    private final int simulationDistance;
    NullConnection playerConnection;
    NameAndId owner;

    BenchServer(Thread serverThread, LevelStorageSource.LevelStorageAccess access, PackRepository packs, WorldStem stem,
            Services services, int viewDistance, int simulationDistance) {
        super(serverThread, access, packs, stem, Optional.empty(), Proxy.NO_PROXY, DataFixers.getDataFixer(), services,
                LoggingLevelLoadListener.forDedicatedServer(), false, new NotificationManager());
        this.viewDistance = viewDistance;
        this.simulationDistance = simulationDistance;
    }

    @Override
    protected boolean initServer() {
        PlayerList players = new PlayerList(this, this.registries(), this.playerDataStorage, new EmptyNotificationService()) {
        };
        players.setViewDistance(viewDistance);
        players.setSimulationDistance(simulationDistance);
        this.setPlayerList(players);
        Gizmos.withCollector(GizmoCollector.NOOP);
        this.loadLevel();
        return true;
    }

    /* One server tick, as MinecraftServer.processPacketsAndTick runs it once the client's packets are in. */
    void tickOnce(BooleanSupplier haveTime) {
        this.tickServer(haveTime);
    }

    /* The work runServer's waitUntilNextTick does between ticks: main-thread tasks, then each level's chunk tasks. */
    void runTasks() {
        this.runAllTasks();
        for (ServerLevel level : this.getAllLevels()) {
            while (level.getChunkSource().pollTask()) {
                // drain
            }
        }
    }

    /* Every queued main-thread task is due. A real server finishes a tick well inside its 50 ms and runs the queue in
     * the time left (haveTime() holds until the next tick's deadline). This loop has no idle time and never advances
     * that deadline (runServer does), so MinecraftServer would hold each task until it was 3 ticks old. Tasks that
     * reschedule themselves then crawl: ServerLevel gives EntityStorage the server as its deserializer's executor, one
     * chunk a run, and HotSpot had loaded the entities of 197 of 841 full chunks after 600 ticks (2026-11-19). TeaVM
     * loaded all of them: there every executor runs on this thread, and server.execute runs a task inline. */
    @Override
    protected boolean shouldRun(net.minecraft.server.TickTask task) {
        return true;
    }

    @Override
    protected void waitUntilNextTick() {
        runTasks();
        Platform.idle();
    }

    @Override
    protected void tickConnection() {
        super.tickConnection();
        if (playerConnection != null) {
            playerConnection.tick();
        }
    }

    @Override
    protected SampleLogger getTickTimeLogger() {
        return sampleLogger;
    }

    @Override
    public boolean isTickTimeLoggingEnabled() {
        return false;
    }

    @Override
    public SystemReport fillServerSystemReport(SystemReport systemReport) {
        systemReport.setDetail("Type", "wjharness mctick bench server");
        return systemReport;
    }

    @Override
    public LevelBasedPermissionSet operatorUserPermissions() {
        return LevelBasedPermissionSet.GAMEMASTER;
    }

    @Override
    public PermissionSet getFunctionCompilationPermissions() {
        return LevelBasedPermissionSet.GAMEMASTER;
    }

    @Override
    public boolean shouldRconBroadcast() {
        return false;
    }

    @Override
    public boolean isDedicatedServer() {
        return false;
    }

    @Override
    public int getRateLimitPacketsPerSecond() {
        return 0;
    }

    @Override
    public int getCommandSpamThresholdSeconds() {
        return 0;
    }

    @Override
    public int getChatSpamThresholdSeconds() {
        return 0;
    }

    @Override
    public boolean useNativeTransport() {
        return false;
    }

    @Override
    public boolean isPublished() {
        return false;
    }

    @Override
    public boolean shouldInformAdmins() {
        return true;
    }

    @Override
    public boolean isSingleplayerOwner(NameAndId nameAndId) {
        return owner != null && owner.equals(nameAndId);
    }

    @Override
    public int getMaxPlayers() {
        return 8;
    }
}
