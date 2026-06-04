// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

import type { GlobalObjects } from "../types/internal";
import type { CharPtr, VoidPtr } from "../types/emscripten";

import { Module, runtimeHelpers } from "./globals";
import { cleanupClient as cleanup_js_client, createDiagConnectionJs, serverSession, stopActiveSession } from "./diagnostics-js";
import { IDiagnosticConnection } from "./common";
import { createDiagConnectionWs } from "./diagnostics-ws";
import { diagnosticHelpers, setRuntimeGlobalsImpl } from "./globals";
import { collectCpuSamples } from "./dotnet-cpu-profiler";
import { collectMetrics } from "./dotnet-counters";
import { collectGcDump } from "./dotnet-gcdump";
import { advertise } from "./client-commands";
import { mono_log_warn } from "./logging";

let socket_handles:Map<number, IDiagnosticConnection> = undefined as any;
let next_socket_handle = 1;
let url_override:string | undefined = undefined;

// On-demand CPU-trace bridge. The DS server (and serverSession) live on this worker thread,
// which can't be reached by emscripten_dispatch while it's parked in g_usleep. So we service
// requests from the DS poll loop itself: each ds_rt_websocket_poll the worker checks a shared
// control block (app C: diag_trace_*), and if the UI thread asked for a trace, runs
// collectCpuSamples here and hands the .nettrace bytes back through shared linear memory.
let diagTraceBusy = false;
let traceDeadline = 0; // performance.now() ms at which the active collection must stop; 0 = none
let traceStopped = false;
function maybeStartRequestedTrace () {
    const M = Module as any;
    if (diagTraceBusy || typeof M._diag_trace_take_request !== "function") return;
    const ms = M._diag_trace_take_request() | 0;
    if (ms <= 0) return;
    diagTraceBusy = true;
    traceStopped = false;
    traceDeadline = performance.now() + ms;
    let promise:Promise<Uint8Array[]>;
    try {
        if (M._diag_set_step) M._diag_set_step(1);
        promise = collectCpuSamples({ durationSeconds: ms / 1000, skipDownload: true });
    } catch (e:any) {
        mono_log_warn("[diag] cpu trace request failed to start: " + (e?.message || e));
        M._diag_stream_mark_done?.();
        diagTraceBusy = false;
        traceDeadline = 0;
        return;
    }
    promise.then(() => {
        // the .nettrace bytes were streamed cross-worker into the shared accumulator
        // (diag_stream_append); just signal the harness that the session has closed.
        if (M._diag_set_step) M._diag_set_step(9);
        M._diag_stream_mark_done?.();
    }).catch((e:any) => {
        if (M._diag_set_step) M._diag_set_step(90);
        mono_log_warn("[diag] cpu trace failed: " + (e?.message || e));
        M._diag_stream_mark_done?.();
    }).finally(() => {
        diagTraceBusy = false;
        traceDeadline = 0;
    });
}

// collectCpuSamples schedules its stop via safeSetTimeout on this (DS-server) worker, but the
// worker never returns to its JS event loop (native poll loop + GC-safe g_usleep), so that timer
// never fires. Drive the stop from the poll loop instead: once the requested duration elapses
// and the session has a session_id, send the stop command (queued for the poll loop to deliver).
function maybeStopRequestedTrace () {
    if (!traceDeadline || traceStopped) return;
    if (performance.now() < traceDeadline) return;
    (Module as any)._diag_set_step2?.(49);
    try {
        // targets the captured active session_id over any connection (serverSession may have
        // been reassigned to a later advert with no session_id yet)
        if (stopActiveSession()) traceStopped = true;
    } catch (e:any) {
        mono_log_warn("[diag] cpu trace stop failed: " + (e?.message || e));
    }
}

export function setRuntimeGlobals (globalObjects: GlobalObjects): void {
    setRuntimeGlobalsImpl(globalObjects);

    diagnosticHelpers.ds_rt_websocket_create = (urlPtr :CharPtr):number => {
        if (!socket_handles) {
            socket_handles = new Map<number, IDiagnosticConnection>();
        }
        const url = url_override ?? runtimeHelpers.utf8ToString(urlPtr);
        const socket_handle = next_socket_handle++;
        const isWebSocket = url.startsWith("ws://") || url.startsWith("wss://");
        const wrapper = isWebSocket
            ? createDiagConnectionWs(socket_handle, url)
            : createDiagConnectionJs(socket_handle, url);
        socket_handles.set(socket_handle, wrapper);
        return socket_handle;
    };

    diagnosticHelpers.ds_rt_websocket_send = (client_socket :number, buffer:VoidPtr, bytes_to_write:number):number => {
        const wrapper = socket_handles ? socket_handles.get(client_socket) : undefined;
        if (!wrapper) {
            // Cross-worker write: the EventPipe sampling/streaming thread runs on a worker whose
            // socket_handles map is uninitialized, so it has no connection wrapper. This is the
            // .nettrace stream — append it to the shared-memory accumulator that the UI-thread
            // harness drains, and report success so EventPipe keeps streaming instead of faulting
            // this thread (a fault here aborts the runtime mid stop-the-world).
            (Module as any)._diag_stream_append?.(buffer, bytes_to_write);
            return bytes_to_write;
        }
        const message = (new Uint8Array(Module.HEAPU8.buffer, buffer as any >>> 0, bytes_to_write)).slice();
        return wrapper.send(message);
    };

    diagnosticHelpers.ds_rt_websocket_poll = (client_socket :number):number => {
        // serviced on the DS worker's own poll loop (see maybeStart/StopRequestedTrace)
        maybeStartRequestedTrace();
        maybeStopRequestedTrace();
        const wrapper = socket_handles ? socket_handles.get(client_socket) : undefined;
        if (!wrapper) {
            return 0;
        }
        return wrapper.poll();
    };

    diagnosticHelpers.ds_rt_websocket_recv = (client_socket :number, buffer:VoidPtr, bytes_to_read:number):number => {
        const wrapper = socket_handles ? socket_handles.get(client_socket) : undefined;
        if (!wrapper) {
            return -1;
        }
        return wrapper.recv(buffer, bytes_to_read);
    };

    diagnosticHelpers.ds_rt_websocket_close = (client_socket :number):number => {
        const wrapper = socket_handles ? socket_handles.get(client_socket) : undefined;
        if (!wrapper) {
            return -1;
        }
        socket_handles.delete(client_socket);
        return wrapper.close();
    };

    globalObjects.api.collectCpuSamples = collectCpuSamples;
    globalObjects.api.collectMetrics = collectMetrics;
    globalObjects.api.collectGcDump = collectGcDump;
    globalObjects.api.connectDSRouter = connectDSRouter;

    cleanup_js_client();
}

// this will take over the existing connection to JS and send new advert message to WS client
// use dotnet-dsrouter server-websocket -v trace
function connectDSRouter (url: string): void {
    if (!serverSession) {
        throw new Error("No active session to reconnect");
    }

    // make sure new sessions hit the new URL
    url_override = url;

    const wrapper = createDiagConnectionWs(serverSession.client_socket, url);
    socket_handles.set(serverSession.client_socket, wrapper);
    wrapper.send(advertise());
}
