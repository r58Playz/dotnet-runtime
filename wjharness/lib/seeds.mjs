// The seed registry: one entry per (Minecraft version, mod set, save) the harness can boot.
//
// A seed is a browser profile whose OPFS already holds the install, the mods, options.txt and a save
// (seedbuild.mjs makes one from an OPFS tar). Since the 2026-10-21 launcher redesign the app no longer
// starts on page load: it shows a launcher (Minecraft version, Fabric Loader version, Play) and boots the
// runtime only when Play is pressed. So a seed has to say which version to type, and every version/mod set
// has its own log lines for "the title screen is up" and its own keyboard path into the save.
//
// Pick one with `--seed NAME` on a driver or `MC_SEED=NAME` (a name below, or a profile directory). The
// default stays `mcsr`, so every driver that predates this file keeps measuring what it always measured.
//
// Per entry:
//   dir         profile directory under $WJ_HOME/state/seeds
//   version     typed into the launcher's "Minecraft version" field
//   fabric      typed into "Fabric Loader version"; the pair names ikvmcraft/versions/<version>-fabric-<fabric>
//   titleRx     the line that means the title screen is up and drawn (marker `titleReady`)
//   joinRx      the line that means the client is in the world (marker `inworld`)
//   genRx       the first line of spawn-area preparation (marker `generating`)
//   menu        [code, key, waitMs] presses from the settled title screen into the save
//   quitTabs    Tab presses from the pause menu to "Save and Quit"; null = not measured for this seed, the
//               driver skips save+quit rather than pressing into an unknown menu (probe: MC_QUIT_PROBE_DIR)
//   defuseRankedAuth  pass IKVM_DEFUSE_RANKED_AUTH=1 (MCSR Ranked only; it changes what the game does)
//   benignRx    lines this mod set logs on every healthy boot that the fault classifier would otherwise count
//               (each read off a real log, with the reason); they are skipped before classification
import { existsSync } from "fs";
import { resolve, join, basename } from "path";
import { SEEDS_DIR } from "./paths.mjs";

/* Seed profiles live in $WJ_HOME/state/seeds (lib/paths.mjs). Kept under its old name: every driver imports it. */
export const MCSR_DIR = SEEDS_DIR();

/* 26.2, each caught and logged, boot continues (mc-out/bc262-2.log, boot/c262a/game.log):
 *   SystemReport.ignoreErrors catches oshi's hardware probe failing in a browser (no udev, no JNA, Auxv) -- "Failed
 *   retrieving info for group ..." with a NoClassDefFoundError; Fabric's Hooks.startClient setting the system look and
 *   feel fails in headless AWT (XToolkit -> GraphicsEnvironment: "Could not initialize class LocalGE"); Controlify's
 *   SDLNativesLoader finds no SDL3 and runs without controllers. Without mods (262n, 2026-11-06) Minecraft's own
 *   narrator also fails, caught: text2speech's NarratorLinux loads flite through JNA, whose Native cannot initialise in a
 *   browser ("Could not initialize class com.sun.jna.Native" from NativeLibrary.<clinit>; native logs "Error while
 *   loading the narrator" the same way). */
const BENIGN_262 = /Could not initialize class oshi\.|at oshi\.|Could not initialize class LocalGE|no SDL3 in java\.library\.path|Could not initialize class com\.sun\.jna\.Native|at com\.sun\.jna\.|text2speech/;
const keys = (s, waits = {}) => s.split(/\s+/).filter(Boolean).map((k) => [k, k, waits[k] ?? (k === "Enter" ? 2500 : 900)]);

/* Read off screenshots after every key (vcmp R509 for 1.16.5 and 26.2; autoworld.sh for MCSR). Keyboard only:
 * mouse buttons never reach Minecraft through emscripten-glfw headless, focus moves with Tab, Enter activates. */
export const SEEDS = {
  mcsr: {
    dir: "seed", version: "1.16.1", fabric: "0.19.3",
    what: "1.16.1 + Fabric, MCSR Ranked + Sodium/Lithium; 'New World' pre-generated to the 83% knee",
    titleRx: /FAILED_AUTH/,
    joinRx: /StateOutput State: inworld/,
    genRx: /StateOutput State: generating/,
    /* Tab,Enter dismisses MCSR Ranked's "authentication data couldn't be loaded" dialog (Escape does not);
     * Tab,Enter = Singleplayer; Tab,Down selects "New World"; Tab,Enter = Play Selected World. */
    menu: [
      ["Tab", "Tab", 700], ["Enter", "Enter", 2500],
      ["Tab", "Tab", 700], ["Enter", "Enter", 3000],
      ["Tab", "Tab", 700], ["ArrowDown", "ArrowDown", 1000],
      ["Tab", "Tab", 700], ["Enter", "Enter", 2000],
    ],
    quitTabs: 8,
    defuseRankedAuth: true,
  },
  165: {
    dir: "seed-165", version: "1.16.5", fabric: "0.19.5",
    what: "1.16.5 + Fabric, 10 mods (Sodium 0.2, Iris 1.4.5 with BSL, Lithium, ModernFix...), RD 12",
    /* ModernFix's measure_time line; RRLS's as a fallback */
    titleRx: /Game took [\d.]+ seconds to start|Overlay is removed!/,
    joinRx: /joined the game/,
    genRx: /Preparing start region for dimension|Preparing spawn area: \d+%/,
    menu: keys("Tab Enter Tab ArrowDown Tab Enter", { Enter: 3000 }),
    quitTabs: null,
  },
  "262u": {
    dir: "seed-262u", version: "26.2", fabric: "0.19.5",
    what: "26.2 + Fabric, Fabulously Optimized (149 mods), RD 2 (R509-R513)",
    /* Fabulously Optimized turns ModernFix's measure_time off; RRLS logs this when the overlay leaves */
    titleRx: /Game took [\d.]+ seconds to start|Overlay is removed!/,
    joinRx: /joined the game/,
    genRx: /Preparing start region for dimension|Preparing spawn area: \d+%/,
    menu: keys("Tab Enter Tab Tab Enter", { Enter: 3500 }),
    quitTabs: null,
    benignRx: BENIGN_262,
  },
  "262b": {
    dir: "seed-262b", version: "26.2", fabric: "0.19.5",
    what: "26.2 + Fabric, Fabulously Optimized 14.1.1 trimmed to 43 mods (+Iris, Lithium, ModernFix), RD 12, survival/normal (2026-10-21 tar)",
    titleRx: /Game took [\d.]+ seconds to start|Overlay is removed!/,
    joinRx: /joined the game/,
    genRx: /Preparing start region for dimension|Preparing spawn area: \d+%/,
    menu: keys("Tab Enter Tab Tab Enter", { Enter: 3500 }),
    quitTabs: null,
    benignRx: BENIGN_262,
  },
  /* 26.2 with NO mods: Fabric 0.19.5 alone (the app launches <version>-fabric-<loader> only), the 262b save set to
   * peaceful, the vanilla resource pack, vsync off. Built 2026-11-06 from the 2026-10-21 tar by
   * runs/2026-11-06-262/mktar.py into state/seeds/ikvmcraft-262n.tar; mcstage builds the native stage `sa262n` from the
   * same tar. Vanilla logs no "title screen is up" line: the marker is the last atlas the 26.2 reload creates (native
   * log, 2026-11-06), and the menu re-drive covers a press that lands under the fading overlay. */
  "262n": {
    dir: "seed-262n", version: "26.2", fabric: "0.19.5",
    what: "26.2 + Fabric 0.19.5, NO mods; the 262b save set to peaceful, RD 12 (2026-11-06)",
    titleRx: /Created: \d+x\d+x\d+ minecraft:textures\/atlas\/shulker_boxes\.png-atlas/,
    joinRx: /joined the game/,
    genRx: /Preparing start region for dimension|Preparing spawn area: \d+%/,
    menu: keys("Tab Enter Tab Tab Enter", { Enter: 3500 }),
    quitTabs: null,
    benignRx: BENIGN_262,
  },
  /* 262n plus one diagnostic mod (runs/2026-11-06-262/diag: wjdiag, mixins into MappedRegistry.freeze and
   * FileToIdConverter.listMatchingResources) for 26.2's registry race. Not a measurement seed. */
  "262d": {
    dir: "seed-262d", version: "26.2", fabric: "0.19.5",
    what: "262n + the wjdiag registry-race diagnostic mod (not for measurement)",
    titleRx: /Created: \d+x\d+x\d+ minecraft:textures\/atlas\/shulker_boxes\.png-atlas/,
    joinRx: /joined the game/,
    genRx: /Preparing start region for dimension|Preparing spawn area: \d+%/,
    menu: keys("Tab Enter Tab Tab Enter", { Enter: 3500 }),
    quitTabs: null,
    benignRx: BENIGN_262,
  },
  /* 262b with the save's difficulty set to peaceful (level.dat Data/difficulty_settings/difficulty, rewritten by an NBT
   * round trip that changes only that string; scratchpad/wj/nbtedit.py, 2026-10-21). The save spawns at night among zombies,
   * so a walking window on 262b can end on the death screen; use this one for repeatable in-game measurement. */
  "262p": {
    dir: "seed-262p", version: "26.2", fabric: "0.19.5",
    what: "262b with the save set to peaceful (no hostile mobs), for in-game windows",
    titleRx: /Game took [\d.]+ seconds to start|Overlay is removed!/,
    joinRx: /joined the game/,
    genRx: /Preparing start region for dimension|Preparing spawn area: \d+%/,
    menu: keys("Tab Enter Tab Tab Enter", { Enter: 3500 }),
    quitTabs: null,
    benignRx: BENIGN_262,
  },
};

/**
 * A seed definition from a registry name, a profile directory, or an already-resolved definition.
 * A directory that is no registry entry's gets the generic modern markers and no menu: it can boot to the
 * title, and a caller that wants a world must pass its own menu.
 */
export function resolveSeed (seed = process.env.MC_SEED || "mcsr") {
  if (seed && typeof seed === "object") return seed;
  const name = String(seed);
  if (SEEDS[name]) return { name, ...SEEDS[name], dir: join(MCSR_DIR, SEEDS[name].dir) };
  const dir = resolve(name);
  const hit = Object.entries(SEEDS).find(([, s]) => join(MCSR_DIR, s.dir) === dir);
  if (hit) return { name: hit[0], ...hit[1], dir };
  if (!existsSync(dir)) throw new Error(`unknown seed '${name}': not one of ${Object.keys(SEEDS).join(", ")} and not a directory`);
  return {
    name: basename(dir), dir, version: null, fabric: null, what: "(unregistered profile directory)",
    titleRx: /Game took [\d.]+ seconds to start|Overlay is removed!|FAILED_AUTH/,
    joinRx: /joined the game|StateOutput State: inworld/,
    genRx: /Preparing start region for dimension|Preparing spawn area: \d+%|StateOutput State: generating/,
    menu: null, quitTabs: null,
  };
}

/* `--seed X` from an argv, else MC_SEED, else mcsr. */
export function seedFromArgs (args) {
  const i = args.indexOf("--seed");
  return resolveSeed(i >= 0 ? args[i + 1] : undefined);
}
