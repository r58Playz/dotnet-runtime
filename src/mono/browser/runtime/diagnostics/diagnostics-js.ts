// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

import { advert1, CommandSetId, commandStopTracing, dotnet_IPC_V1, ServerCommandId } from "./client-commands";
import { DiagnosticConnectionBase, downloadBlob, fnClientProvider, IDiagnosticClient, IDiagnosticConnection, IDiagnosticSession, scheduleDiagnosticServerEventLoop, SessionId } from "./common";
import { PromiseAndController } from "../types/internal";
import { loaderHelpers, Module } from "./globals";
import { mono_log_warn } from "./logging";
import { collectCpuSamples } from "./dotnet-cpu-profiler";
import { collectMetrics } from "./dotnet-counters";
import { collectGcDump } from "./dotnet-gcdump";

//let diagClient:IDiagClient|undefined = undefined as any;
//let server:DiagServer = undefined as any;

// configure your application
// .withEnvironmentVariable("DOTNET_DiagnosticPorts", "download:gcdump")
// or implement function globalThis.dotnetDiagnosticClient with IDiagClient interface

let nextJsClient:PromiseAndController<IDiagnosticClient>;
let fromScenarioNameOnce = false;

// Only the last which sent advert is receiving commands for all sessions
export let serverSession:DiagnosticSession|undefined = undefined;

// A session whose advert arrived before any JS client was armed (the on-demand /
// collect-after-advert ordering). setupJsClient() drives it directly rather than relying on
// resolving the mutable `nextJsClient` promise, which the suspended connectNewClient may no
// longer be awaiting.
let pendingSession:DiagnosticSession|undefined = undefined;

// session_id of the active collection, captured when the server's OK arrives. In the js://
// (server-connects) model a StopTracing must be sent as the client's response to a fresh
// (re-)advert connection — i.e. on the current serverSession, which the server is polling for a
// command — NOT on the busy streaming connection. We just need the captured session_id (the
// re-advert serverSession has no session_id of its own).
let activeSessionId:SessionId|undefined = undefined;

export function stopActiveSession ():boolean {
    const M = Module as any;
    M._diag_set_step2?.(50);
    if (!serverSession || activeSessionId == null) {
        M._diag_set_step2?.(52);
        return false;
    }
    M._diag_set_step2?.(51);
    serverSession.respond(commandStopTracing(activeSessionId));
    M._diag_set_step2?.(53);
    return true;
}

// singleton wrapping the protocol with the diagnostic server in the Mono VM
// there could be multiple connection at the same time.
// DS:advert         ->1
//                     1<- DC1: command to start tracing session
// DS:OK, session ID ->1
// DS:advert         ->2
// DS:events         ->1
// DS:events         ->1
// DS:events         ->1
// DS:events         ->1
//                     2<- DC1: command to stop tracing session
// DS:close          ->1

class DiagnosticSession extends DiagnosticConnectionBase implements IDiagnosticConnection, IDiagnosticSession {
    public session_id: SessionId = undefined as any;
    public diagClient?: IDiagnosticClient;
    public stopDelayedAfterLastMessage:number|undefined = undefined;
    public resumedRuntime = false;

    constructor (public client_socket:number) {
        super(client_socket);
    }

    sendCommand (message: Uint8Array): void {
        if (!serverSession) {
            mono_log_warn("no server yet");
            return;
        }
        serverSession.respond(message);
    }

    connectNewClient () {
        (Module as any)._diag_set_step?.(2);
        if (nextJsClient.promise_control.isDone) {
            // arm-before-advert: a client is already available, drive with it
            nextJsClient.promise.then((client) => this.driveWithClient(client));
        } else {
            // advert-before-arm (on-demand): record and let setupJsClient drive us
            // eslint-disable-next-line @typescript-eslint/no-this-alias
            pendingSession = this;
            (Module as any)._diag_set_step?.(20);
        }
    }

    driveWithClient (client:IDiagnosticClient) {
        const M = Module as any;
        try {
            M._diag_set_step?.(21);
            this.diagClient = client;
            cleanupClient();
            M._diag_set_step?.(22);
            const firstCommand = this.diagClient.commandOnAdvertise();
            M._diag_set_step?.(3);
            this.respond(firstCommand);
            M._diag_set_step?.(31);
        } catch (e:any) {
            M._diag_set_step?.(91);
            mono_log_warn("[diag] driveWithClient failed: " + (e?.message || e));
        }
    }

    is_advert_message (message:Uint8Array):boolean {
        return advert1.every((v, i) => v === message[i]);
    }

    is_response_message (message:Uint8Array):boolean {
        return dotnet_IPC_V1.every((v, i) => v === message[i]) && message[16] == CommandSetId.Server;
    }

    is_response_ok_with_session (message:Uint8Array):boolean {
        return message.byteLength === 28 && message[17] == ServerCommandId.OK;
    }

    parse_session_id (message:Uint8Array):SessionId {
        const view = message.subarray(20, 28);
        const sessionIDLo = view[0] | (view[1] << 8) | (view[2] << 16) | (view[3] << 24);
        const sessionIDHi = view[4] | (view[5] << 8) | (view[6] << 16) | (view[7] << 24);
        return [sessionIDHi, sessionIDLo] as SessionId;
    }

    // this is message from the diagnostic server, which is Mono VM in this browser
    send (message:Uint8Array):number {
        scheduleDiagnosticServerEventLoop();
        (Module as any)._diag_set_step?.(this.is_advert_message(message) ? 4 : (this.is_response_message(message) ? (this.is_response_ok_with_session(message) ? 5 : 6) : 7));
        if (this.is_advert_message(message)) {
            // eslint-disable-next-line @typescript-eslint/no-this-alias
            serverSession = this;
            this.connectNewClient();
        } else if (this.is_response_message(message)) {
            if (this.is_response_ok_with_session(message)) {
                this.session_id = this.parse_session_id(message);
                activeSessionId = this.session_id;
                if (this.diagClient?.onSessionStart) {
                    this.diagClient.onSessionStart(this);
                }
            } else {
                if (this.diagClient?.onError) {
                    this.diagClient.onError(this, message);
                } else {
                    mono_log_warn("Diagnostic session " + this.session_id + " error : " + message.toString());
                }
            }
        } else {
            // .nettrace data stream. On the DS worker this is the magic + FastSerialization
            // header + initial objects (written before the streaming thread takes over). Route
            // it to the same shared accumulator the streaming thread's cross-worker writes go
            // to, so the full stream is reassembled in order and drained by the UI harness.
            const M = Module as any;
            if (M._diag_stream_append && M._malloc) {
                const p = M._malloc(message.length) >>> 0;
                M.HEAPU8.set(message, p);
                M._diag_stream_append(p, message.length);
                M._free(p);
            } else if (this.diagClient?.onData) {
                this.diagClient.onData(this, message);
            } else {
                this.store(message);
            }
        }

        return message.length;
    }

    // this is message to the diagnostic server, which is Mono VM in this browser
    respond (message:Uint8Array) : void {
        this.messagesReceived.push(message);
        scheduleDiagnosticServerEventLoop();
    }

    close (): number {
        if (this.diagClient?.onClose) {
            this.diagClient.onClose(this.messagesToSend);
        }
        if (this.diagClient?.onClosePromise) {
            this.diagClient.onClosePromise.resolve(this.messagesToSend);
        }
        if (this.messagesToSend.length === 0) {
            return 0;
        }
        if (this.diagClient && !this.diagClient.skipDownload) {
            downloadBlob(this.messagesToSend);
        }
        this.messagesToSend = [];
        return 0;
    }
}

export function cleanupClient () {
    nextJsClient = loaderHelpers.createPromiseController<IDiagnosticClient>();
}

export function setupJsClient (client:IDiagnosticClient) {
    // advert-before-arm: a session already advertised and is waiting; drive it directly
    if (pendingSession) {
        const s = pendingSession;
        pendingSession = undefined;
        s.driveWithClient(client);
        return;
    }
    // arm-before-advert: stash the client for the next advert's connectNewClient
    if (nextJsClient.promise_control.isDone) {
        throw new Error("multiple clients in parallel are not allowed");
    }
    nextJsClient.promise_control.resolve(client);
}

export function createDiagConnectionJs (socket_handle:number, scenarioName:string):DiagnosticSession {
    if (!fromScenarioNameOnce) {
        fromScenarioNameOnce = true;
        if (scenarioName.startsWith("js://gcdump")) {
            collectGcDump({});
        }
        if (scenarioName.startsWith("js://counters")) {
            collectMetrics({});
        }
        if (scenarioName.startsWith("js://cpu-samples")) {
            collectCpuSamples({});
        }
        const dotnetDiagnosticClient:fnClientProvider = (globalThis as any).dotnetDiagnosticClient;
        if (typeof dotnetDiagnosticClient === "function" ) {
            nextJsClient.promise_control.resolve(dotnetDiagnosticClient(scenarioName));
        }
    }
    return new DiagnosticSession(socket_handle);
}
