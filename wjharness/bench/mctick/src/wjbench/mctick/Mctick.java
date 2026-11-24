package wjbench.mctick;

import com.mojang.authlib.GameProfile;
import com.mojang.serialization.Dynamic;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.UUID;
import java.util.stream.Stream;
import net.minecraft.SharedConstants;
import net.minecraft.commands.Commands;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.WorldLoader;
import net.minecraft.server.WorldStem;
import net.minecraft.server.level.ClientInformation;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.network.CommonListenerCookie;
import net.minecraft.server.network.config.PrepareSpawnTask;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.permissions.LevelBasedPermissionSet;
import net.minecraft.server.players.NameAndId;
import net.minecraft.network.protocol.game.ServerboundClientTickEndPacket;
import net.minecraft.network.protocol.game.ServerboundMovePlayerPacket;
import net.minecraft.network.protocol.game.ServerboundPlayerLoadedPacket;
import net.minecraft.util.Util;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.entity.player.ChatVisiblity;
import net.minecraft.server.level.ParticleStatus;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.dimension.LevelStem;
import net.minecraft.world.level.storage.LevelDataAndDimensions;
import net.minecraft.world.level.storage.LevelStorageSource;
import net.minecraft.world.level.storage.LevelSummary;

/*
 * mctick: Minecraft 26.2's server tick as a fixed-work benchmark, the server-side counterpart of the jbox2d kernel
 * (wjharness/bench/README.md). It loads a save (default: the 262n seed's "New World", peaceful), joins the save's
 * player through vanilla's own spawn path (PrepareSpawnTask: player data, chunk preload, placeNewPlayer) on a
 * connection that drops packets, lets the world settle, then runs ticks back to back and times them.
 *
 *   java -cp <26.2 client.jar + libraries + mctick.jar> wjbench.mctick.Mctick [warmupTicks] [batchTicks] [batches]
 *
 * System properties: mctick.world=<save dir> (required), mctick.work=<scratch dir the save is copied into; default a
 * temp dir>, mctick.view / mctick.sim (12 / 12, the seed's options), mctick.warmup / batchTicks / batches (the
 * positional args win).
 *
 * Every arm runs the same Java: the same jar for HotSpot, ours (ikvmcraft ?bench) and IKVM on CoreCLR, and these
 * sources through TeaVM, where Platform (src-teavm/) runs the executors' work on this thread.
 *
 * Output (j2d/run's parser shapes, so the same readers work):
 *   [bench] batch i/N: <ms> ms (<ms/tick> ms/tick; tick-only <ms/tick>)
 *   [bench] mean <ms/tick> ms/tick over <ticks> ticks; best batch <ms/tick>
 *   [bench] checksum <long>      a digest of the world after the run (game time, chunk and entity counts by type)
 * A batch's time is the whole server thread between ticks too (client packets, main-thread and chunk tasks), as the
 * game's server thread pays it; tick-only is tickServer alone.
 */
public final class Mctick {
    private Mctick() {
    }

    public static void main(String[] args) throws Exception {
        try {
            run(args);
        } catch (Throwable t) {
            // TeaVM's page sees only an opaque WebAssembly.Exception: say what it was first
            log("[mctick] FAILED: " + t);
            for (Throwable c = t.getCause(); c != null && c != c.getCause(); c = c.getCause()) {
                log("[mctick]   caused by " + c);
            }
            t.printStackTrace();
            throw t;
        }
    }

    private static void run(String[] args) throws Exception {
        Platform.init();
        int warmup = intArg(args, 0, "mctick.warmup", 2000);
        int batchTicks = intArg(args, 1, "mctick.batchTicks", 100);
        int batches = intArg(args, 2, "mctick.batches", 20);
        int view = Integer.getInteger("mctick.view", 12);
        int sim = Integer.getInteger("mctick.sim", 12);
        String worldProp = System.getProperty("mctick.world");
        if (worldProp == null) {
            throw new IllegalArgumentException("set -Dmctick.world=<save directory>");
        }
        Path world = Paths.get(worldProp);
        Path work = System.getProperty("mctick.work") != null ? Paths.get(System.getProperty("mctick.work"))
                : Files.createTempDirectory("mctick");
        long t0 = System.nanoTime();
        log("[mctick] platform " + Platform.NAME + "; schedule warmup=" + warmup + " ticks, then " + batches
                + " batches of " + batchTicks + " ticks; view " + view + ", simulation " + sim);

        String levelName = world.getFileName().toString();
        Path saves = work.resolve("saves");
        log("[mctick] copying " + world + " to " + saves.resolve(levelName));
        copyTree(world, saves.resolve(levelName));
        log("[mctick] save copied to " + saves.resolve(levelName) + " in " + msSince(t0) + " ms; " + saveCheck(world,
                saves.resolve(levelName)));

        SharedConstants.tryDetectVersion();
        log("[mctick] game version " + SharedConstants.getCurrentVersion().id() + ", data version "
                + SharedConstants.getCurrentVersion().dataVersion());
        Bootstrap.bootStrap();
        log("[mctick] bootstrap done at " + msSince(t0) + " ms");
        log("[mctick] chunk check: " + chunkCheck(saves.resolve(levelName)));

        LevelStorageSource source = LevelStorageSource.createDefault(saves);
        LevelStorageSource.LevelStorageAccess access = source.createAccess(levelName);
        Dynamic<?> unfixed = access.getUnfixedDataTagWithFallback();
        LevelSummary summary = access.fixAndGetSummaryFromTag(unfixed);
        if (summary.requiresManualConversion() || !summary.isCompatible()) {
            throw new IllegalStateException("save is not loadable by this version: " + summary);
        }
        Dynamic<?> levelData = net.minecraft.util.datafix.DataFixers.getFileFixer()
                .fix(access, unfixed, new net.minecraft.util.worldupdate.UpgradeProgress());
        PackRepository packs = ServerPacksSource.createPackRepository(access);
        WorldDataConfiguration dataConfig = LevelStorageSource.readDataConfig(levelData);
        WorldLoader.InitConfig init = new WorldLoader.InitConfig(new WorldLoader.PackConfig(packs, dataConfig, false, false),
                Commands.CommandSelection.INTEGRATED, LevelBasedPermissionSet.GAMEMASTER);
        WorldStem stem = Util.blockUntilDone(executor -> WorldLoader.load(init, context -> {
            Registry<LevelStem> dims = context.datapackDimensions().lookupOrThrow(Registries.LEVEL_STEM);
            LevelDataAndDimensions data = LevelStorageSource.getLevelDataAndDimensions(access, levelData,
                    context.dataConfiguration(), dims, context.datapackWorldgen());
            return new WorldLoader.DataLoadOutput<>(data.worldDataAndGenSettings(), data.dimensions().dimensionsRegistryAccess());
        }, WorldStem::new, Util.backgroundExecutor(), executor)).get();
        log("[mctick] datapacks and registries loaded at " + msSince(t0) + " ms");

        BenchServer server = new BenchServer(Thread.currentThread(), access, packs, stem, OfflineServices.create(), view, sim);
        server.initServer();
        log("[mctick] levels loaded at " + msSince(t0) + " ms");

        // ---- join the save's player as a real login does once configuration ends (PrepareSpawnTask)
        UUID id = playerId(saves.resolve(levelName));
        GameProfile profile = new GameProfile(id, "Player");
        NameAndId nameAndId = new NameAndId(profile);
        server.owner = nameAndId;
        PrepareSpawnTask spawn = new PrepareSpawnTask(server, nameAndId);
        spawn.start(packet -> { });
        while (!spawn.tick()) {
            server.runTasks();
            Platform.drainBackground();
            Platform.idle();
        }
        NullConnection conn = new NullConnection();
        ClientInformation client = new ClientInformation("en_us", view, ChatVisiblity.FULL, true, 0, HumanoidArm.RIGHT,
                false, true, ParticleStatus.ALL);
        server.playerConnection = conn;
        ServerPlayer player = spawn.spawnPlayer(conn, new CommonListenerCookie(profile, 0, client, false));
        conn.fromClient(new ServerboundPlayerLoadedPacket());
        log("[mctick] player " + id + " in " + player.level().dimension().identifier() + " at "
                + String.format("%.1f %.1f %.1f", player.getX(), player.getY(), player.getZ()) + "; joined at " + msSince(t0) + " ms; "
                + "difficulty " + player.level().getDifficulty() + ", day time " + player.level().getDefaultClockTime());

        // ---- no wandering traders: WanderingTraderSpawner rolls at random every 1200 ticks once its delay runs out. On
        // ours one arrived (with two llamas) in 3 of ~60 runs through 2026-11-23 and on TeaVM in none of 39, and those
        // runs ticked ~15% slower, so the arms did not do the same work. Off for every arm.
        server.getGameRules().set(net.minecraft.world.level.gamerules.GameRules.SPAWN_WANDERING_TRADERS, false, server);
        log("[mctick] spawn_wandering_traders " + server.getGameRules().get(net.minecraft.world.level.gamerules.GameRules.SPAWN_WANDERING_TRADERS));

        // ---- warmup: ticks as the game runs them; report the world's state as it settles
        Ticker ticker = new Ticker(server, conn, player);
        long tw = System.nanoTime();
        for (int i = 1; i <= warmup; ++i) {
            ticker.tick();
            if (i % 500 == 0 || i == warmup) {
                log("[mctick] warmup tick " + i + "/" + warmup + ": " + state(server, conn) + "; "
                        + String.format("%.3f", (System.nanoTime() - tw) / 1e6 / i) + " ms/tick so far");
            }
        }
        log("[bench] warmup " + warmup + " ticks in " + msSince(tw) + " ms");

        // ---- the measured batches
        long best = Long.MAX_VALUE;
        long total = 0;
        for (int b = 1; b <= batches; ++b) {
            ticker.tickNanos = 0;
            ticker.backgroundNanos = 0;
            long start = System.nanoTime();
            for (int i = 0; i < batchTicks; ++i) {
                ticker.tick();
            }
            long nanos = System.nanoTime() - start;
            total += nanos;
            best = Math.min(best, nanos);
            log("[bench] batch " + b + "/" + batches + ": " + ms(nanos) + " ms (" + perTick(nanos, batchTicks)
                    + " ms/tick; tick-only " + perTick(ticker.tickNanos, batchTicks) + "; background "
                    + perTick(ticker.backgroundNanos, batchTicks) + ")");
        }
        log("[bench] mean " + perTick(total, batchTicks * batches) + " ms/tick over " + batchTicks * batches
                + " ticks; best batch " + perTick(best, batchTicks));
        log("[mctick] end state: " + state(server, conn));
        log("[mctick] entities by type: " + entityTypes(server));
        log("[mctick] chunk origins: " + chunkOrigins(server, player));
        log("[mctick] spawn state: " + spawnState(server));
        log("[mctick] entity places: " + entityPlaces(server, player));
        log("[mctick] " + Platform.backgroundStats() + "; ticks whose between-tick drain hit its cap " + Ticker.roundCapped);
        log("[bench] checksum " + checksum(server));
        Platform.exit(0);
    }

    /* The server thread's work for one game tick: the client's packets, the tick, then the tasks until the next. */
    static final class Ticker {
        final BenchServer server;
        final NullConnection conn;
        final ServerPlayer player;
        long tickNanos;
        long backgroundNanos;
        int n;
        static int roundCapped;

        Ticker(BenchServer server, NullConnection conn, ServerPlayer player) {
            this.server = server;
            this.conn = conn;
            this.player = player;
        }

        void tick() {
            // what a standing client sends: its tick end every tick, its position every 20 (LocalPlayer.sendPosition)
            conn.fromClient(ServerboundClientTickEndPacket.INSTANCE);
            if (++n % 20 == 0) {
                conn.fromClient(new ServerboundMovePlayerPacket.Pos(player.getX(), player.getY(), player.getZ(),
                        player.onGround(), player.horizontalCollision));
            }
            conn.deliver();
            long t = System.nanoTime();
            server.tickOnce(() -> true);
            tickNanos += System.nanoTime() - t;
            server.runTasks();
            // Between ticks, until neither side has work: on the JVM arms pool threads and the server's task queue
            // run concurrently (drainBackground returns 0); on TeaVM every background task can post a main-thread
            // task and back, and a single pass left each such hop to the next tick (chunk loading took ~3x the ticks).
            long b = System.nanoTime();
            int rounds = 0;
            while (Platform.drainBackground() > 0 && ++rounds < 64) {
                server.runTasks();
            }
            if (rounds == 64) {
                ++roundCapped;
            }
            backgroundNanos += System.nanoTime() - b;
        }
    }

    static String state(BenchServer server, NullConnection conn) {
        StringBuilder sb = new StringBuilder();
        for (ServerLevel level : server.getAllLevels()) {
            int entities = 0;
            for (Entity e : level.getAllEntities()) {
                ++entities;
            }
            sb.append(level.dimension().identifier().getPath()).append(' ').append(level.getChunkSource().getLoadedChunksCount())
                    .append(" chunks ").append(entities).append(" entities; ");
        }
        return sb.append("packets ").append(conn.packetsSent).append(", chunk packets ").append(conn.chunksSent).toString();
    }

    static String entityTypes(BenchServer server) {
        Map<String, Integer> byType = new TreeMap<>();
        for (ServerLevel level : server.getAllLevels()) {
            for (Entity e : level.getAllEntities()) {
                byType.merge(BuiltInRegistries.ENTITY_TYPE.getKey(e.getType()).getPath(), 1, Integer::sum);
            }
        }
        return byType.toString();
    }

    /* Chunks the overworld holds at FULL within 25 of the player: a chunk read from the save keeps its InhabitedTime,
     * one generated in this run starts at 0. "regenerated" are chunks the save holds at FULL whose time is now lower. */
    static final Map<Long, Long> DISK_FULL = new java.util.HashMap<>();

    static String chunkOrigins(BenchServer server, ServerPlayer player) {
        ServerLevel level = server.overworld();
        int cx = player.chunkPosition().x();
        int cz = player.chunkPosition().z();
        int loaded = 0;
        int fromDisk = 0;
        int regenerated = 0;
        int notOnDisk = 0;
        int[] entitiesLoaded = new int[26];
        int[] fullAt = new int[26];
        StringBuilder examples = new StringBuilder();
        for (int dz = -25; dz <= 25; ++dz) {
            for (int dx = -25; dx <= 25; ++dx) {
                net.minecraft.world.level.chunk.LevelChunk c = level.getChunkSource().getChunkNow(cx + dx, cz + dz);
                if (c == null) {
                    continue;
                }
                ++loaded;
                int d = Math.max(Math.abs(dx), Math.abs(dz));
                ++fullAt[d];
                if (level.areEntitiesLoaded(net.minecraft.world.level.ChunkPos.pack(cx + dx, cz + dz))) {
                    ++entitiesLoaded[d];
                }
                Long disk = DISK_FULL.get(net.minecraft.world.level.ChunkPos.pack(cx + dx, cz + dz));
                if (disk == null) {
                    ++notOnDisk;
                } else if (c.getInhabitedTime() < disk) {
                    if (regenerated++ < 6) {
                        examples.append(" [").append(cx + dx).append(", ").append(cz + dz).append("] disk ").append(disk)
                                .append(" now ").append(c.getInhabitedTime());
                    }
                } else {
                    ++fromDisk;
                }
            }
        }
        return loaded + " full chunks: " + fromDisk + " from the save, " + regenerated + " regenerated, " + notOnDisk
                + " not full in the save" + (examples.length() > 0 ? ";" + examples : "") + "; entities loaded by ring "
                + rings(entitiesLoaded, fullAt);
    }

    static String rings(int[] num, int[] den) {
        StringBuilder out = new StringBuilder();
        for (int d = 0; d < den.length; ++d) {
            if (den[d] > 0) {
                out.append(d == 0 ? "" : " ").append(d).append(':').append(num[d]).append('/').append(den[d]);
            }
        }
        return out.toString();
    }

    /* Where the entities stand, by type: "s" in a FULL chunk read from the save, "f" in a FULL chunk not full in the
     * save, "p" in a chunk not FULL now; and the chessboard distance range (in chunks) from the player. */
    static String entityPlaces(BenchServer server, ServerPlayer player) {
        ServerLevel level = server.overworld();
        int cx = player.chunkPosition().x();
        int cz = player.chunkPosition().z();
        Map<String, int[]> by = new TreeMap<>();
        for (net.minecraft.world.entity.Entity e : level.getAllEntities()) {
            String type = BuiltInRegistries.ENTITY_TYPE.getKey(e.getType()).getPath();
            int[] v = by.computeIfAbsent(type, k -> new int[] { 0, 0, 0, Integer.MAX_VALUE, 0 });
            int x = e.chunkPosition().x();
            int z = e.chunkPosition().z();
            if (level.getChunkSource().getChunkNow(x, z) == null) {
                ++v[2];
            } else if (DISK_FULL.containsKey(net.minecraft.world.level.ChunkPos.pack(x, z))) {
                ++v[0];
            } else {
                ++v[1];
            }
            int d = Math.max(Math.abs(x - cx), Math.abs(z - cz));
            v[3] = Math.min(v[3], d);
            v[4] = Math.max(v[4], d);
        }
        StringBuilder out = new StringBuilder();
        for (Map.Entry<String, int[]> en : by.entrySet()) {
            int[] v = en.getValue();
            out.append(' ').append(en.getKey()).append(" s").append(v[0]).append(" f").append(v[1]).append(" p")
                    .append(v[2]).append(" d").append(v[3]).append('-').append(v[4]);
        }
        return out.toString();
    }

    /* The natural spawner's last census (what the mob caps are checked against) beside our own count by category. */
    static String spawnState(BenchServer server) {
        ServerLevel level = server.overworld();
        net.minecraft.world.level.NaturalSpawner.SpawnState state = level.getChunkSource().getLastSpawnState();
        int[] ours = new int[net.minecraft.world.entity.MobCategory.values().length];
        for (net.minecraft.world.entity.Entity e : level.getAllEntities()) {
            ++ours[e.getType().getCategory().ordinal()];
        }
        StringBuilder out = new StringBuilder();
        if (state == null) {
            out.append("none");
        } else {
            out.append("spawnable chunks ").append(state.getSpawnableChunkCount()).append(";");
        }
        for (net.minecraft.world.entity.MobCategory c : net.minecraft.world.entity.MobCategory.values()) {
            out.append(' ').append(c.getName()).append(' ')
                    .append(state == null ? "-" : String.valueOf(state.getMobCategoryCounts().getInt(c))).append('/')
                    .append(ours[c.ordinal()]);
        }
        return out + " (spawner's count / all entities)";
    }

    /* Game time, loaded chunks, entities by type: order-independent and cheap; read after the window. */
    static long checksum(BenchServer server) {
        long h = 1125899906842597L;
        for (ServerLevel level : server.getAllLevels()) {
            h = h * 31 + level.getGameTime();
            h = h * 31 + level.getChunkSource().getLoadedChunksCount();
            Map<String, Integer> byType = new TreeMap<>();
            for (Entity e : level.getAllEntities()) {
                byType.merge(BuiltInRegistries.ENTITY_TYPE.getKey(e.getType()).toString(), 1, Integer::sum);
            }
            for (Map.Entry<String, Integer> e : byType.entrySet()) {
                h = h * 31 + e.getKey().hashCode();
                h = h * 31 + e.getValue();
            }
        }
        return h;
    }

    /* The save's own player: the one file in players/data (a singleplayer save has exactly one). */
    static UUID playerId(Path save) throws IOException {
        try (Stream<Path> s = Files.list(save.resolve("players").resolve("data"))) {
            List<String> ids = s.map(p -> p.getFileName().toString()).filter(n -> n.endsWith(".dat"))
                    .map(n -> n.substring(0, n.length() - 4)).sorted().toList();
            if (ids.size() != 1) {
                throw new IllegalStateException("expected one player in " + save + "/players/data, found " + ids);
            }
            return UUID.fromString(ids.get(0));
        }
    }

    /* Read every stored chunk of the overworld's region, entities and poi files through vanilla's RegionFile and
     * NbtIo: per file, chunks present, full (region only), unreadable, and a sum of the tags' hash codes, which must
     * agree between arms (it does not depend on identity hashes). */
    static String chunkCheck(Path save) {
        Path base = save.resolve("dimensions").resolve("minecraft").resolve("overworld");
        StringBuilder out = new StringBuilder();
        for (String kind : new String[] { "region", "entities", "poi" }) {
            Path dir = base.resolve(kind);
            List<Path> files;
            try (Stream<Path> s = Files.list(dir)) {
                files = s.filter(f -> f.getFileName().toString().endsWith(".mca")).sorted().toList();
            } catch (Throwable e) {
                out.append("\n  ").append(kind).append(": FAILED ").append(e);
                continue;
            }
            for (Path file : files) {
                String[] parts = file.getFileName().toString().split("\\.");
                int rx = Integer.parseInt(parts[1]);
                int rz = Integer.parseInt(parts[2]);
                out.append("\n  ").append(kind).append('/').append(file.getFileName()).append(": ");
                try (net.minecraft.world.level.chunk.storage.RegionFile region = new net.minecraft.world.level.chunk.storage.RegionFile(
                        new net.minecraft.world.level.chunk.storage.RegionStorageInfo("check",
                                net.minecraft.world.level.Level.OVERWORLD, kind.equals("region") ? "chunk" : kind),
                        file, dir, false)) {
                    int present = 0;
                    int full = 0;
                    int failed = 0;
                    long hash = 0;
                    String firstFailure = "";
                    for (int z = 0; z < 32; ++z) {
                        for (int x = 0; x < 32; ++x) {
                            net.minecraft.world.level.ChunkPos pos = new net.minecraft.world.level.ChunkPos(rx * 32 + x,
                                    rz * 32 + z);
                            if (!region.doesChunkExist(pos)) {
                                continue;
                            }
                            ++present;
                            try (java.io.DataInputStream in = region.getChunkDataInputStream(pos)) {
                                if (in == null) {
                                    if (failed++ == 0) {
                                        firstFailure = "; first failure " + pos + ": no stream";
                                    }
                                    continue;
                                }
                                net.minecraft.nbt.CompoundTag tag = net.minecraft.nbt.NbtIo.read(in);
                                if (tag.getStringOr("Status", "?").equals("minecraft:full")) {
                                    ++full;
                                    if (kind.equals("region")) {
                                        DISK_FULL.put(pos.pack(), tag.getLongOr("InhabitedTime", -1));
                                    }
                                }
                                hash += tag.hashCode() * 31L + present;
                            } catch (Throwable e) {
                                if (failed++ == 0) {
                                    firstFailure = "; first failure " + pos + ": " + e;
                                }
                            }
                        }
                    }
                    out.append(present).append(" present, ").append(full).append(" full, ").append(failed)
                            .append(" unreadable, hash ").append(Long.toHexString(hash)).append(firstFailure);
                } catch (Throwable e) {
                    out.append("FAILED ").append(e);
                }
            }
        }
        return out.toString();
    }

    /* The overworld's region files as copied: count, bytes, and whether a stream reads the same length back. */
    static String saveCheck(Path original, Path copy) throws IOException {
        Path region = copy.resolve("dimensions").resolve("minecraft").resolve("overworld").resolve("region");
        if (!Files.isDirectory(region)) {
            return "NO REGION DIRECTORY at " + region;
        }
        long files = 0;
        long bytes = 0;
        long streamed = 0;
        try (Stream<Path> s = Files.list(region)) {
            for (Path p : (Iterable<Path>) s::iterator) {
                ++files;
                bytes += Files.size(p);
                try (java.io.InputStream in = Files.newInputStream(p)) {
                    byte[] buf = new byte[65536];
                    int r;
                    while ((r = in.read(buf)) > 0) {
                        streamed += r;
                    }
                }
            }
        }
        return "region files " + files + ", " + bytes + " bytes (streamed " + streamed + ")";
    }

    static void copyTree(Path from, Path to) throws IOException {
        String root = from.toString();
        try (Stream<Path> s = Files.walk(from)) {
            for (Path p : (Iterable<Path>) s::iterator) {
                String rel = p.toString().substring(root.length());
                while (rel.startsWith("/") || rel.startsWith("\\")) {
                    rel = rel.substring(1);
                }
                Path dst = rel.isEmpty() ? to : to.resolve(rel);
                if (Files.isDirectory(p)) {
                    Files.createDirectories(dst);
                } else if (!p.getFileName().toString().equals("session.lock")) {
                    Files.copy(p, dst);
                }
            }
        }
    }

    static void log(String line) {
        Platform.out(line);
    }

    private static long msSince(long t0) {
        return (System.nanoTime() - t0) / 1000000L;
    }

    private static String ms(long nanos) {
        return String.valueOf(nanos / 1000000.0);
    }

    private static String perTick(long nanos, int ticks) {
        return String.valueOf(nanos / 1000000.0 / ticks);
    }

    private static int intArg(String[] args, int index, String property, int fallback) {
        if (args != null && index < args.length) {
            try {
                return Integer.parseInt(args[index]);
            } catch (NumberFormatException e) {
                return fallback;
            }
        }
        return Integer.getInteger(property, fallback);
    }
}
