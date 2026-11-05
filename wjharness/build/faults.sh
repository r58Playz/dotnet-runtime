# Shared fault signatures. Sourced by exp.sh / menu.sh / autoworld.sh so they cannot drift apart.
#
# Deliberately does NOT include "JSPI mailbox export REJECTED". That message is emitted with several
# different payloads and only some are fatal:
#
#   ": RuntimeError: memory access out of bounds"  -> real, a wild load/store killed the thread
#   ": [MONO] * Assertion at ..."                  -> real
#   ": [object Object]"                            -> seen alongside real faults
#   ": unwind"                                     -> BENIGN. `unwind` is emscripten's normal
#                                                     stack-unwind sentinel (the keep-the-runtime-alive
#                                                     idiom), not a trap. Runs sail straight past it.
#
# Treating the bare message as fatal aborted a perfectly healthy run ("FAILED before entering the world"
# while `Preparing start region` appeared moments later). An audit of all 27 runs to date showed every
# genuine failure also emitted one of the markers below, and that `unwind` shows up in the healthy part
# of those runs too — so the specific markers are strictly better evidence than the wrapper message.
# Additions after a run sat for 15 minutes looking idle. The set above misses whole failure modes, and a
# missed failure mode is worse than a false positive here: the run burns its entire WORLD_WAIT/soak budget
# and reports "never reached inworld" or "survived", so it is scored as a stall or a PASS.
#
#   lost connection: / Stopping singleplayer server
#       The silent disconnect. The client drops from its own integrated server, the server shuts itself
#       down ("Stopping singleplayer server as player logged out"), and NOTHING in the old set matches --
#       no crash report, no assertion. This is a real in-world failure and has to count as one.
#       Safe against the normal end of a run: autoworld only tests FAIL_RX while waiting for / soaking in
#       the world, and an orderly shutdown happens after that.
#
#   Unable to cast object of type / cannot be cast to
#       The SAME ClassCastException as Arg_InvalidCastException, but reworded by MONO_DEBUG=casts and by
#       IKVM_VERBOSE_CAST. Instrumenting the failure renamed it out of the fault set, so exactly the arms
#       aimed at identifying it were the ones that could no longer detect it.
#
#   Failed to light chunk / Exception ticking world / Lock is no longer valid
#       Direct markers, rather than relying on "Game crashed!" appearing afterwards.
#   GLFW error
#       NOT a runtime fault. "GLFW error 65544: Cannot create GL context for [#canvas]" is the browser
#       refusing a WebGL context -- an ENVIRONMENT failure (GPU state, driver, too many chromium launches
#       back to back), and it kills the game before the title screen so nothing about the variable under
#       test is exercised. It scored 2/2 "failures" for a control arm that had been 0/4, which for a few
#       minutes looked like a real regression in the runtime. Kept in the set because a run that cannot
#       render is still a run that must not be counted as a PASS -- but callers have to distinguish it:
#       check which signature actually matched before attributing a failure to the change under test.
#
#   OOM_RX (below) -- renderer death from the BROWSER's side, not the game's.
#       Five renderer crashes were investigated as an RLIMIT_DATA problem before anyone read the actual
#       message; they all died on SIGILL with 6.9-10.4 GiB of RLIMIT_DATA headroom. The message was there
#       the whole time, in exactly the stream chrome.sh already captures:
#
#         V8 javascript OOM (CALL_AND_RETRY_LAST).   [v8_initializer.cc -> OOM_CRASH(0)]
#
#       Nothing grepped for it, so the harness scored these as FREEZE/STALL/"never reached inworld" and
#       the trajectory was attributed to whichever bucket happened to be largest. Kept SEPARATE from the
#       game-fault set because the attribution is different in kind: a match here means the browser ran
#       out of some internal resource, so the arm tested nothing about the runtime change -- the same
#       reasoning as GLFW error above. Check which set matched before blaming the variable under test.
OOM_RX="V8 javascript OOM|Fatal JavaScript out of memory|Exceeding maximum wasm|Failed to reserve memory|Check failed: .*[Aa]llocat|PartitionAlloc.*(OOM|out of memory)|Received signal 4|SIGILL"

# systemd-oomd, which is the HOST running out of memory -- a different thing entirely from the renderer
# exhausting an internal V8 limit, and not detectable in the log at all.
#
# This box is 15 GiB of RAM with ~30 GiB of zram, so paging costs physical memory rather than freeing it
# and PSI climbs fast; oomd here is DefaultMemoryPressureLimit=40% over 15s and has already reaped an
# entire terminal scope (191 processes) mid-session. A renderer killed this way leaves NO V8 message and
# NO core -- SIGKILL is not catchable -- so it presents as "the renderer just vanished", which is exactly
# what the OOM_RX crashes look like from the outside while meaning the opposite. Always check this before
# attributing a disappearance to the runtime under test.
#
# usage: oomd_kills_since <epoch-seconds>   -> prints matching journal lines, exit 0 if any were found
oomd_kills_since() {
	journalctl --user --since "@${1:-0}" --no-pager 2>/dev/null | grep -i "systemd-oomd killed"
}

FAIL_RX="unexpected GC filler class|memory access out of bounds|Arg_InvalidCastException|Unable to cast object of type|cannot be cast to|Failed to light chunk|Exception ticking world|Lock is no longer valid|lost connection:|Stopping singleplayer server|Game crashed!|forcing abort|assert_not_reached|Assertion at|GLFW error|MD5 MessageDigest|$OOM_RX"
