// avrOSdb (DAP) — minimal VS Code companion extension.
//
// VS Code cannot attach to an arbitrary DAP server without a contributed debug
// type, so this extension contributes the `avrosdb` type and points it at a
// already-running `avrOSdb --dap` TCP server (DebugAdapterServer). It launches
// nothing itself — start the server separately:
//
//   build/avrOSdb --dap --port 1234 /dev/ttyAMA2 firmware.elf
//
// then use the "Attach to avrOSdb (--dap)" launch configuration.
//
// NOTE: the avrOSdb DAP front-end is built up across phases; Phase 16
// implements the connection handshake (attach -> stopped at entry, threads,
// disconnect). See doc/UserManual.md §6.

const vscode = require("vscode");

function activate(context) {
  const factory = {
    createDebugAdapterDescriptor(session) {
      const cfg = session.configuration;
      // Connect to the running avrOSdb --dap server over TCP.
      return new vscode.DebugAdapterServer(cfg.port || 1234,
                                           cfg.host || "127.0.0.1");
    },
  };
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("avrosdb", factory));
}

function deactivate() {}

module.exports = { activate, deactivate };
