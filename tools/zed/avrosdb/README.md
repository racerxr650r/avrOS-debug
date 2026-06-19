# avrOSdb (DAP) — Zed extension

Debug AVR-Dx targets in [Zed](https://zed.dev) via `avrOSdb --dap`. This
extension registers an `avrosdb` debug adapter so Zed's native debugger can
**launch** the server and attach, or **attach** to an already-running server —
no hand-written wiring beyond a small `.zed/debug.json` entry.

It is the Zed counterpart of the VS Code companion in
[`tools/vscode/avrosdb-dap`](../../vscode/avrosdb-dap). Because Zed does not let
an extension contribute custom side-bar tree views, this extension does **not**
provide the avrOS state-machine / event / queue introspection the VS Code
extension does — you get launch/attach plus Zed's built-in Variables, Call
Stack, and Breakpoints panes. For avrOS runtime introspection, use the VS Code
extension or `monitor avros …` on the GDB-RSP path.

## Install (development / dev extension)

Zed compiles the extension's Rust to WebAssembly when you install it as a dev
extension — you only need the Rust toolchain:

```bash
rustup target add wasm32-wasip1     # one-time
```

Then in Zed: **Extensions → Install Dev Extension…** and pick this
`tools/zed/avrosdb` directory. (Or, from the repo root, `make package-zed`
pre-builds the `wasm32` artefact to validate it compiles.)

## Configure a debug session — `.zed/debug.json`

Launch (one action spawns avrOSdb and attaches):

```json
[
  {
    "label": "Debug with avrOSdb",
    "adapter": "avrosdb",
    "request": "launch",
    "program": "$ZED_WORKTREE_ROOT/build/avrOSdb",
    "serial": "/dev/ttyAMA2",
    "elf": "$ZED_WORKTREE_ROOT/firmware.elf",
    "port": 1234
  }
]
```

Attach to a server you started yourself (`avrOSdb --dap --port 1234 <serial>
<elf>`):

```json
[
  {
    "label": "Attach to avrOSdb",
    "adapter": "avrosdb",
    "request": "attach",
    "host": "127.0.0.1",
    "port": 1234
  }
]
```

`program` defaults to `avrOSdb` on `PATH`; `serial` to `/dev/ttyAMA2`; `port` to
`1234`. `extraArgs` (e.g. `["--baud","230400"]`) are inserted before the
serial/elf operands.

## No-extension alternative

If you would rather not install an extension, start the server manually and let
Zed connect over TCP via a `tcp_connection` block — see User Manual §6.4.
