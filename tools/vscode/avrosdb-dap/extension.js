// avrOSdb (DAP) — VS Code companion extension.
//
// VS Code cannot talk to an arbitrary DAP server without a contributed debug
// type, so this extension contributes the `avrosdb` type. avrOSdb speaks DAP
// over a TCP socket (it opens a listener), so for a `launch` request the
// extension spawns `avrOSdb --dap --port <port> <serial> <elf>`, waits for it
// to report "listening on", and connects VS Code to it — one F5 starts the
// server and attaches, no separate terminal. A `request: "attach"` config
// instead connects to an already-running server (e.g. on a remote target host).
//
// See doc/UserManual.md §6.3.

const vscode = require("vscode");
const cp = require("child_process");

function activate(context) {
  const out = vscode.window.createOutputChannel("avrOSdb");
  const spawned = new Map(); // debug session id -> spawned ChildProcess

  const factory = {
    async createDebugAdapterDescriptor(session) {
      const cfg = session.configuration;
      const host = cfg.host || "127.0.0.1";
      const port = cfg.port || 1234;

      // launch: start avrOSdb --dap ourselves, then connect once it listens.
      if (cfg.request === "launch") {
        const bin = cfg.program || "avrOSdb";
        const args = ["--dap", "--port", String(port)];
        if (Array.isArray(cfg.extraArgs)) args.push(...cfg.extraArgs);
        if (cfg.serial) args.push(cfg.serial);
        if (cfg.elf) args.push(cfg.elf);

        out.appendLine(`launching: ${bin} ${args.join(" ")}`);
        await new Promise((resolve, reject) => {
          let proc;
          try {
            proc = cp.spawn(bin, args, { cwd: cfg.cwd });
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
                `avrOSdb exited (code ${code}) before listening on ` +
                `${host}:${port}`));
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
    // Terminate the spawned server when its debug session ends.
    vscode.debug.onDidTerminateDebugSession((s) => {
      const p = spawned.get(s.id);
      if (p) { try { p.kill(); } catch (_) {} spawned.delete(s.id); }
    }),
    out);
}

function deactivate() {}

module.exports = { activate, deactivate };
