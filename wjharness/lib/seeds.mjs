// The seed registry: one entry per (Minecraft version, mod set, save) the harness can boot.
//
// A seed is a browser profile whose OPFS already holds the install, the mods, options.txt and a save
// (seedbuild.mjs makes one from an OPFS tar). Since the 2026-10-21 launcher redesign the app no longer
// starts on page load: it shows a launcher (Minecraft version, Fabric Loader version, Play) and boots the
// runtime only when Play is pressed. So a seed has to say which version to type, and every version/mod set
// has its own log lines for "the title screen is up" and its own keyboard path into the save.
//
// Pick one with `--seed NAME` on a driver or `MC_SEED=NAME` (a name below, or a profile directory). The
// default is `262n`, the performance target (2026-11-08; it was `mcsr` until then, so a run directory from before
// that date without `--seed` measured 1.16.1). 26.2 has ONE seed: the modded packs (262u, 262b, 262p) and the
// registry-race diagnostic (262d) were retired on 2026-11-08 to $WJ_STATE/retired/2026-11-08 (findings log).
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
export const DEFAULT_SEED = "262n";

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
  /* THE DEFAULT and the performance target. 26.2 with NO mods: Fabric 0.19.5 alone (the app launches
   * <version>-fabric-<loader> only), the vanilla resource pack, vsync off, RD 12, and the user's 2026-10-21 "New World"
   * save (from the retired modded pack 262b) with its difficulty set to PEACEFUL (level.dat
   * Data/difficulty_settings/difficulty, by drive/nbtedit.py). The save spawns at night, so a normal
   * difficulty would put zombies on the player and could end a walking window on the death screen. Built 2026-11-06
   * from the 2026-10-21 tar by runs/2026-11-06-262/mktar.py into state/seeds/ikvmcraft-262n.tar; mcstage builds the
   * native stage `sa262n` from the same tar (its level.dat reads peaceful too). Vanilla logs no "title screen is up"
   * line: the marker is the last atlas the 26.2 reload creates (native log, 2026-11-06), and the menu re-drive covers
   * a press that lands under the fading overlay. */
  "262n": {
    dir: "seed-262n", version: "26.2", fabric: "0.19.5",
    what: "26.2 + Fabric 0.19.5, NO mods, the save set to peaceful, RD 12 (2026-11-06): the performance target",
    titleRx: /Created: \d+x\d+x\d+ minecraft:textures\/atlas\/shulker_boxes\.png-atlas/,
    joinRx: /joined the game/,
    genRx: /Preparing start region for dimension|Preparing spawn area: \d+%/,
    menu: keys("Tab Enter Tab Tab Enter", { Enter: 3500 }),
    quitTabs: null,
    benignRx: BENIGN_262,
  },
  /* 262n's save in CREATIVE with commands on (2026-11-08, the memory census): the player flies, and the save's datapack
   * `memwalk` teleports it along +X, 32 blocks a step, at most one step a second, and only once the server has the
   * chunks 9 ahead (and 7 ahead x 7 to each side) entity-ticking; it starts 1200 ticks after load and logs
   * `[memwalk] step N` as chat every 8 steps. So every run, ours or native, loads chunks continuously along the same
   * path at the rate its own server generates them. options.txt has pauseOnLostFocus:false. Built by
   * runs/2026-11-08-memwalk/mk262c.py from ikvmcraft-262n.tar into state/seeds/ikvmcraft-262c.tar; native stage `sa262c`.
   * Not a performance seed: the teleport makes chunk generation the workload. */
  "262c": {
    dir: "seed-262c", version: "26.2", fabric: "0.19.5",
    what: "26.2 + Fabric 0.19.5, no mods, creative + commands, datapack teleport along +X (2026-11-08): continuous chunk loading",
    titleRx: /Created: \d+x\d+x\d+ minecraft:textures\/atlas\/shulker_boxes\.png-atlas/,
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
export function resolveSeed (seed = process.env.MC_SEED || DEFAULT_SEED) {
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

/* `--seed X` from an argv, else MC_SEED, else DEFAULT_SEED. */
export function seedFromArgs (args) {
  const i = args.indexOf("--seed");
  return resolveSeed(i >= 0 ? args[i + 1] : undefined);
}
