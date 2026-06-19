// avrOSdb (DAP) — VS Code companion extension.
//
// Two jobs:
//
//  1. Turnkey debug. VS Code cannot talk to an arbitrary DAP server without a
//     contributed debug type, so this extension contributes the `avrosdb` type.
//     avrOSdb speaks DAP over a TCP socket, so for a `launch` request the
//     extension spawns `avrOSdb --dap --port <port> <serial> <elf>`, waits for
//     the "listening on" banner, and connects VS Code to it — one F5 starts the
//     server and attaches, no separate terminal. A DebugConfigurationProvider
//     supplies a ready-to-run config so F5 works even with no launch.json, and
//     locates (or offers to build) the avrOSdb binary. A `request: "attach"`
//     config instead connects to an already-running server.
//
//  2. avrOS Debug View. An "avrOS" activity-bar container with six tree views.
//     Variables / Call Stack / Breakpoints mirror the Run-and-Debug side bar by
//     proxying the active avrosdb session's DAP data (VS Code does not allow the
//     built-in views to be re-parented, so they are re-rendered here via
//     session.customRequest). State Machines / Events / Queues drive the
//     avrOSdb-specific custom requests avrosdb/fsmList, avrosdb/eventList, and
//     avrosdb/queueList (avrOS has no `monitor` console channel over DAP).
//
// See doc/UserManual.md §6.3.

const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");

// ── tree item helpers ──────────────────────────────────────────────────────

function leaf(label, description, icon) {
  const it = new vscode.TreeItem(String(label), vscode.TreeItemCollapsibleState.None);
  if (description !== undefined && description !== null) it.description = String(description);
  if (icon) it.iconPath = new vscode.ThemeIcon(icon);
  return it;
}

function node(label, description, kind, icon) {
  const it = new vscode.TreeItem(String(label), vscode.TreeItemCollapsibleState.Collapsed);
  if (description !== undefined && description !== null) it.description = String(description);
  if (icon) it.iconPath = new vscode.ThemeIcon(icon);
  it._kind = kind;
  return it;
}

// Is `session` one of ours, and is it the active session?
function isAvrosSession(s) {
  return s && s.type === "avrosdb";
}

// ── avrOS introspection views (custom requests) ─────────────────────────────
//
// Each provider maps one custom request's response body to a flat list. They
// are read-only snapshots refreshed whenever the target halts.

class IntrospectProvider {
  // command: DAP custom request; mapper: (body) => TreeItem[]
  constructor(getSession, command, mapper, emptyLabel) {
    this._getSession = getSession;
    this._command = command;
    this._mapper = mapper;
    this._emptyLabel = emptyLabel;
    this._items = [];
    this._emitter = new vscode.EventEmitter();
    this.onDidChangeTreeData = this._emitter.event;
  }
  getTreeItem(e) { return e; }
  getChildren() { return this._items; }
  async refresh() {
    const s = this._getSession();
    if (!isAvrosSession(s)) { this._items = []; this._emitter.fire(); return; }
    try {
      const body = await s.customRequest(this._command);
      const items = this._mapper(body) || [];
      this._items = items.length ? items : [leaf(this._emptyLabel, "", "info")];
    } catch (e) {
      this._items = [leaf("(unavailable)", String(e && e.message || e), "warning")];
    }
    this._emitter.fire();
  }
  clear() { this._items = []; this._emitter.fire(); }
}

function mapFsms(body) {
  const fsms = (body && body.fsms) || [];
  return fsms.map((f) =>
    leaf(f.name || `fsm ${f.id}`, f.state || "", f.active ? "debug-start" : "circle-outline"));
}
function mapEvents(body) {
  const events = (body && body.events) || [];
  return events.map((e) =>
    leaf(e.name || "<event>",
         e.status === null || e.status === undefined ? "—" : `0x${Number(e.status).toString(16)}`,
         "symbol-event"));
}
function mapQueues(body) {
  const queues = (body && body.queues) || [];
  return queues.map((q) =>
    leaf(`queue[${q.id}]`, `capacity=${q.capacity} elemSize=${q.elemSize}`, "list-ordered"));
}

// ── Call Stack view (proxies threads + stackTrace) ──────────────────────────

class CallStackProvider {
  constructor(getSession) {
    this._getSession = getSession;
    this._items = [];
    this._emitter = new vscode.EventEmitter();
    this.onDidChangeTreeData = this._emitter.event;
  }
  getTreeItem(e) { return e; }
  getChildren() { return this._items; }
  async refresh() {
    const s = this._getSession();
    if (!isAvrosSession(s)) { this._items = []; this._emitter.fire(); return; }
    try {
      const th = await s.customRequest("threads");
      const threads = (th && th.threads) || [{ id: 1, name: "cpu" }];
      const tid = threads[0].id;
      const st = await s.customRequest("stackTrace", { threadId: tid, startFrame: 0, levels: 64 });
      const frames = (st && st.stackFrames) || [];
      this._items = frames.length
        ? frames.map((f) => {
            const where = f.source && f.source.name ? `${f.source.name}:${f.line}` : "";
            return leaf(f.name || `0x${(f.instructionPointerReference || "").replace(/^0x/, "")}`,
                        where, "debug-stackframe");
          })
        : [leaf("(no frames)", "", "info")];
    } catch (e) {
      this._items = [leaf("(unavailable)", String(e && e.message || e), "warning")];
    }
    this._emitter.fire();
  }
  clear() { this._items = []; this._emitter.fire(); }
}

// ── Variables view (proxies stackTrace → scopes → variables, lazy) ──────────

class VariablesProvider {
  constructor(getSession) {
    this._getSession = getSession;
    this._emitter = new vscode.EventEmitter();
    this.onDidChangeTreeData = this._emitter.event;
    this._frameId = null;
  }
  getTreeItem(e) { return e; }

  async getChildren(element) {
    const s = this._getSession();
    if (!isAvrosSession(s)) return [];
    try {
      // Top level: resolve frame 0, list its scopes.
      if (!element) {
        const th = await s.customRequest("threads");
        const tid = (((th && th.threads) || [{ id: 1 }])[0]).id;
        const st = await s.customRequest("stackTrace", { threadId: tid, startFrame: 0, levels: 1 });
        const frame = (st && st.stackFrames && st.stackFrames[0]) || null;
        if (!frame) return [leaf("(not stopped)", "", "info")];
        this._frameId = frame.id;
        const sc = await s.customRequest("scopes", { frameId: frame.id });
        const scopes = (sc && sc.scopes) || [];
        return scopes.map((scope) => {
          const it = node(scope.name, "", "ref", "symbol-namespace");
          it._ref = scope.variablesReference;
          return it;
        });
      }
      // Expansion: list variables for a reference.
      if (element._ref) {
        const vr = await s.customRequest("variables", { variablesReference: element._ref });
        const vars = (vr && vr.variables) || [];
        return vars.map((v) => {
          const expandable = v.variablesReference && v.variablesReference > 0;
          const it = expandable ? node(v.name, v.value, "ref", "symbol-variable")
                                : leaf(v.name, v.value, "symbol-field");
          if (expandable) it._ref = v.variablesReference;
          return it;
        });
      }
      return [];
    } catch (e) {
      return [leaf("(unavailable)", String(e && e.message || e), "warning")];
    }
  }
  refresh() { this._emitter.fire(); }
  clear() { this._emitter.fire(); }
}

// ── Breakpoints view (mirrors the workspace breakpoint set) ─────────────────

class BreakpointsProvider {
  constructor() {
    this._emitter = new vscode.EventEmitter();
    this.onDidChangeTreeData = this._emitter.event;
  }
  getTreeItem(e) { return e; }
  getChildren() {
    const bps = vscode.debug.breakpoints || [];
    if (!bps.length) return [leaf("(no breakpoints)", "", "info")];
    return bps.map((bp) => {
      if (bp instanceof vscode.SourceBreakpoint || (bp.location && bp.location.uri)) {
        const loc = bp.location;
        const name = loc.uri.path.split("/").pop();
        const line = loc.range.start.line + 1;
        const desc = bp.condition ? `if ${bp.condition}` : "";
        const it = leaf(`${name}:${line}`, desc, bp.enabled ? "debug-breakpoint" : "debug-breakpoint-disabled");
        return it;
      }
      if (bp instanceof vscode.FunctionBreakpoint || bp.functionName) {
        return leaf(bp.functionName, bp.condition ? `if ${bp.condition}` : "", "debug-breakpoint-function");
      }
      return leaf("breakpoint", "", "debug-breakpoint");
    });
  }
  refresh() { this._emitter.fire(); }
}

// ── turnkey: locate the avrOSdb binary, offer to build it ───────────────────

function substituteWorkspace(p) {
  const ws = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  const root = ws ? ws.uri.fsPath : process.cwd();
  return String(p || "").replace(/\$\{workspaceFolder\}/g, root);
}

async function locateAvrosdb(cfg, out) {
  const ws = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  const root = ws ? ws.uri.fsPath : process.cwd();
  const candidate = substituteWorkspace(cfg.program || "${workspaceFolder}/build/avrOSdb");

  if (fs.existsSync(candidate)) return candidate;

  // Not built yet — offer to run `make` in the workspace root.
  const pick = await vscode.window.showWarningMessage(
    `avrOSdb not found at ${candidate}. Build it now?`, "Build (make)", "Use PATH", "Cancel");
  if (pick === "Build (make)") {
    out.appendLine(`building: make (cwd ${root})`);
    const ok = await new Promise((resolve) => {
      const proc = cp.spawn("make", [], { cwd: root });
      proc.stdout.on("data", (d) => out.append(String(d)));
      proc.stderr.on("data", (d) => out.append(String(d)));
      proc.on("exit", (code) => resolve(code === 0));
      proc.on("error", () => resolve(false));
    });
    if (ok && fs.existsSync(candidate)) return candidate;
    vscode.window.showErrorMessage("avrOSdb build failed — see the avrOSdb output channel.");
    return undefined;
  }
  if (pick === "Use PATH") return "avrOSdb";
  return undefined;
}

// ── activation ──────────────────────────────────────────────────────────────

function activate(context) {
  const out = vscode.window.createOutputChannel("avrOSdb");
  const spawned = new Map(); // debug session id -> spawned ChildProcess

  const activeSession = () => {
    const s = vscode.debug.activeDebugSession;
    return isAvrosSession(s) ? s : undefined;
  };

  // --- views ---
  const fsmView = new IntrospectProvider(activeSession, "avrosdb/fsmList", mapFsms, "(no state machines)");
  const evtView = new IntrospectProvider(activeSession, "avrosdb/eventList", mapEvents, "(no events)");
  const queView = new IntrospectProvider(activeSession, "avrosdb/queueList", mapQueues, "(no queues)");
  const stackView = new CallStackProvider(activeSession);
  const varsView = new VariablesProvider(activeSession);
  const bpView = new BreakpointsProvider();

  const refreshAll = () => {
    fsmView.refresh(); evtView.refresh(); queView.refresh();
    stackView.refresh(); varsView.refresh(); bpView.refresh();
  };
  const clearAll = () => {
    fsmView.clear(); evtView.clear(); queView.clear();
    stackView.clear(); varsView.clear(); bpView.refresh();
  };

  context.subscriptions.push(
    vscode.window.registerTreeDataProvider("avrosStateMachines", fsmView),
    vscode.window.registerTreeDataProvider("avrosEvents", evtView),
    vscode.window.registerTreeDataProvider("avrosQueues", queView),
    vscode.window.registerTreeDataProvider("avrosCallStack", stackView),
    vscode.window.registerTreeDataProvider("avrosVariables", varsView),
    vscode.window.registerTreeDataProvider("avrosBreakpoints", bpView),
    vscode.commands.registerCommand("avrosdb.refresh", refreshAll),
    vscode.debug.onDidChangeActiveDebugSession(() => refreshAll()),
    vscode.debug.onDidTerminateDebugSession((s) => {
      if (isAvrosSession(s)) clearAll();
    }),
    vscode.debug.onDidChangeBreakpoints(() => bpView.refresh()),
    out);

  // Refresh the views every time the target halts (stopped) and clear the
  // stack/variables snapshots when it resumes (continued).
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterTrackerFactory("avrosdb", {
      createDebugAdapterTracker() {
        return {
          onDidSendMessage(m) {
            if (m && m.type === "event" && m.event === "stopped") {
              // Give the adapter a beat to settle, then snapshot.
              setTimeout(refreshAll, 50);
            } else if (m && m.type === "event" && m.event === "continued") {
              stackView.clear(); varsView.clear();
            }
          },
        };
      },
    }));

  // --- turnkey debug configuration provider ---
  const provider = {
    // F5 with no/empty launch.json: hand back a runnable default.
    provideDebugConfigurations() {
      return [
        {
          type: "avrosdb",
          request: "launch",
          name: "Debug with avrOSdb (--dap)",
          program: "${workspaceFolder}/build/avrOSdb",
          serial: "/dev/ttyAMA2",
          elf: "${workspaceFolder}/firmware.elf",
          port: 1234,
        },
      ];
    },
    // Fill in defaults and ensure the binary exists before the session starts.
    async resolveDebugConfiguration(folder, cfg) {
      // Empty config (F5 with no launch.json) → seed a launch config.
      if (!cfg.type && !cfg.request && !cfg.name) {
        cfg.type = "avrosdb";
        cfg.request = "launch";
        cfg.name = "Debug with avrOSdb (--dap)";
        cfg.program = "${workspaceFolder}/build/avrOSdb";
        cfg.serial = "/dev/ttyAMA2";
        cfg.elf = "${workspaceFolder}/firmware.elf";
        cfg.port = 1234;
      }
      if (cfg.type !== "avrosdb") return cfg;
      if ((cfg.request || "launch") === "launch") {
        const bin = await locateAvrosdb(cfg, out);
        if (!bin) return undefined; // abort launch
        cfg.program = bin;
      }
      return cfg;
    },
  };
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider("avrosdb", provider),
    vscode.debug.registerDebugConfigurationProvider("avrosdb", provider,
      vscode.DebugConfigurationProviderTriggerKind.Dynamic));

  // --- debug adapter descriptor factory (spawn-on-launch + connect) ---
  const factory = {
    async createDebugAdapterDescriptor(session) {
      const cfg = session.configuration;
      const host = cfg.host || "127.0.0.1";
      const port = cfg.port || 1234;

      if (cfg.request === "launch") {
        const bin = cfg.program || "avrOSdb";
        const args = ["--dap", "--port", String(port)];
        if (Array.isArray(cfg.extraArgs)) args.push(...cfg.extraArgs);
        if (cfg.serial) args.push(substituteWorkspace(cfg.serial));
        if (cfg.elf) args.push(substituteWorkspace(cfg.elf));

        out.appendLine(`launching: ${bin} ${args.join(" ")}`);
        await new Promise((resolve, reject) => {
          let proc;
          try {
            proc = cp.spawn(bin, args, { cwd: substituteWorkspace(cfg.cwd) || undefined });
          } catch (e) {
            return reject(e);
          }
          spawned.set(session.id, proc);
          let ready = false;
          const ok = () => { if (!ready) { ready = true; resolve(); } };
          const sniff = (d) => {
            const s = String(d);
            out.append(s);
            if (s.includes("listening on")) ok();
          };
          proc.stdout.on("data", sniff);
          proc.stderr.on("data", sniff);
          proc.on("error", (e) => { if (!ready) reject(e); });
          proc.on("exit", (code) => {
            if (!ready)
              reject(new Error(
                `avrOSdb exited (code ${code}) before listening on ${host}:${port}`));
          });
          // Fallback: if the build doesn't print the banner, give it a moment.
          setTimeout(ok, 8000);
        });
      }

      return new vscode.DebugAdapterServer(port, host);
    },
  };

  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("avrosdb", factory),
    vscode.debug.onDidTerminateDebugSession((s) => {
      const p = spawned.get(s.id);
      if (p) { try { p.kill(); } catch (_) {} spawned.delete(s.id); }
    }));
}

function deactivate() {}

module.exports = { activate, deactivate };
