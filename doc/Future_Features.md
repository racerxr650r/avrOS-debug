Read [](file:///home/john/Projects/avrOS-debug/doc/PVD.md)

Here are feature ideas grouped by alignment with the existing PVD/roadmap and effort. None of these are committed — they would each need a new HLR or roadmap update.

## In line with current vision (FSM/avrOS introspection)

1. **`monitor avros queues` / `monitor avros mempool`** — the PVD §7.1 promises these alongside `monitor avros events`, but only events is implemented today. Adds non-intrusive UPDI reads to decode queue head/tail/payloads and memory-pool free-lists.
2. **FSM state-history ring buffer** — a small SRAM-resident ring in avrOS that the server polls to render an FSM transition timeline in the IDE.
3. **Virtual-thread backtrace per FSM** — today each FSM shows as a thread with its function pointer; extend by reading the saved `Y`/`Z`/`SP` slot in the FSM record (if avrOS adds one) to surface a one-frame "current state handler" backtrace.
4. **Watchpoints over UPDI background reads** — software-emulated data watchpoints by polling specific addresses between RSP packets, bypassing the AVR's 1-2 hardware breakpoint limit.
5. **`monitor reset` / `monitor reset halt`** — RESET via UPDI SYSRST with optional re-entry to debug, scriptable from launch configs.

## NVM / programming features

6. **Read-back / verify after `--load`** — read FLASH after write and compare against the ELF; surface differing pages.
7. **Hex/ELF dump subcommands** — `--dump-flash <file>`, `--dump-eeprom <file>` for backup/forensics, reusing the existing NVM read path.
8. **EEPROM/USERROW preservation across `--erase`** — chip-erase then restore selected windows from a snapshot file.
9. **Fuses pretty-printer** — `--device` mode already reads SIGROW/REVID; extend to decode FUSE bytes against the per-family table.
10. **Signed/locked-device unlock workflow** — detect locked target, prompt, perform key-sequenced UPDI unlock, re-enter debug.

## Family / silicon coverage (extends Phase 8 work)

11. **AVR-EA / AVR-EB family maps** — the table is structured for it; needs window addresses + SIGROW relocation handling like AVR-DU.
12. **tinyAVR-1/2 UPDI** — same protocol, different NVM controller version; would expand the addressable user base significantly.
13. **Auto-baud / link quality probe** — `--device` mode currently uses 115200; auto-negotiate down to the highest reliable rate per cable.

## Host-side / developer experience

14. **`launch.json` snippet generator** — `avrOSdb --emit-vscode-config` writes a ready-to-use Cortex-Debug stanza pointing at this binary and the active ELF.
15. **Zed DAP shim** — small standalone translator so Zed users don't need an external gdb in the loop (this is the PVD §9 "Native DAP Translation" theme).
16. **Structured JSON event log** — `--log-json <file>` emitting one record per RSP packet, UPDI transaction, and FSM transition for CI postmortems.
17. **Replay / record mode** — capture a UPDI session and replay against unit tests; would let HW-only bugs become reproducible in CI.

## Robustness / observability

18. **Live `--device` mode** — repeat the SIGROW/SYSCFG read every N seconds and emit deltas; useful for diagnosing flaky UPDI cabling.
19. **UPDI link metrics monitor command** — `monitor updi stats` exposing TX/RX byte counts, retry counts, parity errors, baud changes.
20. **Graceful re-attach** — if the GDB client disconnects, keep UPDI open and accept a fresh client without re-entering debug or losing breakpoints.

## Build / release

21. **Windows port via WSL/MinGW** — explicitly out-of-scope today (PVD §7.2) but the termios/select code is small enough to abstract behind a thin HAL.
22. **Static-musl Linux release** — single-binary tarball for CI runners and offline workstations.
23. **`apt`/`dnf` repository** — the `.deb`/`.rpm` builds already exist under build; an actual hosted repo would close the install loop.

## Highest-leverage suggestions

If you want the biggest user-visible payoff per unit of work, my top three would be:

- **(1) `monitor avros queues` + `monitor avros mempool`** — closes the largest gap between PVD promises and shipped behaviour.
- **(6) Read-back/verify after `--load`** — turns the loader into a tool you can trust in CI.
- **(14) `launch.json` emitter** — collapses the "get the IDE talking to my AVR" onboarding step from a wiki page to one command.

Want me to draft HLR/LLR text for any of these and wire them into Project.xml?