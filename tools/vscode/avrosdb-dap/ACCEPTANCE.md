# avrosdb-dap — Manual Acceptance Checklist

The extension's GUI surfaces (the activity-bar **avrOS Debug** container and its
six `TreeView`s) cannot be driven by the headless Neovim/Python harnesses, so
their acceptance is this documented manual checklist. The automated coverage for
the underlying data lives on the avrOSdb side — the `avrosdb/*List` custom
requests are exercised by host `test_dap` and the on-target
`make hw-test-dap-introspect` case (see User Manual §6.1, Phase 21 in
`doc/SDP.md`).

Run through this list against a live AVR-Dx target after any change to the
extension (`tools/vscode/avrosdb-dap/`). Attach a screenshot of the populated
activity-bar view to the PR.

## Setup

1. Build the host binary: `make` (produces `build/avrOSdb`).
2. Install the extension (one of): `make package-vscode` then
   `code --install-extension dist/avrosdb-dap-<ver>.vsix`, or open
   `tools/vscode/avrosdb-dap` in VS Code and press **F5** (Extension Host).
3. Open a firmware workspace whose ELF links against avrOS (e.g. an avrOS
   example with FSM/event/queue tables — the same `gdb_target.elf` the
   introspection tests use).
4. Reload VS Code after installing.

## A. Turnkey F5 (no `launch.json`) — HLR-088

- [ ] In a workspace with **no** `.vscode/launch.json`, press **F5**; the
      picker offers **avrOSdb (--dap)** and starts with the defaults
      (`build/avrOSdb`, `/dev/ttyAMA2`, `firmware.elf`, port 1234).
- [ ] Rename/move `build/avrOSdb` and press **F5**: the extension offers to
      **build it** (`make`) or fall back to `avrOSdb` on `PATH`; declining both
      aborts the launch with a clear message.
- [ ] With a hand-written `launch` config, **F5** spawns
      `avrOSdb --dap --port <port> <serial> <elf>`, attaches, and the target
      stops at entry (first line of `main` highlighted).
- [ ] Ending the session stops the spawned server (no orphan `avrOSdb`).
- [ ] An `attach` config connects to a separately-started server.

## B. avrOS Debug View container — HLR-087

- [ ] The **avrOS** icon appears in the activity bar; selecting it opens the
      **avrOS Debug** container with exactly six views: **Variables**,
      **Call Stack**, **Breakpoints**, **State Machines**, **Events**,
      **Queues**.
- [ ] With no active `avrosdb` session, all six views are empty.

## C. Debug-proxy views (mirror Run-and-Debug) — HLR-087

While stopped at a breakpoint inside a function with locals:

- [ ] **Variables** shows Locals / Registers / Globals; structs and arrays
      expand; values match the built-in *Variables* view. Editing a value
      writes back to the target.
- [ ] **Call Stack** shows the DWARF-unwound frames with `file:line`; selecting
      a frame re-scopes **Variables** to that frame.
- [ ] **Breakpoints** lists every source/function breakpoint and its condition;
      adding/removing a breakpoint updates the view.
- [ ] All three refresh automatically on each halt (`stopped` event).

## D. avrOS introspection views — HLR-086 / HLR-087

While stopped, against an ELF with avrOS tables:

- [ ] **State Machines** lists each FSM task with its current state and active
      flag — matching `monitor avros tasks` on the GDB-RSP path.
- [ ] **Events** lists each registered event and its status flag — matching
      `monitor avros events`.
- [ ] **Queues** lists each queue's capacity and element size — matching
      `monitor avros queues`.
- [ ] The three refresh on halt; the **↻** title-bar button forces a manual
      refresh.
- [ ] Against a bare-metal ELF with no avrOS tables, the three views are empty
      (graceful degrade), not an error.
