/*
 *  Copyright 2024 Alexey Andreev.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
package org.teavm.samples.benchmark.teavm;

import org.teavm.samples.benchmark.shared.Scene;

/*
 * The jbox2d fixed-work driver for TeaVM's LINEAR-MEMORY wasm backend (the TeaVM arms of wj j2d/run).
 *
 * TeaVM's JSO is js/wasm-gc only, so this uses nothing but System.nanoTime() and System.out, which the legacy backend
 * maps to its own teavm.nanoTime / teavm.logString imports. The lines it prints are IkvmBenchmarkStarter's, so every
 * arm is read by the same parser.
 *
 * The schedule is main's positional arguments [warmupSteps] [batchSteps] [batches], which web/teavm-wasm-fixed.html
 * passes from its ?args=W,S,N query (j2d/run --schedule). Until 2026-11-18 the batch count was compiled in, which is
 * why state/arms holds teavm-{full,simple}60 beside the 12-batch builds.
 */
public final class WasmFixedWorkBenchmarkStarter {
    private WasmFixedWorkBenchmarkStarter() {
    }

    public static void main(String[] args) {
        int warmupSteps = intArg(args, 0, 600);
        int batchSteps = intArg(args, 1, 200);
        int batches = intArg(args, 2, 12);
        Scene scene = new Scene();
        log("[bench] fixed-work: warmup=" + warmupSteps + " steps, then " + batches
                + " batches of " + batchSteps + " steps (dt=0.01, 20 velocity / 40 position iterations)");

        long nanos = timeSteps(scene, warmupSteps);
        log("[bench] warmup " + warmupSteps + " steps in " + ms(nanos) + " ms ("
                + msPerStep(nanos, warmupSteps) + " ms/step)");

        long best = Long.MAX_VALUE;
        long total = 0;
        for (int batch = 1; batch <= batches; ++batch) {
            nanos = timeSteps(scene, batchSteps);
            total += nanos;
            if (nanos < best) {
                best = nanos;
            }
            log("[bench] batch " + batch + "/" + batches + ": " + ms(nanos) + " ms ("
                    + msPerStep(nanos, batchSteps) + " ms/step)");
        }

        int steps = batches * batchSteps;
        log("[bench] mean " + msPerStep(total, steps) + " ms/step over " + steps
                + " steps; best batch " + msPerStep(best, batchSteps) + " ms/step");
        log("[bench] checksum " + scene.checksum());
    }

    private static int intArg(String[] args, int index, int fallback) {
        if (args == null || index >= args.length) {
            return fallback;
        }
        try {
            return Integer.parseInt(args[index]);
        } catch (NumberFormatException e) {
            return fallback;
        }
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

    private static void log(String line) {
        System.out.println(line);
    }
}
