"""feclass.py -- what the SERVER thread is doing in one sample, from the JIT function names on its call chain.

Minecraft 1.16.1's server loop (MinecraftServer.runServer) alternates two things (javap of client-intermediary.jar):
  tick(BooleanSupplier)          MinecraftServer.method_3748 -- the tick's work
  waitUntilNextTick()            MinecraftServer.method_16208 -> managedBlock(() -> !haveTime())  (class_1255.method_18857)
      managedBlock loops:          pollTask() -- MinecraftServer.method_20415 (pollTaskInternal) -> class_1255.method_16075
                                     -> doRunTask (class_1255.method_18859) runs ONE queued task (real work)
                                   else waitForTasks() -- class_1255.method_20813 = Thread.yield + LockSupport.parkNanos(100 us)
                                   and every pass reads the clock through haveTime -> the GLX time supplier -> glfwGetTime.

So a sample is, in this priority order:
  'tick work'  its chain reaches tick()                                   (TICK)
  'tasks'      it is inside a queued task run between ticks                 (TASK)
  'idle wait'  it is the wait loop's own machinery: managedBlock, pollTask, the !haveTime lambda, waitForTasks, parkNanos
  'other'      none of these (runServer's own code, autosave scheduling, broken chains)

Before 2026-11-05 fedoor/fepart called everything that was not under waitForTasks / parkNanos "tick work", which put the
wait loop's ~485 clock reads a tick (pollTaskInternal's and the lambda's haveTime) into tick work: R525's "JNI.invokeD =
15% of tick-work L1i misses" was that misclassification. The 2026-11-05 site census counts ~20 tick-work clock reads a
tick against ~485 idle ones. Names: IKVM nests and numbers lambda classes (`MinecraftServer/__<>AnonN:getAsBoolean`);
the frame names are jitdump names, `<ret> <ns>.<Class>:<method> (<args>)`, hence the trailing space."""
import re

TICK = re.compile(r"MinecraftServer:method_3748 ")
TASK = re.compile(r"class_1255:method_18859 ")
IDLE = re.compile(r"class_1255:method_20813 |class_1255:method_18857 |LockSupport:parkNanos |"
                  r"MinecraftServer:method_20415 |MinecraftServer:method_16208 |MinecraftServer/__<>Anon\d+:getAsBoolean")
PARTS = ("tick work", "tasks", "idle wait", "other")


def classify(names):
    """names: the function names on one sample's chain (leaf first; None for frames that are not JIT functions)."""
    names = [n for n in names if n]
    if any(TICK.search(n) for n in names):
        return "tick work"
    if any(TASK.search(n) for n in names):
        return "tasks"
    if any(IDLE.search(n) for n in names):
        return "idle wait"
    return "other"
