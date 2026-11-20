package org.teavm.samples.benchmark.ikvm;

import org.teavm.samples.benchmark.shared.Scene;

/*
 * Two drivers:
 *
 *   fixed (default) -- run a fixed number of world.step() calls as fast as possible and report
 *       ms/step. This is the one to compare against TeaVM, because it is unclipped: the number
 *       goes down when the runtime gets faster.
 *
 *   realtime (-Dbench.mode=realtime) -- the original driver, kept as a "can it hold 100 Hz?"
 *       check. It reports CPU-ms per wall-clock second, which is bounded by ~1000: a runtime that
 *       cannot sustain 100 steps/s silently runs fewer steps and still prints ~900-1000, so it
 *       cannot show a speedup.
 *
 * The browser path (IkvmWasm.RunJar) invokes main() from the jar manifest with no args, so the
 * defaults have to be the useful ones. Desktop runs can override positionally:
 *   java -jar bench.jar [warmupSteps] [batchSteps] [batches]
 * and the browser through Java system properties bench.warmup / bench.batchSteps / bench.batches, which the
 * ikvmcraft page sets from ?jprop.<name>=<value> (a missing positional arg falls back to the property, then to the
 * default). Used by scratchpad/wj/j2d/run.mjs --schedule for the steady-state window (R416).
 */
public class IkvmBenchmarkStarter {
    private static final int DEFAULT_WARMUP_STEPS = 600;
    private static final int DEFAULT_BATCH_STEPS = 200;
    private static final int DEFAULT_BATCHES = 12;

    public static void main(String[] args) throws InterruptedException {
        if ("realtime".equals(System.getProperty("bench.mode"))) {
            runRealtime();
        } else {
            runFixed(intArg(args, 0, "bench.warmup", DEFAULT_WARMUP_STEPS),
                    intArg(args, 1, "bench.batchSteps", DEFAULT_BATCH_STEPS),
                    intArg(args, 2, "bench.batches", DEFAULT_BATCHES));
        }
    }

    private static void runFixed(int warmupSteps, int batchSteps, int batches) {
        Scene scene = new Scene();
        System.out.println("[bench] fixed-work: warmup=" + warmupSteps + " steps, then " + batches
                + " batches of " + batchSteps + " steps (dt=0.01, 20 velocity / 40 position iterations)");

        // Warmup is a real part of the measurement story here, not noise to hide: the wasm JIT
        // only promotes a method after MONO_WASM_JIT_THRESHOLD calls, and world.step() is called
        // once per step, so the top-level method needs several hundred steps before it exists as
        // wasm at all. Report it rather than discarding it silently.
        long warmupNanos = timeSteps(scene, warmupSteps);
        System.out.println("[bench] warmup " + warmupSteps + " steps in " + ms(warmupNanos) + " ms ("
                + msPerStep(warmupNanos, warmupSteps) + " ms/step)");

        long best = Long.MAX_VALUE;
        long total = 0;
        for (int i = 1; i <= batches; ++i) {
            long nanos = timeSteps(scene, batchSteps);
            total += nanos;
            if (nanos < best) {
                best = nanos;
            }
            System.out.println("[bench] batch " + i + "/" + batches + ": " + ms(nanos) + " ms ("
                    + msPerStep(nanos, batchSteps) + " ms/step)");
        }

        int measuredSteps = batchSteps * batches;
        System.out.println("[bench] mean " + msPerStep(total, measuredSteps) + " ms/step over "
                + measuredSteps + " steps; best batch " + msPerStep(best, batchSteps) + " ms/step");
        // Steady state is what to compare: the mean still carries whatever warmup leaked past the
        // warmup phase, and the scene keeps settling for the first few thousand steps.
        System.out.println("[bench] checksum " + scene.checksum());
    }

    private static long timeSteps(Scene scene, int steps) {
        long start = System.nanoTime();
        scene.stepFixed(steps);
        return System.nanoTime() - start;
    }

    private static String ms(long nanos) {
        return String.valueOf(nanos / 1000000.0);
    }

    private static String msPerStep(long nanos, int steps) {
        return String.valueOf(nanos / 1000000.0 / steps);
    }

    private static int intArg(String[] args, int index, String property, int fallback) {
        if (args == null || index >= args.length) {
            return Integer.getInteger(property, fallback);
        }
        try {
            return Integer.parseInt(args[index]);
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static void runRealtime() throws InterruptedException {
        Scene scene = new Scene();
        int currentSecond = 0;
        long startMillisecond = System.currentTimeMillis();
        double timeSpentCalculating = 0;
        while (true) {
            long start = System.nanoTime();
            scene.calculate();
            long end = System.nanoTime();

            int second = (int) ((System.currentTimeMillis() - startMillisecond) / 1000);
            if (second > currentSecond) {
                System.out.println("Computing second " + second + " took "
                        + (long) (timeSpentCalculating / 1000000) + " ms");
                currentSecond = second;
                timeSpentCalculating = 0;
            }
            timeSpentCalculating += end - start;

            int sleepTime = scene.timeUntilNextStep();
            if (sleepTime > 0) {
                Thread.sleep(sleepTime);
            }
        }
    }
}
