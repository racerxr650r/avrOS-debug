# Software Design Document: avrOS-debug (aod)

**Version:** 0.1
**Date:** 2026-05-16
**Author(s):** John Anderson

## 1. Introduction

### 1.1 Purpose of the Document
This document provides a detailed design for the `aod` (avrOS-debug)
command-line GDB stub application. It is intended for embedded application developers and toolchain integrators
of the `aod` software.

### 1.2 Scope of the Document
This document describes the design of the source modules that implement the avrOSdb server:

*   [src/main.c](../src/main.c): Entry point, CLI argument parsing, and the top-level select()-based event loop.
*   [src/updi.c](../src/updi.c): UPDI physical layer: UART serial management, UPDI protocol framing, NVM flash programming, and UPDI console bridge.
*   [src/gdb_rsp.c](../src/gdb_rsp.c): GDB Remote Serial Protocol server: packet codec, command dispatch, and session lifecycle management.
*   [src/elf_parser.c](../src/elf_parser.c): ELF parser: locates avrOS FLASH-resident system tables and produces the AvrOsSymbolIndex.
*   [src/fsm_mapper.c](../src/fsm_mapper.c): avrOS FSM introspection snapshot provider: reads the FSM registration tables and exposes each FSM's name, active flag, and current state.
*   [src/monitor.c](../src/monitor.c): Custom monitor command handler: implements the `avros events` and `avros queues` sub-commands via non-intrusive UPDI reads.
*   [Makefile](../Makefile): Build orchestration: compile, test, install, uninstall, check-tools, and bundle (Debian .deb, Red Hat .rpm, Homebrew formula) targets.
*   [tests/hw/hw_test.c](../tests/hw/hw_test.c): On-target hardware integration test harness: links against `src/updi.c` and exercises the live UPDI silicon, device-info report, SRAM round-trips, NVM page programming, and the RSP server's TCP path. Manual-only — never wired into `make test`.
*   [doc/avrOSdb.1](../doc/avrOSdb.1): Unix man page: reference documentation for the avrOSdb command.

It does not cover the build system, IDE adapter layers (Cortex-Debug, Zed DAP), or Windows support, all of which are out of scope for the initial release (see `doc/PVD.md §7.2`).

### 1.3 Project Overview
`avrOSdb` is a POSIX C99 GDB stub that bridges the UPDI debug interface of modern AVR microcontrollers (DA/DB families) to standard IDEs over the GDB Remote Serial Protocol (RSP). A developer launches the stub, points it at a serial device and an ELF binary, and connects any GDB-compatible front-end — VS Code with Cortex-Debug, Zed with its DAP adapter, or bare `avr-gdb` — using the standard `target extended-remote` command.

The server connects to the target via a TTL-level UART serial adapter with a 1 kΩ resistor on the UPDI line. No external JTAG programmer or proprietary debugger hardware is required — only a USB-to-serial adapter or direct Raspberry Pi UART pins.

Unlike conventional GDB stubs that expose a flat memory model, `avrOSdb` provides native avrOS state-machine awareness. At attach time it parses the supplied ELF binary to locate the avrOS FSM registration tables in FLASH, then reads each registered finite state machine's runtime state into a snapshot the developer can inspect via `monitor` commands (which FSM is active, and each FSM's current state). avrOS FSMs are surfaced as introspection, not as GDB threads: the live CPU is the sole GDB thread.

The project ships with a `make install` target that installs the compiled binary to `$(PREFIX)/bin/` and the accompanying Unix man page to `$(PREFIX)/share/man/man1/`. A `make check-tools` target validates that all required build tools (`gcc`, `make`, `avr-gcc`, `avr-binutils`) are present on the host before any compilation is attempted. A `make bundle` target produces native distribution packages for three platforms: a Debian binary package (`.deb`), a Red Hat RPM package (`.rpm`), and a Homebrew formula (`dist/avrOSdb.rb`) for macOS. All output artefacts are written under the `dist/` directory. These targets ensure the project can be built, deployed, and distributed by a developer from a single `make` command sequence with no manual file copying.

**Planned — Phase 9 — CI-Grade Loader & Link Diagnostics (not yet implemented).** A forthcoming iteration extends the programmer and the `--device` diagnostic mode with the following capabilities so the tool can be trusted as a CI build/flash step and used as a first-line link-health probe. Tracked in GitHub issue #28; not yet bound to HLRs/LLRs.

* **Read-back verify after `--load`.** After every successful NVM write, re-read the affected pages via UPDI and compare against the ELF payload. Mismatching pages shall be reported by `(window, page-aligned address, expected-CRC, actual-CRC)`, the application shall exit with a distinct non-zero exit code (`2`, reserved for verify failure as opposed to `1` for I/O failure), and shall not enter debug or open the GDB listener. Verify shall cover FLASH, EEPROM, USERROW, and FUSES windows; LOCK is excluded because lockbits become read-only post-write.
* **`--prog` program-and-exit mode.** A new mutually-exclusive operating mode that programs the supplied ELF into NVM, verifies it (as above), and exits — no GDB listener is opened, no `updi_enter_debug()` is issued, the UPDI link is dropped cleanly with the target left running. Mode shall print, to `stdout`, the negotiated `baud=<N>` line at start-up and a single-line ANSI-aware progress bar (`[#####.....] 53%  page 27/51 erasing|writing|verifying <window>`) that updates in place when `stdout` is a TTY and degrades to one line per phase transition when piped. Intended use is `avrOSdb --prog /dev/ttyUSB0 firmware.elf` from a CI script or a Makefile `flash:` target.
* **Fuses pretty-printer (`--device` mode).** Extend the existing `--device` SIGROW/REVID dump (LLR-MAIN-08, LLR-UPDI-26) to read all FUSE bytes, then decode each bit-field against the per-family fuse table (BODCFG, OSCCFG, SYSCFG0/1, CODESIZE, BOOTSIZE, …) using human-readable enum names rather than raw hex. Lock byte is decoded with the same scheme. Output format mirrors avrdude's `-Tu` so existing tooling can consume it.
* **Auto-baud / link-quality probe (`--device` mode).** Replace the hard-coded 115200 baud with a probe that walks a candidate ladder (`230400, 200000, 150000, 115200, 57600, 38400, 19200`), at each step opening the link, issuing a small fixed UPDI transaction sequence (LDCS, NVMCTRL STATUS read × N), and recording the byte error / retry rate. The highest rate with zero retries over the sample window becomes the reported negotiated baud. Result is printed as `baud=<N> errors=<K>/<total>` per candidate and the chosen rate is highlighted. Probe is read-only; no NVM, fuses, or system-reset paths are touched.

Phase 9 work shall reuse the existing layered architecture (§2.1): the NVM read primitives live in `src/updi.c`, the verify and progress-bar wiring lives in `src/main.c`, and the fuse-decode tables live alongside the per-family `g_device_table[]` in `src/updi.c` so they are selected by the same `updi_select_device()` path. No new top-level module is introduced.

**Planned — Phase 10 — GDB Protocol Completion (avarice Feature Parity) (not yet implemented).** The Phase 1–8 RSP server implements the minimum subset of the GDB Remote Serial Protocol needed to attach, read state, single-step, set up to two hardware breakpoints, continue, interrupt, and detach. Phase 10 brings the protocol surface up to functional parity with the legacy `avarice` JTAG/dW stub for the AVR-Dx UPDI use case: equivalent `monitor` verbs, equivalent `(gdb) load` behaviour, equivalent watchpoint and software-breakpoint experience, and equivalent extended-remote lifecycle. Wire-level drop-in compatibility with `avarice`-specific scripts is an explicit non-goal — individual `.gdbinit` files, IDE launch configurations, and CI invocations may need targeted edits to reach the same outcome. Tracked in GitHub issue #31; bound to HLRs in §12 below.

The following protocol features and monitor commands are in scope; each is justified by an `avarice` capability that today forces the operator either to drop out of the GDB session or to bridge through a second tool (avrdude, pymcuprog, raw scripts).

* **Flash programming from the GDB session (`vFlashErase`, `vFlashWrite`, `vFlashDone`).** Implements `(gdb) load` so a developer can rebuild and reflash without exiting `avr-gdb`. The handlers shall reuse the existing `updi_nvm_*` page primitives in `src/updi.c`; on `vFlashDone` the application shall re-enter OCD via `updi_enter_debug()` and present the post-load CPU as halted at reset for the next `c`/`s`. `vFlashErase` requests outside the on-target FLASH window (i.e. into EEPROM/USERROW/FUSES) shall be rejected with `E22` rather than silently corrupting non-FLASH NVM, because `gdb load` has no per-section verb to disambiguate.
* **Software breakpoints via flash `BREAK` opcode (`Z0`/`z0` true SW BP).** Phase 1–8 aliases `Z0` to the two OCD hardware comparators (HLR-016), capping the user at two simultaneous breakpoints. Phase 10 adds a true software-breakpoint path that patches the AVR `BREAK` opcode (`0x9598`) into the affected FLASH page, with a per-session shadow map of `(address, original 2-byte opcode)` so `z0` restores the original instruction on remove. The save/patch/restore sequence shall preserve live register, SREG, and SP state across the unavoidable NVMPROG enter/exit (which issues a CPU reset). Implementation shall reuse the page-aligned read-modify-write helper from `src/updi.c`. When the operator opts out via `monitor avros bp-mode hw-only`, `Z0` continues to alias to the HW comparators (the Phase 1–8 behaviour).
* **Hardware data watchpoints (`Z2`/`Z3`/`Z4`, `z2`/`z3`/`z4`).** Originally scoped to expose on-silicon data-address comparators, but the FF-bomb experiment in `doc/reference/guesswork.md` and cross-checks against Bloom, `feline-felicity/avr-absurd`, and Microchip's own `pyavrdebug` (CMSIS-DAP stack) all confirm the AVR-Dx UPDI OCD exposes no data-watchpoint hardware. The server therefore replies the empty RSP packet (`$#00`) to Z2/Z3/Z4 — the documented "unsupported" signal — and GDB falls back transparently to software watchpoints (single-step + memory poll). The user-visible `watch <expr>` command continues to work; no `qSupported` capability is advertised.
* **Standard GDB monitor commands.** The `monitor avros` namespace is kept (and remains the home for FSM-specific introspection); Phase 10 adds an `avarice`-compatible top-level set of monitor verbs: `monitor reset` (issue `updi_reset()` and halt), `monitor halt` (call `updi_halt()`), `monitor go` (call `updi_run()`), `monitor erase` / `monitor chip-erase` (CHIPERASE via `updi_chip_erase()`, refused unless the session was opened with `--allow-erase`), `monitor version` (print server version, git hash, and target family), and `monitor help` (list every known verb). Implementation lives in `src/monitor.c`; the dispatcher shall continue to route the `avros` prefix to the existing avrOS-specific handlers, falling through to the new generic handlers for any other verb.
* **Extended-remote lifecycle and protocol cleanup.** Implements `vRun`/`vAttach`/`vKill` so an IDE can launch and relaunch the debug target from one persistent GDB session (`target extended-remote` semantics), and fills in the small standard packets that `avarice` answers but Phase 1–8 leaves as empty replies: `qC` (current thread), `qOffsets` (returns zeros for our position-independent reset vector), `T<tid>` (is-thread-alive), and `R` (RCmd restart, equivalent to `monitor reset` + `c`). These are individually trivial but cumulatively eliminate the "selected thread is gone" and "unsupported feature" spurious warnings that some IDE configurations promote to errors.

Phase 10 work shall continue to honour the layered architecture (§2.1) and the FSM-first principles (`doc/PVD.md §6.1`): no SW-breakpoint or watchpoint installation may unwind or rewrite FSM virtual-thread state, and `monitor reset` / `monitor chip-erase` shall always invalidate the FSM context before silicon side-effects so the next register/memory query sees a coherent thread list. The single-threaded `select()` event loop and `updi_*` primitive APIs are unchanged.

### 1.4 Definitions, Acronyms, and Abbreviations
*   **UPDI:** Unified Program and Debug Interface — the single-wire debug protocol used by AVR DA/DB-family microcontrollers. Carried over a UART at the configured baud rate with a 1 kΩ isolation resistor on the UPDI pin.
*   **RSP:** GDB Remote Serial Protocol — the text-based packet protocol that `avr-gdb` uses to communicate with a remote stub over a socket or serial link.
*   **FSM:** Finite State Machine — the cooperative task unit in avrOS, represented at runtime by a function pointer into a FLASH-resident state table. All FSMs share the single hardware stack.
*   **ELF:** Executable and Linkable Format — the binary file produced by `avr-gcc` that carries machine code, DWARF debug information, and the symbol table used to locate avrOS system tables.
*   **avrOS:** A cooperative real-time operating system for AVR microcontrollers. Tasks are modelled as FSMs; inter-task communication uses queues and named events.
*   **FSM Snapshot:** An internal record of the avrOS FSM registration table read from the target at halt -- per-FSM name, active flag, and current-state function pointer -- exposed to the developer via `monitor avros` introspection commands. avrOS FSMs are not presented as GDB threads.
*   **Harvard Architecture:** The AVR memory model, in which FLASH (program memory) and SRAM (data memory) occupy separate address spaces. ELF virtual addresses must be converted to physical FLASH word addresses during symbol resolution.

### 1.5 References
*   Product Vision Document: [doc/PVD.md](PVD.md)
*   GDB Remote Serial Protocol: https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html
*   Microchip AVR UPDI Programming Reference (AN3272): https://www.microchip.com/en-us/application-notes/an3272
*   System V ABI ELF Specification: https://refspecs.linuxfoundation.org/elf/elf.pdf
*   AVR-LibC Reference Manual: https://avrdudes.github.io/avr-libc/

### 1.6 Document Overview
*   Section 1: Introduction.
*   Section 2: System Overview.
*   Section 3: Detailed design for [src/main.c](../src/main.c).
*   Section 4: Detailed design for [src/updi.c](../src/updi.c).
*   Section 5: Detailed design for [src/gdb_rsp.c](../src/gdb_rsp.c).
*   Section 6: Detailed design for [src/elf_parser.c](../src/elf_parser.c).
*   Section 7: Detailed design for [src/fsm_mapper.c](../src/fsm_mapper.c).
*   Section 8: Detailed design for [src/monitor.c](../src/monitor.c).
*   Section 9: Data Dictionary.
*   Section 10: Traceability.

## 2. System Overview

### 2.1 System Architecture
`avrOSdb` is a single-process C99 application. Responsibilities are divided into six source modules arranged in four protocol layers; each layer depends only on the layers below it, and no upward calls are permitted. This layering is a hard design rule — see §2.2 "Layered Architecture" — and is enforced by review and by the `.h` dependency graph (the lower layers' headers must not `#include` any header from a higher layer).

**Layer stack (top to bottom):**

| # | Layer | Source modules | Owns | May call |
| - | ----- | -------------- | ---- | -------- |
| 4 | **Application** | `src/elf_parser.c`, `src/fsm_mapper.c`, `src/monitor.c` | ELF parsing, FSM-to-thread mapping, monitor verb dispatch | Layers 1–3 |
| 3 | **Protocol** | `src/gdb_rsp.c` | GDB RSP codec, packet dispatch table, GDB TCP socket lifecycle | Layers 1–2 (and Layer 4 callbacks via `RspHandlers`) |
| 2 | **Transport / Hardware** | `src/updi.c` | UART fd, UPDI framing, NVM programming, OCD register access, execution-control primitives | Layer 1 (libc / POSIX) |
| 1 | **Entry / Event Loop** | `src/main.c` | `AppConfig`, argv parsing, `select()` loop, signal handlers, resource teardown | All layers (top-of-stack orchestrator) |

Layer 1 (`src/main.c`) is the orchestrator and is permitted to reach into any module to wire fds and contexts; it sits notionally above the stack rather than inside it. Within layers 2–4, calls are strictly downward: Layer 4 may call Layer 2 for memory/OCD primitives, Layer 3 may call Layer 4 for FSM/monitor work via the `RspHandlers` callback table, but Layer 2 never calls Layers 3 or 4 and Layer 3 never calls Layer 1.

**Concurrency model:** The application is entirely single-threaded. All I/O multiplexing is performed by a single `select()` call inside `event_loop()`. No POSIX threads (`pthreads`) are used at any point. All module entry points are called synchronously and must return before the next I/O event can be processed. Long-blocking operations (e.g., NVM flash write, UPDI link initialisation) are therefore only permitted at startup or in response to explicit GDB commands, never in the event-loop hot path.

**Inter-module data flow:**

| Producing module | Data produced | Consuming module |
| ---------------- | ------------- | ---------------- |
| `src/main.c` | `AppConfig` (argv, fd values) | All modules (passed by pointer at init) |
| `src/updi.c` | Raw target memory bytes, execution-control status | `src/gdb_rsp.c`, `src/fsm_mapper.c`, `src/monitor.c` |
| `src/elf_parser.c` | `AvrOsSymbolIndex` (resolved FLASH/SRAM addresses) | `src/fsm_mapper.c`, `src/monitor.c` |
| `src/fsm_mapper.c` | `FsmContext` (virtual thread list, synthesized register frames) | `src/gdb_rsp.c` |
| `src/monitor.c` | Formatted O-packet text | `src/gdb_rsp.c` (via `rsp_send_packet()`) |
| `src/gdb_rsp.c` | Decoded RSP packet payload strings, thread-selection state | `src/updi.c`, `src/fsm_mapper.c`, `src/monitor.c` |

**Resource ownership:**

| Resource | Owner module | Lifetime | Open call | Close call |
| -------- | ------------ | -------- | --------- | ---------- |
| UART serial fd | `src/updi.c` | Process | `updi_open()` | `updi_close()` |
| GDB TCP listen socket | `src/gdb_rsp.c` | Process | `rsp_listen()` | `rsp_close()` |
| GDB TCP client socket | `src/gdb_rsp.c` | Session | `rsp_accept()` | detach / `rsp_close()` |
| ELF file descriptor + heap buffers | `src/elf_parser.c` | Session | `elf_open()` | `elf_close()` |
| `FsmContext` static array | `src/fsm_mapper.c` | Session | `fsm_build_thread_list()` | `fsm_invalidate()` |
| `AppConfig` struct | `src/main.c` (stack) | Process | `parse_args()` | implicit on `main()` return |

*   **[src/main.c](../src/main.c)** — Entry point; owns the top-level `select()`-based event loop and coordinates all other layers.
*   **[src/updi.c](../src/updi.c)** — UPDI physical layer; owns the UART serial file descriptor, UPDI protocol framing, execution-control primitives (halt, run, step), NVM flash programming, and the UPDI console bridge.
*   **[src/gdb_rsp.c](../src/gdb_rsp.c)** — GDB RSP layer; owns the GDB TCP socket, RSP packet codec, and command dispatch table.
*   **[src/elf_parser.c](../src/elf_parser.c)** — ELF parser; produces the `AvrOsSymbolIndex` (FLASH addresses of avrOS system tables) at attach time.
*   **[src/fsm_mapper.c](../src/fsm_mapper.c)** — FSM mapper; reads UPDI memory using the symbol index and exposes avrOS FSM state as introspection.
*   **[src/monitor.c](../src/monitor.c)** — Monitor handler; implements the custom `monitor avros events|queues` introspection commands without halting the CPU.

The startup and attach sequence proceeds as follows:

1.  `main()` parses argv, opens the UART serial device, and starts the GDB TCP listener socket.
2.  A GDB client connects; the RSP layer sends the initial hello and negotiates capabilities via `qSupported`.
3.  On the first `vAttach` or `?` packet, the ELF parser scans the supplied `.elf` file and populates the `AvrOsSymbolIndex` with avrOS FSM table addresses.
4.  The FSM mapper reads the FLASH-resident tables via UPDI background reads and builds the FSM introspection snapshot.
5.  Subsequent GDB packets (register reads, memory reads, breakpoints, step/continue) are dispatched through the RSP layer to the UPDI layer.
6.  `monitor avros` commands are intercepted by the monitor handler, which issues UPDI background reads without halting the CPU core.

### 2.2 Design Goals and Constraints
*   **Layered Architecture:** The source tree is organised into four protocol layers with strictly downward call direction (entry/event-loop → protocol → transport/hardware, with the application layer sitting above the protocol layer; see §2.1). A header in a lower layer shall not `#include` a header from a higher layer, and a lower-layer function shall not call a higher-layer function. The single permitted upward path is dependency-inversion via the `RspHandlers` callback table in `src/gdb_rsp.h`, which lets the protocol layer (Layer 3) invoke application-layer (Layer 4) code without taking a compile-time dependency on it. This rule is checked at review time: every new module shall declare its layer in this SDD, and every new `#include` shall be either same-layer or lower-layer.
*   **FSM-First Visualization:** Virtual threads map 1-to-1 to avrOS FSM table entries. The server never attempts to unwind dormant stack frames; execution context is defined solely by the FSM's current state function pointer.
*   **Lean Host Architecture:** The server is a single C99 process. Runtime dependencies are limited to libc and the POSIX serial and socket APIs. No Java, Python virtualization, or Electron runtime is required on the host.
*   **Editor-Agnostic Core:** The server exposes only standard GDB RSP. IDE-specific integration (Cortex-Debug, Zed DAP adapter) is handled entirely by the GDB client; the server does not implement any IDE extension protocol.
*   **Non-Intrusive Polling:** `monitor avros` commands use UPDI background reads, which can be issued while the CPU is running. The core is halted only by an explicit user breakpoint or a step/continue boundary.
*   **Fail-Safe Address Resolution:** If ELF symbol resolution fails to locate the avrOS system tables, the server degrades gracefully to a standard bare-metal GDB stub rather than refusing to connect.
*   **Single-Threaded Concurrency Model:** The server uses a single POSIX `select()` event loop with no POSIX threads. All module entry points are synchronous and return to the event loop promptly. Long-blocking operations (NVM programming, UPDI link initialisation) are permitted only at startup or in direct response to an explicit GDB command, never during the event-loop hot path.
*   **Deterministic Memory Allocation:** Static and stack allocation are used for all hot-path data structures (`AppConfig`, `FsmContext`, RSP packet buffers, breakpoint table). The only heap (`malloc`) usage is in `elf_open()` to load the ELF symbol and string tables; this allocation is bounded by the size of the ELF binary and occurs once per session at attach time. `elf_close()` frees all heap memory on detach.
*   **POSIX C99 Build:** The stub is compiled with a host C99 compiler (gcc ≥ 4.8 or clang ≥ 3.4) against the POSIX.1-2008 API. No compiler-specific extensions, no C11 atomics, and no dynamic libraries beyond libc are required. The Makefile produces a standalone executable with no runtime package dependencies.
*   **Modular Test Isolation:** Each module exposes a public C API declared in a matching `.h` header. The UPDI layer can be exercised against a loopback serial device or pre-recorded byte stream. The ELF parser and FSM mapper can be driven against any AVR ELF binary without live hardware. The RSP layer can be tested over a loopback TCP socket. No module requires a live AVR target to run its unit tests.

    A companion **on-target hardware integration harness** (`tests/hw/hw_test.c`) supplements the PTY-based unit tests by exercising the same UPDI public API against a real AVR-Dx target connected via a serial device. The harness is manual-only and is never invoked by `make test` (which must remain hardware-independent for CI). It is driven by the dedicated Makefile targets `hw-test`, `hw-test-nvm`, `hw-test-rsp`, and `hw-test-all`; destructive operations (FLASH erase/write, RSP-server spawn) are gated behind explicit opt-in variables.

## 3. Detailed Design for [src/main.c](../src/main.c)

### 3.1 Purpose and Responsibilities
[src/main.c](../src/main.c) is the entry point for the `avrOSdb` executable, providing CLI argument parsing, resource initialization, and the top-level `select()`-based event loop that multiplexes the GDB client socket and the UPDI serial device.

*   Define `main()` and parse command-line arguments into an `AppConfig` struct.
*   Open the UART serial device and pass the file descriptor to the UPDI layer.
*   Create the GDB listener socket and pass it to the RSP layer.
*   Run the event loop, dispatching each ready file descriptor to the appropriate layer until the GDB client disconnects.
*   Release all resources on exit and return an appropriate exit code.
*   When `--device` is supplied, execute the one-shot device-info diagnostic path: open UPDI, call `updi_read_device_info()`, print the formatted report to `stdout`, and return without entering `event_loop()`. Every failure on this path emits a verbose diagnostic on `stderr` that names the failed UPDI step.

### 3.2 External Interfaces
#### 3.2.1 Public C API

`src/main.c` has no exported public API; all functions are `static` or `main()` itself. It is the top-level consumer of all other module APIs. The only externally visible symbol is:

```c
extern volatile sig_atomic_t g_quit;  /* set to 1 by SIGINT/SIGTERM handler */
```

This variable is checked at the top of each `event_loop()` iteration and is also accessible to any module that needs to detect a shutdown-in-progress condition.

#### 3.2.2 Command-Line Arguments

`avrOSdb [--port <port>] [--baud <baud>] [--erase] [--load] [--allow-lock-updi] <serial-device> <elf-file>`
`avrOSdb --device [--baud <baud>] <serial-device> [elf-file]`

*   `--port <port>` — TCP port for the GDB listener (default: `1234`).
*   `--baud <baud>` — UART baud rate (default: `115200`).
*   `--erase` — issue a UPDI chip-erase before any `--load` step; required when the ELF contains a LOCK segment.
*   `--load` — program every `PT_LOAD` segment of the ELF to the matching NVM kind (FLASH, EEPROM, USERROW, FUSES, LOCK). SIGROW segments are skipped (read-only). Mutually exclusive with `--device`.
*   `--allow-lock-updi` — when set, `--load` is allowed to write LOCK byte patterns that would disable the UPDI interface (`UPDIDIS`); without this flag only the 4-byte unlock pattern `0x5CC5C55C` is accepted.
*   `--device` — one-shot diagnostic mode: open the UPDI link, read the target SIGROW and ASI status, print a verbose human-readable report to `stdout`, and exit without binding the GDB listener. `<elf-file>` is optional in this mode. Mutually exclusive with `--load`.
*   `<serial-device>` — path to the UART device (e.g. `/dev/ttyUSB0`).
*   `<elf-file>` — path to the AVR ELF binary.


### 3.3 Internal Structure
#### 3.3.1 Key Data Structures

The event loop multiplexes three file descriptors using POSIX `select()`:

*   **`int listen_fd`** — passive TCP listener socket; present in `fd_set` at all times.
*   **`int gdb_fd`** — active GDB client socket; `-1` when no client is connected; added to `fd_set` only while a session is active.
*   **`int updi_fd`** — UART serial device; added to `fd_set` while a GDB session is active to forward unsolicited console traffic.

All three file descriptors are stored directly in `AppConfig`. The maximum fd value is recomputed before each `select()` call to keep the fd_set range tight.


#### 3.3.2 Key Functions

*   **`int main(int argc, char *argv[])`** — Parse argv, open resources, run the event loop, and return an exit code.
*   **`static void parse_args(int argc, char *argv[], AppConfig *cfg)`** — Walk argv and populate cfg; print usage to stderr and exit on error.
*   **`static int run_device_mode(const AppConfig *cfg)`**
    *   Purpose: Diagnostic path taken when `cfg->device_info` is true. Opens the UPDI link, invokes `updi_read_device_info()`, prints a verbose human-readable report to `stdout`, and returns the process exit code (0 on success, 1 on any UPDI failure). Skips ELF loading, `rsp_listen()`, and the entire event loop.
    *   Pre-condition: `cfg->serial_device` is non-NULL; `cfg->baud_rate` is positive; `cfg->device_info` is true.
    *   Post-condition: UPDI fd is closed before return; no other resources are allocated.
    *   Return Value: 0 on success; 1 on any failure (UPDI open, BREAK/SYNCH, or any SIGROW/ASI read).
    *   Logic:
        1.  `updi_open(cfg->serial_device, cfg->baud_rate)` \u2014 on failure, print `"error: cannot open UPDI device '<dev>': <errno-text>"` to `stderr` and return 1.
        2.  `updi_read_device_info()` \u2014 on failure, print `"error: <fail_op> failed (rc=<fail_errno>) \u2014 check wiring, target power, UPDIDIS fuse"` to `stderr`, close UPDI, and return 1.
        3.  Look up the 3-byte signature in the static `device_family[]` table; print `Serial device`, `Baud rate`, `Signature`, `Family`, `Revision`, `Serial`, and `UPDI status` lines to `stdout`.
        4.  Close UPDI and return 0.

*   **`static void event_loop(AppConfig *cfg)`**
    *   Purpose: Run the top-level select()-based multiplexer until the process receives a termination signal.
    *   Pre-condition: `cfg->listen_fd` and `cfg->updi_fd` are valid open file descriptors; `cfg->gdb_fd` is -1.
    *   Logic:
        1.  Build an `fd_set` from `listen_fd` and, when `gdb_fd >= 0`, from `gdb_fd` and `updi_fd`.
        2.  Call `select()` with no timeout; block until at least one descriptor becomes readable.
        3.  If `listen_fd` is ready and `gdb_fd` is -1, call `rsp_accept()` to accept the incoming GDB client; store the returned fd in `cfg->gdb_fd` and call `elf_find_avros_tables()` / `fsm_build_thread_list()` to initialise the session.
        4.  If `updi_fd` is ready, call `updi_console_poll()` and write any returned bytes to `stdout`.
        5.  If `gdb_fd` is ready, call `rsp_recv_packet()` followed by `rsp_dispatch()`; on a zero-byte read (client disconnect) close `gdb_fd` and reset it to -1.
        6.  Repeat until a SIGINT or SIGTERM is caught, then fall through to resource cleanup.


#### 3.3.3 Parsing Strategy / Algorithm

**Signal handling:** `main()` registers a `SIGINT`/`SIGTERM` handler that sets a global `volatile sig_atomic_t g_quit` flag. The `event_loop()` tests this flag at the top of each iteration and returns cleanly, allowing `main()` to call `updi_close()`, `rsp_close()`, and `elf_close()` before `exit(0)`.

If the `--load` flag is set, `main()` invokes `updi_nvm_write_flash()` for each ELF `PT_LOAD` segment before entering `event_loop()`. A progress line is printed to `stdout` for each FLASH page written.

**Exit code table:**

| Exit code | Condition |
| --------- | --------- |
| `0` | Normal exit after SIGINT/SIGTERM or GDB `k` (kill) packet. |
| `1` | Fatal startup error: bad arguments, serial device open failure, or socket bind failure. |
| `1` | `--load` flash write failure (UPDI error or write-protection). |
| `0` | `--device` diagnostic mode completed; device info printed to stdout. |
| `1` | `--device` diagnostic mode failed at any UPDI step (BREAK/SYNCH, SIGROW read, ASI read). |

**Resource teardown sequence** (guaranteed order on all exit paths):

1.  `rsp_close(cfg.gdb_fd)` — close the active client socket if present.
2.  `rsp_close(cfg.listen_fd)` — close the listener socket.
3.  `elf_close(&ctx)` — free ELF heap buffers and close ELF file descriptor.
4.  `updi_close(cfg.updi_fd)` — drain and close the serial port.
5.  `return exit_code` from `main()`.

**Test approach for `src/main.c`:** Integration-level only. Verified by launching the compiled binary with valid and invalid argument combinations and checking exit codes and stderr output. Full session flow is tested by driving a GDB client script against a loopback serial device.

### 3.4 Dependencies

*   `src/updi.c` — `updi_open()` / `updi_close()` / `updi_console_poll()`
*   `src/gdb_rsp.c` — `rsp_listen()` / `rsp_accept()` / `rsp_recv_packet()` / `rsp_dispatch()` / `rsp_close()`
*   `src/elf_parser.c` — `elf_open()` / `elf_find_avros_tables()` / `elf_close()`
*   `src/fsm_mapper.c` — `fsm_build_thread_list()`

### 3.5 Error Handling and Logging

*   **Missing or invalid arguments** Print usage string to `stderr` and `exit(1)`.
*   **Serial device open failure** Print `strerror(errno)` to `stderr` and `exit(1)`.
*   **GDB socket bind failure** Print `strerror(errno)` to `stderr` and `exit(1)`.
*   **--load flash write failure** Print the UPDI error message to `stderr` and `exit(1)`; the target is left in an indeterminate state.
*   **--device combined with --load** `parse_args()` detects the conflict, prints `"error: --device is mutually exclusive with --load"` to `stderr`, prints the usage line, and calls `exit(1)`.
*   **--device UPDI link failure** `run_device_mode()` emits a verbose `stderr` diagnostic naming the failed UPDI step (open, BREAK, SYNCH, SIGROW read, ASI read) and returns 1. No GDB listener is bound; no event loop is entered.

## 4. Detailed Design for [src/updi.c](../src/updi.c)

### 4.1 Purpose and Responsibilities
[src/updi.c](../src/updi.c) implements the UPDI physical layer, managing the UART serial port and encoding/decoding UPDI protocol frames for memory access, execution control, NVM flash programming, and UPDI-based console bridging.

*   Open, configure (8E2, half-duplex), and close the UART serial device. On hosts where the underlying device cannot honour even parity (notably Linux PTY slaves used by the unit-test harness), fall back to 8N2 so the link still works in test environments.
*   Encode and transmit UPDI commands (`STCS`, `LDCS`, `ST`, `LD`, `KEY`, `REPEAT`, `ST_PTR_WORD`, `ST_PTR_LONG`) over the UART.
*   Receive UPDI response frames and validate ACK/NAK bytes.
*   Provide non-intrusive background memory reads while the CPU is running.
*   Implement NVM write sequences for FLASH page erase and program.
*   Expose execution-control entry points (`updi_halt`, `updi_run`, `updi_step`) as -1 stubs; the full halt/run/step implementation is deferred to the Phase 3 OCD layer, which carries its own Microchip specification independent of the §35 UPDI document.
*   Bridge the avrOS software UART console channel to host stdout for CLI and automated test use.

### 4.2 External Interfaces
#### 4.2.1 Public C API (src/updi.h)

All functions exported from `src/updi.c` and declared in `src/updi.h`:

```c
/* Lifecycle */
int  updi_open(const char *device, int baud);
void updi_close(int fd);

/* Memory access */
int  updi_mem_read (int fd, uint32_t addr,       uint8_t *buf, size_t len);
int  updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len);

/* Execution control */
int  updi_halt(int fd);
int  updi_run (int fd);
int  updi_step(int fd);

/* NVM programming */
int  updi_nvm_write_flash(int fd, uint32_t word_addr,
                          const uint8_t *data, size_t len);

/* Console bridge */
int  updi_console_poll(int fd, char *buf, size_t cap);

/* Device-signature diagnostics (Phase 7) */
typedef struct {
    uint8_t  device_id[3];     /* SIGROW DEVICEID0..2 @ 0x1100-0x1102 (DS §7.6.1)  */
    uint8_t  revid;            /* SYSCFG.REVID        @ 0x0F01        (DS §8.3.2.1) */
    uint8_t  serial[16];       /* SIGROW SERNUM0..15  @ 0x1110-0x111F (DS §7.6.2.3) */
    uint8_t  asi_sys_status;
    uint8_t  asi_key_status;
    uint8_t  asi_statusb;
    const char *fail_op;       /* NULL on success      */
    int         fail_errno;    /* negative updi error  */
} UpdiDeviceInfo;

int  updi_read_device_info(int fd, UpdiDeviceInfo *info);
```

Error return convention: all functions return 0 on success and -1 on error (errno not set); `updi_nvm_write_flash()` additionally returns `UPDI_ERR_WP` (-2) when FLASH write-protection is active.

#### 4.2.2 Hardware Connection

Half-duplex UART at the configured baud rate (default 115200). The AVR UPDI pin is connected to both TX and RX on the host adapter through a 1 kΩ isolation resistor. The UPDI protocol requires 8E2 framing (8 data bits, even parity, 2 stop bits) per AVR128DA datasheet §35.3.1.


### 4.3 Internal Structure
#### 4.3.1 Key Data Structures

UPDI protocol constants used throughout the module:

| Constant | Value | Description |
| -------- | ----- | ----------- |
| `UPDI_SYNCH`       | `0x55` | UPDI synchronisation character sent after BREAK. |
| `UPDI_ACK`         | `0x40` | Acknowledgement byte returned by the target after each data byte. |
| `UPDI_MAX_BLOCK`   | `256`  | Maximum bytes transferred in a single REPEAT+LD/ST burst. |

UPDI ASI (Application System Interface) Control/Status register map (AVR128DA §35.4):

| Register | Offset | Description |
| -------- | ------ | ----------- |
| `ASI_STATUSB`     | `0x01` | Status B; reading clears the PESIG bit set on every BREAK. |
| `ASI_CTRLA`       | `0x02` | UPDI control register A; bit 2 = IBD (inter-byte delay enable). |
| `ASI_CTRLB`       | `0x03` | UPDI control register B. |
| `ASI_KEY_STATUS`  | `0x07` | NVM-key acceptance status. |
| `ASI_RESET_REQ`   | `0x08` | Write `0x59` to assert CPU reset; write `0x00` to release. |
| `ASI_SYS_CTRLA`   | `0x0A` | System control A. |
| `ASI_SYS_STATUS`  | `0x0B` | System status; bit 3 (`0x08`) = NVMPROG (NVM programming mode active). |

NVM controller registers (accessed via UPDI ST/LD at base address `0x1000`):

| Register | Offset | Description |
| -------- | ------ | ----------- |
| `NVMCTRL_CTRLA`  | `0x00` | NVM command register; write command code to initiate NVM operation. |
| `NVMCTRL_STATUS` | `0x02` | Status register; bit 0 = BUSY. |

AVR-Dx OCD (On-Chip Debug) register block — accessed via UPDI LDS/STS at UPDI base `0x0F80`. The CPU must be halted before reading or writing these registers; behaviour against a running CPU is undefined.

| Register | UPDI address | Width | Description |
| -------- | ------------ | ----- | ----------- |
| `OCD_BP0A`    | `0x0F80` | 3 B | Hardware breakpoint 0 address (LE byte address). |
| `OCD_BP1A`    | `0x0F84` | 3 B | Hardware breakpoint 1 address (LE byte address). |
| `OCD_CTRL0`   | `0x0F88` | 1 B | OCD control 0; `OCD_CTRL0_HWBP` (0x01) global HW-BP enable, `OCD_CTRL0_STEP` (0x04) arms single-step on next RUN. |
| `OCD_CTRL1`   | `0x0F89` | 1 B | OCD control 1; `OCD_CTRL1_BP0` (0x01) and `OCD_CTRL1_BP1` (0x02) enable individual comparators. |
| `OCD_STATUS0` | `0x0F8C` | 1 B | OCD halt status 0. |
| `OCD_STATUS1` | `0x0F8D` | 1 B | OCD halt status 1; `OCD_STATUS1_EXTBRK` (0x10) = external break / STOP / Ctrl-C halt cause. |
| `OCD_PC`      | `0x0F94` | 2 B | Program counter (word-aligned PC+1 storage; converted to/from GDB byte address by `updi_ocd_read_pc()` / `updi_ocd_write_pc()`). |
| `OCD_SP`      | `0x0F98` | 2 B | Stack pointer (little-endian SPL\|SPH). |
| `OCD_SREG`    | `0x0F9C` | 1 B | Status register. |
| `OCD_REGFILE` | `0x0FA0` | 32 B | General-purpose register file r0..r31. |

UPDI CS-space OCD registers (accessed via LDCS/STCS):

| CS register | Offset | Description |
| ----------- | ------ | ----------- |
| `ASI_OCD_CTRLA`  | `0x04` | OCD control: write `ASI_OCD_CTRLA_STOP` (0x01) to halt, `ASI_OCD_CTRLA_RUN` (0x02) to resume. |
| `ASI_OCD_STATUS` | `0x05` | OCD status; `ASI_OCD_STATUS_STOPPED` (0x01) = CPU halted. |


#### 4.3.2 Key Functions

*   **`int updi_open(const char *device, int baud)`** — Open and configure the serial port for UPDI; return fd or -1 on error.
*   **`void updi_close(int fd)`** — Drain pending output and close the serial port.
*   **`int updi_mem_read(int fd, uint32_t addr, uint8_t *buf, size_t len)`**
    *   Purpose: Read len bytes from target address addr via UPDI LD commands; supports both SRAM and FLASH address spaces.
    *   Pre-condition: `fd` is a valid UPDI serial file descriptor; `buf` points to a caller-allocated buffer of at least `len` bytes; the UPDI link has been initialised by `updi_open()`.
    *   Post-condition: On success, `buf` contains the verbatim target memory contents at the requested address range.
    *   Return Value: 0 on success; -1 on UART framing error, timeout, or UPDI NAK.
    *   Logic:
        1.  If `len` is greater than `UPDI_MAX_BLOCK`, split the read into consecutive transactions each transferring up to `UPDI_MAX_BLOCK` bytes.
        2.  For each block, transmit three separate UPDI frames (datasheet §35.3.3.4): (a) the pointer-set frame — `ST_PTR_WORD` (`0x69`) followed by the 16-bit target address in little-endian order when `addr ≤ 0xFFFF`, or `ST_PTR_LONG` (`0x6A`) followed by the 24-bit address in little-endian order when `addr > 0xFFFF` (required for the AVR128DA/DB mapped-Flash region above 64 KiB) — then read the single ACK (`0x40`); (b) `REPEAT` (`0xA0`) followed by `(block_len - 1)`; (c) `LD ptr++` (`0x24`).
        3.  Use `select()` with a 100 ms timeout before each `read()`; accumulate `block_len` data bytes into the destination buffer.
        4.  Return -1 immediately on any `select()` timeout or framing error (unexpected byte in place of ACK).

*   **`int updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len)`** — Write len bytes to target address addr via UPDI ST; return 0 or -1.
*   **`int updi_halt(int fd)`**
    *   Purpose: Halt the AVR-Dx CPU by writing `ASI_OCD_CTRLA_STOP` (0x01) to `ASI_OCD_CTRLA` (UPDI CS `0x04`) via STCS, then polling `ASI_OCD_STATUS` via LDCS until `ASI_OCD_STATUS_STOPPED` (0x01) is observed.
    *   Return Value: 0 on success; -1 on UPDI failure or if STOPPED is not asserted within the LDCS poll budget.

*   **`int updi_run(int fd)`** — Resume the halted CPU by writing ASI_OCD_CTRLA_RUN (0x02) to ASI_OCD_CTRLA via STCS; returns 0 immediately without polling. Callers that need to wait for a halt use updi_ocd_poll_halted().
*   **`int updi_step(int fd)`** — Single-step one instruction: set OCD_CTRL0_STEP in OCD_CTRL0 via STS, write ASI_OCD_CTRLA_RUN to ASI_OCD_CTRLA via STCS, then poll ASI_OCD_STATUS until STOPPED. Returns 0 or -1.
*   **`int updi_enter_debug(int fd)`** — Arm AVR-Dx OCD mode for the lifetime of the session: send the UPDI KEY opcode + 8-byte 'OCD     ' key, pulse ASI_RESET_REQ (0x59 then 0x00), and poll ASI_OCD_STATUS for STOPPED. Called once per session from main.c after updi_open() and any --load step.
*   **`int updi_ocd_poll_halted(int fd, int timeout_ms)`** — Poll ASI_OCD_STATUS at ~1 ms intervals for the STOPPED bit. Returns 0 if STOPPED, +1 on timeout (CPU still running), -1 on UPDI failure.
*   **`int updi_ocd_read_halt_status(int fd, uint8_t *status0, uint8_t *status1)`** — Read OCD_STATUS0 (0x0F8C) and OCD_STATUS1 (0x0F8D) via UPDI LDS into the caller's bytes. OCD_STATUS1.EXTBRK (0x10) distinguishes external-break / Ctrl-C halts from hardware-breakpoint, single-step, BREAK-opcode, change-of-flow, and interrupt halts.
*   **`int updi_ocd_read_gpr(int fd, uint8_t n, uint8_t *val)`** — Read AVR GPR r<n> (0..31) via UPDI LDS at OCD_REGFILE + n (0x0FA0 + n). CPU must be halted.
*   **`int updi_ocd_write_gpr(int fd, uint8_t n, uint8_t val)`** — Write AVR GPR r<n> via UPDI STS at OCD_REGFILE + n. CPU must be halted.
*   **`int updi_ocd_read_sreg(int fd, uint8_t *val)`** — Read AVR SREG via UPDI LDS at OCD_SREG (0x0F9C).
*   **`int updi_ocd_write_sreg(int fd, uint8_t val)`** — Write AVR SREG via UPDI STS at OCD_SREG.
*   **`int updi_ocd_read_sp(int fd, uint16_t *val)`** — Read AVR SP (little-endian SPL|SPH) via UPDI LDS at OCD_SP (0x0F98).
*   **`int updi_ocd_write_sp(int fd, uint16_t val)`** — Write AVR SP via UPDI STS at OCD_SP.
*   **`int updi_ocd_read_pc(int fd, uint32_t *byte_addr)`** — Read OCD_PC (0x0F94) via UPDI LDS and convert the silicon's word-aligned PC+1 storage to a GDB byte address (shift left 1, undo the AVR +1 post-increment).
*   **`int updi_ocd_write_pc(int fd, uint32_t byte_addr)`** — Convert a GDB byte address back to the word-aligned PC+1 form and write OCD_PC via UPDI STS.
*   **`int updi_ocd_set_hw_bp(int fd, uint8_t idx, uint32_t byte_addr)`** — Program hardware-breakpoint comparator idx (0 or 1) to a GDB byte address: write the 3-byte LE address to OCD_BP0A/OCD_BP1A via UPDI STS, set the matching OCD_CTRL1_BP0/OCD_CTRL1_BP1 enable bit, and set the global OCD_CTRL0_HWBP enable bit.
*   **`int updi_ocd_clear_hw_bp(int fd, uint8_t idx)`** — Disable hardware-breakpoint comparator idx by clearing the matching OCD_CTRL1_BP0/OCD_CTRL1_BP1 bit via UPDI LDS/STS read-modify-write on OCD_CTRL1.
*   **`int updi_nvm_write_eeprom(int fd, uint32_t addr, const uint8_t *data, size_t len)`** — Program EEPROM (UPDI window 0x814000–0x8143FF) byte-by-byte using NVMCTRL command EEERWR (0x13), polling NVMSTATUS.EEBUSY (bit 1) between bytes. Window-checks addr/len; returns -1 on out-of-window or BUSY timeout.
*   **`int updi_nvm_write_userrow(int fd, uint32_t addr, const uint8_t *data, size_t len)`** — Program USERROW (UPDI window 0x810080–0x8100FF, 128 B) using the same EEERWR + EEBUSY-poll sequence as updi_nvm_write_eeprom.
*   **`int updi_nvm_write_fuses(int fd, uint32_t addr, const uint8_t *data, size_t len)`** — Program FUSES (UPDI window 0x820000–0x82001F, 32 B) using the same EEERWR + EEBUSY-poll sequence as updi_nvm_write_eeprom.
*   **`int updi_nvm_write_lockbits(int fd, uint32_t addr, const uint8_t *data, size_t len, bool allow_updi_disable)`** — Program LOCK (UPDI window 0x820040–0x820043, 4 B). Returns UPDI_ERR_LOCKED (-3) if ASI_SYS_STATUS.LOCKSTATUS is asserted, or if !allow_updi_disable and the 4-byte little-endian payload is not the unlock pattern UPDI_LOCK_UNLOCKED (0x5CC5C55C). Otherwise programs via EEERWR + EEBUSY poll.
*   **`int updi_nvm_write_flash(int fd, uint32_t word_addr, const uint8_t *data, size_t len)`**
    *   Purpose: Erase and program one or more FLASH pages starting at word_addr using the UPDI NVM controller write-page sequence.
    *   Pre-condition: Target CPU is halted (`updi_halt()` has been called); `word_addr` is aligned to a FLASH page boundary; `len` is a non-zero multiple of the target FLASH page size (512 bytes for AVR DA/DB).
    *   Return Value: 0 on success; -1 on NVM timeout or UPDI communication error; `UPDI_ERR_WP` (a distinct negative constant) if FLASH write protection is detected in `NVMCTRL_STATUS`.
    *   Logic:
        1.  Transmit the UPDI `KEY` opcode (`0xE0`) followed by the 8-byte NVM key string `"NVMProg "`.
        2.  `STCS ASI_RESET_REQ = 0x59` to assert reset, then `STCS ASI_RESET_REQ = 0x00` to release reset (per AVR128DA §35.3.7.2).
        3.  Poll `ASI_SYS_STATUS` via LDCS until bit 3 (NVMPROG, mask `0x08`) is set; timeout after 100 polling iterations (1 ms each).
        4.  For each FLASH page: write the page-sized data block via `updi_mem_write()` (which itself uses the 3-frame `ST_PTR_WORD` + `REPEAT` + `ST ptr++` burst); then issue the NVMCTRL ERWP (Erase + Write Page) command by writing `0x03` to `NVMCTRL_CTRLA` (`0x1000`).
        5.  Poll `NVMCTRL_STATUS` (`0x1002`) bit 0 (BUSY) via `updi_mem_read()` until clear; timeout after 20 iterations per page.
        6.  Check `NVMCTRL_STATUS` bit 2 (WRERR); if set, return `UPDI_ERR_WP`.
        7.  After all pages are written, exit NVM mode with `STCS ASI_RESET_REQ = 0x59` followed by `STCS ASI_RESET_REQ = 0x00`.

*   **`int updi_console_poll(int fd, char *buf, size_t cap)`** — Poll the UPDI console channel and copy any pending bytes to buf; return byte count or -1.
*   **`int updi_read_device_info(int fd, UpdiDeviceInfo *info)`**
    *   Purpose: Read the AVR-Dx SIGROW.DEVICEID0..2 at `0x1100`–`0x1102` (datasheet §7.6.1, Table 7-4), the SYSCFG.REVID byte at `0x0F01` (datasheet §8.3.2.1; SYSCFG base `0x0F00`), the 16-byte SIGROW.SERNUM0..15 at `0x1110`–`0x111F` (datasheet §7.6.2.3), and the UPDI ASI status registers (`ASI_SYS_STATUS`, `ASI_KEY_STATUS`, `ASI_STATUSB`), populating the caller-supplied `UpdiDeviceInfo` struct. The operation is non-destructive — the CPU is **not** halted and no NVM activity is initiated.
    *   Pre-condition: `fd` is a valid UPDI serial file descriptor; `info` points to a caller-allocated `UpdiDeviceInfo` struct; the UPDI link has been initialised by `updi_open()`.
    *   Post-condition: On success, every field of `*info` carries verbatim bytes from the target; on failure, `info->fail_op` carries a short identifier of the failed UPDI step (one of `"break"`, `"synch"`, `"asi-statusb"`, `"sigrow"`, `"revid"`, `"sernum"`, `"sys-status"`, `"key-status"`) and `info->fail_errno` carries either the negative `updi_mem_read` return or 0.
    *   Return Value: 0 on success; -1 on any UPDI read failure.
    *   Logic:
        1.  Read three bytes at `0x1100` via `updi_mem_read()` into `info->device_id[0..2]`; on failure record `"sigrow"` and return -1.
        2.  Read one byte at `0x0F01` (SYSCFG.REVID, datasheet §8.3.2.1) into `info->revid`; on failure record `"revid"` and return -1.
        3.  Read sixteen bytes at `0x1110` (SIGROW.SERNUM0..15, datasheet §7.6.2.3) into `info->serial[0..15]`; on failure record `"sernum"` and return -1.
        4.  Read `ASI_SYS_STATUS`, `ASI_KEY_STATUS`, and `ASI_STATUSB` via three `LDCS` opcodes; record `"sys-status"` / `"key-status"` / `"asi-statusb"` on failure.
        5.  On success, clear `info->fail_op` to `NULL` and return 0.


#### 4.3.3 Parsing Strategy / Algorithm

**UPDI link initialisation sequence (performed inside `updi_open()`):**

1.  Open the serial port with `O_RDWR | O_NOCTTY | O_NONBLOCK`, then restore blocking mode via `fcntl()`.
2.  Configure `termios` for raw 8E2 mode: `cfmakeraw()`, set `CS8`, `CSTOPB`, `PARENB` (with `PARODD` cleared = even parity), disable flow control, and apply `cfsetispeed()` / `cfsetospeed()` for the operating baud rate.
3.  If the kernel rejects `PARENB` with `EINVAL` (Linux PTY slaves silently strip parity from virtual terminals), retry the `tcsetattr()` call with `PARENB` cleared. Real serial hardware accepts 8E2; the fallback only affects PTY-backed unit tests.
4.  Run up to three cold-start attempts that mirror avrdude's `serialupdi` programmer (verified bit-for-bit by `strace`-ing avrdude against the same target):
    - **Attempt 0 — fast path** (succeeds against an already-prepped target, e.g. immediately after a previous successful session): write one `0x00` wake byte at session baud, then `tcdrain()` + `tcflush(TCIFLUSH)`.
    - **Attempts 1, 2 — slow path** (required on cold power-on with passive single-wire combiners such as the RPi PL011): switch the kernel baud to 300 with `CSTOPB` cleared (1 stop bit) via `tcsetattr(TCSADRAIN)`, write two `0x00` bytes — each immediately followed by `tcdrain()` (mandatory: without it the queued byte is flushed or re-clocked by the subsequent baud switch, destroying the BREAK pulse) — and read-and-discard their half-duplex echoes at 300 baud (each byte holds TX low ≈ 33 ms = ≥ 60 ms total). Wait ≈ 50 ms (`nanosleep`) so any line-bus echoes still in flight reach the kernel RX queue at 300-baud framing, then `tcflush(TCIFLUSH)` to discard them, then restore session baud (`tcsetattr(TCSADRAIN)` + a second `tcflush(TCIFLUSH)`).
    - For every attempt, follow the wake / BREAK with three frames: `STCS ASI_CTRLB = ASI_CTRLB_CCDETDIS (0x08)` (disable contention detection), `STCS ASI_CTRLA = ASI_CTRLA_IBDLY (0x80)` (enable inter-byte delay on responses), and `LDCS ASI_STATUSA` (link probe — `ASI_STATUSA` carries UPDIREV and is always readable while UPDI is enabled). If all three succeed, read the 32-byte System Information Block (`SYNCH` + `UPDI_OP_KEY_SIB` `0xE6`, followed by `UPDI_SIB_LEN = 32` response bytes) to wake any target left in UPDI SLEEP from a prior session and to confirm the link end-to-end; on success the function returns the open fd. Otherwise it advances to the next attempt.
5.  After three failed attempts the function returns -1.

**Why the 300-baud BREAK trick is necessary:** No portable POSIX API generates a multi-millisecond line-low BREAK on demand. `tcsendbreak()` is too short (and on the RPi PL011 driver is silently ignored). Switching the kernel baud rate is the only mechanism that reliably stretches a single `0x00` byte to ≥ 30 ms of line-low time, which is what cold-power AVR-Dx UPDI requires to clear its contention-detect latch.

**Why the SIB-read SLEEP wake is necessary:** A target left in UPDI SLEEP from a prior debug session will accept every link-layer probe (STCS, LDCS) — those frames merely echo on the half-duplex line — yet reject every memory access, manifesting as `updi_mem_read()` returning -1 immediately after a successful-looking `updi_open()`. avrdude reads the 32-byte SIB unconditionally on every connect; the act of issuing a key-opcode-class frame (`0xE6`) is what wakes the target. The 32-byte payload itself (e.g. `"    AVR P:2D:1-3M2 (A7.KV001.0)\0"`) is discarded.

**Half-duplex echo cancellation:** Because TX and RX share the same physical wire, every transmitted byte is echoed back on the RX line. All UPDI transmit helpers skip one echo byte per transmitted byte before reading response data.

**UPDI timing budget:**

| Operation | Timeout | Notes |
| --------- | ------- | ----- |
| `updi_mem_read()` per-byte | 100 ms (`select()`) | A `select()` with `timeval { 0, 100000 }` guards every `read()`. |
| `updi_nvm_write_flash()` NVMPROG wait | ≈ 100 ms | 100 LDCS polls of `ASI_SYS_STATUS` at 1 ms intervals. |
| `updi_nvm_write_flash()` page BUSY poll | ≈ 20 ms per page | 20 reads of `NVMCTRL_STATUS` (bit 0 = BUSY). |
| Cold-start slow-path BREAK | ≥ ≈ 60 ms per attempt | Two `0x00` bytes at 300 baud, mirroring avrdude's serialupdi. |

Note: `updi_halt()`, `updi_run()`, `updi_step()`, and `updi_ocd_poll_halted()` use a bounded LDCS poll on `ASI_OCD_STATUS` (typical halt latency ≤ 1 ms after `STCS ASI_OCD_CTRLA = STOP`); `updi_ocd_poll_halted()` exposes the deadline as a millisecond argument so the RSP `c` handler can multiplex OCD polling with a `select()` on the GDB client socket.

**UPDI command opcode encoding:** Each command frame begins with a single command byte followed by its operands. Key opcodes (AVR128DA §35.3.3):

| Opcode mnemonic | Byte value | Purpose |
| --------------- | ---------- | ------- |
| `LDCS rd, cs`   | `0x80\|cs` | Load Control/Status register `cs`. |
| `STCS cs, rr`   | `0xC0\|cs` | Store to Control/Status register `cs`. |
| `ST_PTR_WORD`   | `0x69`     | Set the burst pointer with a 16-bit address operand (precedes REPEAT/LD/ST bursts for addresses ≤ 0xFFFF). |
| `ST_PTR_LONG`   | `0x6A`     | Set the burst pointer with a 24-bit address operand (used for addresses > 0xFFFF, e.g. AVR128DA/DB mapped Flash). |
| `ST ptr++, rr`  | `0x64`     | Store with pointer post-increment (burst write). |
| `LD rd, ptr++`  | `0x24`     | Load with pointer post-increment (burst read). |
| `REPEAT`        | `0xA0`     | Set repeat count for the next bulk transfer (n-1 in operand). |
| `KEY`           | `0xE0`     | Transmit 8-byte key to unlock a privileged mode. |

**Test approach for `src/updi.c`:** Unit-testable using a POSIX pseudo-terminal pair (`openpty()`): one end is passed to `updi_open()`, the other is driven by the test harness. Test cases cover: termios 8E2 configuration (with PARENB-fallback under PTYs), the cold-start wake-byte/STCS-CCDETDIS/STCS-IBDLY/LDCS-STATUSA probe and its 3-retry policy, the 3-frame `ST_PTR_WORD`/`REPEAT`/`LD-or-ST` burst, the 100 ms `select()` deadline, NVM precondition checks, NVMPROG and per-page BUSY timeouts, and the OCD control primitives (`updi_enter_debug`, `updi_halt`, `updi_run`, `updi_step`, `updi_ocd_poll_halted`, `updi_ocd_read_halt_status`, and the OCD register-file readers/writers).

### 4.4 Dependencies

*   POSIX `termios` serial API (`tcgetattr`, `tcsetattr`, `cfsetspeed`, `cfmakeraw`).
*   Standard POSIX file I/O (`open`, `read`, `write`, `close`, `fcntl`, `tcdrain`).

### 4.5 Error Handling and Logging

*   **UART framing error or UPDI NAK** Return -1 to the caller; the caller decides whether to retry or abort the session.
*   **NVM write protection** Return `UPDI_ERR_WP` so the caller can report `"Flash write-protected"` to the GDB console.
*   **UPDI link lost mid-session** Any `read()` returning 0 or any unexpected byte sequence causes the current operation to return -1; the RSP layer then sends a GDB error packet and awaits the next client command.

## 5. Detailed Design for [src/gdb_rsp.c](../src/gdb_rsp.c)

### 5.1 Purpose and Responsibilities
[src/gdb_rsp.c](../src/gdb_rsp.c) implements the GDB Remote Serial Protocol server, accepting a single GDB client connection on a TCP port, decoding RSP packets, dispatching them to command handlers, and encoding response packets.

*   Bind and accept a single GDB client connection on the configured TCP port.
*   Decode incoming RSP packets (`$<data>#<checksum>`), verify checksums, and send ACK (`+`) or NAK (`-`).
*   Dispatch decoded packets to command handlers for register access, memory access, breakpoints, step/continue, and monitor commands.
*   Encode and transmit RSP response packets.
*   Handle lifecycle packets: `qSupported`, `qAttached`, `vAttach`, `D` (detach), and `k` (kill).

### 5.2 External Interfaces
#### 5.2.1 Public C API (src/gdb_rsp.h)

All functions exported from `src/gdb_rsp.c` and declared in `src/gdb_rsp.h`:

```c
/* Lifecycle */
int  rsp_listen(uint16_t port);      /* create + bind + listen; return fd or -1 */
int  rsp_accept(int listen_fd);      /* block until client; return client fd */
void rsp_close (int fd);             /* close client or listen fd */

/* Packet codec */
int  rsp_recv_packet(int fd, char *buf, size_t cap);
    /* Returns: payload byte count; 0 on disconnect; -1 on error */
int  rsp_send_packet(int fd, const char *payload);
    /* Returns: 0 on success; -1 on write error */

/* No-ack mode (toggled by QStartNoAckMode) */
void rsp_set_noack(bool enabled);
bool rsp_get_noack(void);

/* Dispatch */
int  rsp_dispatch(int fd, const char *packet, RspHandlers *h);
    /* Returns: 0 on success; -1 on write error from a handler */

/* Default handlers (used by main; tests may override individual slots) */
void rsp_default_handlers(RspHandlers *h, RspContext *ctx);
```

The `RspHandlers` and `RspContext` structs are declared in `src/gdb_rsp.h`. The caller normally invokes `rsp_default_handlers()` to populate `RspHandlers` with the production handler set before passing it to `rsp_dispatch()`. `RspContext` carries the shared session state (UPDI fd, FSM context, symbol index, selected thread IDs, GDB client fd pointer, and quit flag pointer) and is stored in `RspHandlers.ctx`. Socket options applied by `rsp_listen()`: `SO_REUSEADDR` (allow rapid server restart) and `TCP_NODELAY` (disable Nagle algorithm to minimise GDB round-trip latency).

#### 5.2.2 GDB Network Interface

TCP server socket on the configured port (default `1234`). Accepts exactly one client connection at a time; a new connection is accepted after the previous client detaches.


### 5.3 Internal Structure
#### 5.3.1 Key Data Structures

**`RspHandlers`** — Function pointer table dispatched by `rsp_dispatch()`. Each handler receives the raw packet payload string, the client socket fd, and a pointer to the shared application context.

| Field | Packet(s) handled | Description |
| ----- | ----------------- | ----------- |
| `on_halt_reason`  | `?`                       | Stop-reason query; calls `signal_for_halt_status()` (reads `OCD_STATUS1` via `updi_ocd_read_halt_status()`) and replies `T02thread:<id>;` (SIGINT) when `OCD_STATUS1.EXTBRK` is set or `T05thread:<id>;` (SIGTRAP) otherwise. |
| `on_read_regs`    | `g`                       | Read all 35 GDB AVR registers (R0-R31, SREG, SP, PC; 78 hex chars) live from the OCD register file via `updi_ocd_read_gpr()` / `read_sreg()` / `read_sp()` / `read_pc()`. The live CPU is the sole GDB thread. |
| `on_write_regs`   | `G`, `P`                  | Write all registers (`G`) or a single register (`P n=vv`); both route per-slot to `updi_ocd_write_gpr/sreg/sp/pc` (no `updi_mem_write()`). |
| `on_read_mem`     | `m addr,len`              | Read memory; flips GDB unified-address bit 23 to produce the UPDI physical address before calling `updi_mem_read()`. |
| `on_write_mem`    | `M addr,len:data`, `X addr,len:bin` | Write memory: `updi_nvm_write_flash()` for program-space addresses (GDB bit 23 clear), `updi_mem_write()` for data-space addresses (GDB bit 23 set). |
| `on_continue`     | `c`, `vCont;c`            | Resume target via `updi_run()`, invalidate the FSM cache, multiplex `select(client_fd)` against `updi_ocd_poll_halted(1 ms)` to deliver Ctrl-C interrupts during the run, then halt and reply with `T02thread:<id>;` (Ctrl-C) or the signal from `signal_for_halt_status()`. Bounds UPDI poll failures with `UPDI_FAIL_MAX = 8`. |
| `on_step`         | `s`, `vCont;s`            | Single-step via `updi_step()`, rebuild the FSM thread list, reply with the stop-reason packet produced by `on_halt_reason`. |
| `on_insert_bp`    | `Z0`, `Z1`                | Install a hardware-breakpoint comparator in one of two `RspContext.hw_bp_addr[]` slots via `updi_ocd_set_hw_bp()`; both `Z0` and `Z1` route to the same two comparators. |
| `on_remove_bp`    | `z0`, `z1`                | Release a hardware-breakpoint comparator via `updi_ocd_clear_hw_bp()` and reset the shadow slot. |
| `on_thread_info`  | `qfThreadInfo`/`qsThreadInfo` | Reply `l` -- a single implicit GDB thread (the live CPU). |
| `on_thread_extra` | `qThreadExtraInfo`        | Return FSM name string as hex-encoded ASCII. |
| `on_set_thread_g` | `Hg<tid>`                 | Accept the set-thread packet; only the live-CPU thread (TID 1) is valid. |
| `on_set_thread_c` | `Hc<tid>`                 | Select virtual thread for subsequent `c`/`s` operations. |
| `on_monitor`      | `qRcmd`                   | Forward the raw ASCII-hex command body to `monitor_dispatch()`. |
| `on_detach`       | `D`, `k`                  | Detach (`D`: call `hw_bp_clear_all(ctx)` to release both OCD comparators, then `updi_run()` and close the client fd) or kill (`k`: set the global quit flag). |

**Breakpoint shadow:** The two AVR-Dx OCD hardware-breakpoint comparators (`BP0`, `BP1`) are tracked in the session context as `RspContext.hw_bp_addr[0..1]`, each storing the GDB byte address currently programmed or the sentinel `HW_BP_SLOT_EMPTY` (`0xFFFFFFFF`). Both `Z0` (software) and `Z1` (hardware) GDB packets route to the same two comparators — patching the AVR `BREAK` opcode into FLASH at runtime would require exiting OCD mode, entering NVMPROG (which resets the CPU and destroys live register/SREG/SP state), patching, and re-entering OCD, a sequence whose state-preservation cost outweighs the benefit. `on_insert_bp` programs silicon via `updi_ocd_set_hw_bp()`; `on_remove_bp` releases via `updi_ocd_clear_hw_bp()`; `on_detach` calls the `hw_bp_clear_all()` helper so silicon is left clean for the next session.

**Async Ctrl-C interrupt:** While `on_continue` is polling the target for a halt, it uses `select()` on the GDB client fd with a 5 ms timeout to detect the GDB Ctrl-C async-interrupt byte (`0x03`). On detection it calls `updi_halt()` and replies `T02thread:<id>;` (SIGINT). The poll loop also bounds consecutive `updi_ocd_poll_halted()` failures with `UPDI_FAIL_MAX = 8` and replies `E01` if the link degrades to that point, rather than spinning indefinitely.

**Packet buffer:** `RSP_PACKET_MAX` (2048 bytes) is the compile-time maximum for a single RSP payload, sized to accommodate a full AVR `g`-packet response (35 registers × 2 hex chars/byte = 70 bytes) plus worst-case `qXfer` memory-map XML overhead.


#### 5.3.2 Key Functions

*   **`int rsp_listen(uint16_t port)`** — Create, bind, and listen on the GDB TCP socket; return the listen fd or -1.
*   **`int rsp_accept(int listen_fd)`** — Block until a GDB client connects; return the client fd.
*   **`int rsp_recv_packet(int fd, char *buf, size_t cap)`**
    *   Purpose: Read one complete RSP packet from the client socket into buf, verify its checksum, and send ACK or NAK.
    *   Pre-condition: `fd` is a connected GDB client socket; `buf` points to a caller-allocated buffer of at least `cap` bytes; `cap` is at least `RSP_PACKET_MAX`.
    *   Post-condition: On success, `buf` holds the null-terminated payload string (between `$` and `#`); the checksum bytes have been consumed from the socket.
    *   Return Value: Number of payload bytes written to buf on success; 0 on client disconnect; -1 on checksum mismatch (NAK sent) or socket error.
    *   Logic:
        1.  Read bytes one at a time until `$` is received; discard any `+`/`-` ACK/NAK bytes that precede it.
        2.  Accumulate payload bytes into `buf`, computing a running XOR checksum, until `#` is received.
        3.  Read the two ASCII hex checksum bytes that follow `#` and parse them to a single byte value.
        4.  Compare computed checksum to received checksum. On match, write `+` to `fd` and return payload length. On mismatch, write `-` to `fd` and return -1.

*   **`int rsp_send_packet(int fd, const char *payload)`** — Frame payload as an RSP packet and transmit; return 0 or -1.
*   **`int rsp_dispatch(int fd, const char *packet, RspHandlers *h)`**
    *   Purpose: Identify the RSP packet type from the first character (and optionally subsequent characters) of packet and invoke the matching handler in h. Returns 0 on success or -1 if the selected handler reports a write error.
    *   Logic:
        1.  Match the leading characters of `packet` against the handler table using a switch on `packet[0]` with secondary string comparisons for multi-character commands (`qS`, `qA`, `qf`, `qs`, `qR`, `QS`, `vC`, `Hg`, `Hc`, `Z0`, `z0`).
        2.  Invoke the matching handler function pointer from `h`, passing `fd`, `packet`, and the shared application context pointer.
        3.  If no handler matches, call `rsp_send_packet(fd, "")` to send the mandatory empty response `$#00`.
    *   Notes: The following packets are handled inline (not via the `RspHandlers` table) because their responses are fixed strings that require no target interaction: `qSupported` (feature string), `qAttached` (returns `1`), `QStartNoAckMode` (calls `rsp_set_noack(true)` and returns `OK`), and `vCont?` (returns the supported vCont actions string `vCont;c;s`).

*   **`void rsp_set_noack(bool enabled)`** — Toggle no-ack mode after a successful QStartNoAckMode negotiation; suppresses both the sending and the expectation of +/- ack bytes.
*   **`bool rsp_get_noack(void)`** — Returns the current no-ack mode flag; primarily for tests and diagnostics.
*   **`void rsp_default_handlers(RspHandlers *h, RspContext *ctx)`** — Populate every slot in *h with the production handler implementations and bind the shared session state pointer ctx to h->ctx.

#### 5.3.3 Parsing Strategy / Algorithm

**RSP packet codec:**

*Reception* (`rsp_recv_packet`): reads bytes until `$`, then accumulates payload until `#`, then reads two hex checksum digits. Computes XOR of all payload bytes. Sends `+` on match, `-` on mismatch. After `QStartNoAckMode` is negotiated, the server stops sending `+`/`-` as the client will not retransmit.

*Transmission* (`rsp_send_packet`): computes XOR checksum over the payload string, formats the frame as `$<payload>#<XX>` and writes it in a single `write()` call to minimise TCP segment fragmentation.

**Capability negotiation:** On the `qSupported` packet the server replies with the feature string `"PacketSize=800;QStartNoAckMode+;multiprocess-;vContSupported+"`. The server enables no-ack mode immediately upon receiving `QStartNoAckMode+`.

**Thread context:** GDB selects a thread with `Hg<tid>` and `Hc<tid>`. The server stores the selected thread IDs in `RspHandlers.g_thread` and `RspHandlers.c_thread`. Register reads use `g_thread`; continue/step use `c_thread`.

**Static memory layout of `src/gdb_rsp.c`:**

| Variable | Type | Size | Purpose |
| -------- | ---- | ---- | ------- |
| `pkt_buf[RSP_PACKET_MAX]` | `char[2048]` | 2 KiB | Receive buffer for one RSP packet payload. |
| `rsp_buf[RSP_PACKET_MAX+8]` | `char[2056]` | 2 KiB | Transmit buffer: framed `$payload#XX`. |
| `g_thread`, `c_thread` | `int` | 8 B | Selected GDB thread IDs for register and continue operations. |

(The previous 96-byte software-breakpoint table has been removed; the two hardware-breakpoint shadow slots now live in `RspContext.hw_bp_addr[]` and are session-scoped.)

Total static BSS in `src/gdb_rsp.c`: approximately 4.2 KiB.

**TCP socket options applied by `rsp_listen()`:**

*   `SO_REUSEADDR` — allows rapid server restart without waiting for TIME_WAIT to expire.
*   `TCP_NODELAY` — disables Nagle's algorithm; each `write()` is sent immediately, which is critical for GDB round-trip latency (GDB sends many small packets and waits for each response before continuing).

**Test approach for `src/gdb_rsp.c`:** Unit-testable using a pair of connected loopback sockets (`socketpair()`) with all `updi_*` and `fsm_*` symbols replaced by `ld --wrap` stubs that record call sequences and return canned values. Test cases cover: packet reception with correct and incorrect checksums; ACK/NAK behaviour before and after `QStartNoAckMode`; `rsp_dispatch()` routing to each handler; empty-response for unknown packets; `G`/`P` writes routed per slot to `updi_ocd_write_gpr/sreg/sp/pc`; `g` reads served from the OCD register file for the active thread; `Z0`/`Z1` insert programming an OCD comparator via `updi_ocd_set_hw_bp()`; `z0`/`z1` releasing via `updi_ocd_clear_hw_bp()`; a third unique-address insert returning `E08`; Ctrl-C (`\x03`) on the client socket halting the target during `c` and the reply carrying signal `T02`; `D` (detach) calling `hw_bp_clear_all()` before `updi_run()` so silicon comparators are released; and the `UPDI_FAIL_MAX = 8` retry bound preventing `c` from spinning on a degraded UPDI link.

### 5.4 Dependencies

*   `src/updi.c` — execution control and memory access.
*   `src/fsm_mapper.c` — virtual thread enumeration and per-thread register state.
*   `src/monitor.c` — custom `monitor avros` commands.
*   POSIX socket API.

### 5.5 Error Handling and Logging

*   **Checksum mismatch** Send `-` (NAK) and await retransmit from the GDB client.
*   **Unrecognized packet type** Send the empty response `$#00` as required by the RSP specification.
*   **Client disconnect** Close the client fd, reopen the listener, and wait for the next GDB connection.
*   **Breakpoint comparators exhausted** Return GDB error packet `E08`; no comparator is programmed. The AVR-Dx provides only two HW comparators (`BP0`/`BP1`), so GDB receives `E08` on the third unique-address `Z0`/`Z1` insert.

## 6. Detailed Design for [src/elf_parser.c](../src/elf_parser.c)

### 6.1 Purpose and Responsibilities
[src/elf_parser.c](../src/elf_parser.c) parses the AVR ELF binary to extract the FLASH addresses and sizes of the avrOS system tables and to produce the `AvrOsSymbolIndex` consumed by the FSM mapper.

*   Open and validate the ELF file header and target architecture.
*   Locate the symbol table section and iterate over symbol entries.
*   Apply the Harvard architecture offset to convert ELF virtual addresses to physical FLASH word addresses.
*   Populate an `AvrOsSymbolIndex` with the FLASH address, SRAM status address, and entry count for each detected avrOS table.
*   Provide ELF program-header metadata to the RSP layer for memory-map query responses.

### 6.2 External Interfaces
#### 6.2.1 Public C API (src/elf_parser.h)

All functions exported from `src/elf_parser.c` and declared in `src/elf_parser.h`:

```c
/* Lifecycle */
int  elf_open (const char *path, ElfContext *ctx);
    /* Validates magic/arch, loads .symtab and .strtab into ctx. */
    /* Returns: 0 on success; -1 on I/O error, bad magic, or malloc failure. */
void elf_close(ElfContext *ctx);
    /* Frees ctx->symtab and ctx->strtab; closes ctx->fd. */

/* Symbol resolution */
int      elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx);
    /* Returns: 0 (partial or full success); -1 on internal ELF read error. */
uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma);
    /* Applies: physical_word_addr = (vma - ctx->flash_base) / 2 */
```

The `ElfContext` and `AvrOsSymbolIndex` struct definitions are also declared in `src/elf_parser.h`.  Callers must zero-initialise `AvrOsSymbolIndex` before passing it to `elf_find_avros_tables()`.


### 6.3 Internal Structure
#### 6.3.1 Key Data Structures

**`ElfContext`** — All ELF parsing state maintained between `elf_open()` and `elf_close()`.

| Field | Type | Description |
| ----- | ---- | ----------- |
| `fd`          | `int`         | Open file descriptor for the ELF binary (kept open for lazy section reads). |
| `ehdr`        | `Elf32_Ehdr`  | Cached ELF32 file header (magic, machine, entry point, section count). |
| `symtab`      | `Elf32_Sym *` | Heap-allocated copy of the `.symtab` section contents. |
| `sym_count`   | `size_t`      | Number of `Elf32_Sym` entries in `symtab`. |
| `strtab`      | `char *`      | Heap-allocated copy of the `.strtab` string table used for symbol names. |
| `strtab_size` | `size_t`      | Byte size of the `strtab` buffer. |
| `flash_base`  | `uint32_t`    | ELF VMA of the first `PT_LOAD` segment (FLASH); used as Harvard offset base. |
| `flash_size`  | `uint32_t`    | Byte size of the FLASH load segment. |
| `sram_base`   | `uint32_t`    | ELF VMA of the SRAM segment. |
| `sram_size`   | `uint32_t`    | Byte size of the SRAM segment. |

Harvard address conversion formula applied by `elf_flash_addr()`:
`physical_word_addr = (vma − flash_base) / 2`

Both `.symtab` and `.strtab` are loaded fully into heap memory at `elf_open()` time and freed by `elf_close()`, so no file seeks are required during symbol lookup.


#### 6.3.2 Key Functions

*   **`int elf_open(const char *path, ElfContext *ctx)`** — Open and parse the ELF file; populate ctx; return 0 or -1.
*   **`void elf_close(ElfContext *ctx)`** — Release all resources held by ctx.
*   **`int elf_find_avros_tables(ElfContext *ctx, AvrOsSymbolIndex *idx)`**
    *   Purpose: Search the loaded symbol table for the avrOS system table boundary symbols and populate idx with resolved FLASH and SRAM addresses.
    *   Pre-condition: `ctx` has been successfully initialised by `elf_open()`; `idx` points to a zero-initialised `AvrOsSymbolIndex`.
    *   Post-condition: All fields of `idx` that correspond to symbols found in the ELF are populated. Fields for missing symbols retain their zero-initialised values.
    *   Return Value: 0 if at least the FSM table symbols were found and resolved; 0 with `idx` partially populated if only some symbols were found; -1 only on an internal ELF read error.
    *   Logic:
        1.  Iterate over all `sym_count` entries in `ctx->symtab`; skip entries with `st_name == 0` or `st_shndx == SHN_UNDEF`.
        2.  Look up each symbol's name in `ctx->strtab` at offset `sym->st_name` and compare against the avrOS sentinel names (see algorithm section for the full symbol name table).
        3.  For `_start` symbols (FLASH-resident table boundaries), call `elf_flash_addr()` to convert the VMA to a physical word address and store in the corresponding `idx` field.
        4.  For `__stop_<NAME>` symbols, subtract the paired `__start_<NAME>` VMA and divide by the per-entry struct size to compute the entry count (e.g., `fsm_table_count`).
        5.  For the `currStateMachine` symbol (SRAM-resident variable), store its VMA directly without the Harvard offset.

*   **`uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma)`** — Convert an ELF virtual memory address to a physical FLASH word address.

#### 6.3.3 Parsing Strategy / Algorithm

`elf_open()` validates and loads the ELF as follows:

1.  Read the first 4 bytes; verify the ELF magic (`0x7F 'E' 'L' 'F'`). Verify `e_ident[EI_CLASS] == ELFCLASS32`.
2.  Verify `e_machine == EM_AVR` (0x0053); reject non-AVR ELF files with a warning.
3.  Scan `e_phnum` program headers at offset `e_phoff`; for each `PT_LOAD` segment, record the segment VMA (`p_vaddr`) and size (`p_filesz`) to populate `flash_base` / `flash_size` (first LOAD segment) and `sram_base` / `sram_size` (second LOAD segment).
4.  Scan `e_shnum` section headers at offset `e_shoff`; find the section with `sh_type == SHT_SYMTAB`. Read its `sh_size / sizeof(Elf32_Sym)` entries into a heap buffer.
5.  Load the associated string table section (index given by `sh_link` on the `.symtab` section header) into a second heap buffer.

`elf_find_avros_tables()` scans the loaded symbol array for the following names:

| Symbol name             | `AvrOsSymbolIndex` field | Address space     | Per-entry stride |
| ----------------------- | ------------------------ | ----------------- | ---------------- |
| `__start_FSM_TABLE`     | `fsm_table_addr`         | FLASH (word addr) | 9 bytes (`fsmStateMachineDescr_t`)        |
| `__stop_FSM_TABLE`      | (size computation)       | FLASH             | — |
| `__start_QUE_TABLE`     | `queue_table_addr`       | FLASH (word addr) | 10 bytes (`queDescriptor_t`)              |
| `__stop_QUE_TABLE`      | (size computation)       | FLASH             | — |
| `__start_EVNT_TABLE`    | `event_table_addr`       | FLASH (word addr) | 4 bytes (`evntDescriptor_t`)              |
| `__stop_EVNT_TABLE`     | (size computation)       | FLASH             | — |
| `currStateMachine`      | `current_fsm_addr`       | SRAM (byte addr)  | — |

The boundary symbols `__start_<NAME>` and `__stop_<NAME>` are emitted by the avrOS application's custom linker script (`avrOS.x`) and bracket the input section that holds the corresponding registration table. Entry counts (e.g., `fsm_table_count`) are derived by computing `(__stop − __start) / sizeof(per_entry_descriptor)`, where the descriptor sizes match the 1-byte-packed AVR layout shown above.

**Heap memory budget for `elf_open()`:**

| Buffer | Formula | Typical size (1000-symbol AVR ELF) |
| ------ | ------- | ---------------------------------- |
| `ctx->symtab` | `sym_count x sizeof(Elf32_Sym)` = `sym_count x 16` B | ~16 KiB |
| `ctx->strtab` | `.strtab` section `sh_size` | ~8 KiB |
| Total | | ~24 KiB per session |

Both buffers are freed by `elf_close()`. Peak heap usage occurs between `elf_open()` and the first `elf_close()`.

**Parse complexity:** `elf_open()` performs O(e_phnum + e_shnum) file seeks plus two sequential block reads. `elf_find_avros_tables()` performs a single O(sym_count) linear scan with 8 string comparisons per symbol entry.

**Test approach for `src/elf_parser.c`:** Unit-testable using pre-built AVR ELF fixtures. Test cases cover: magic validation rejection, `EM_AVR` check, correct `flash_base` / `sram_base` extraction, all 7 avrOS sentinel symbols found with correct address conversions, partial symbol set (graceful degradation), `malloc` failure via injection shim, and `elf_close()` resource-free correctness.

### 6.4 Dependencies

*   Standard C file I/O.
*   ELF header definitions from `<elf.h>` (Linux) or a bundled `elf.h` for macOS portability.

### 6.5 Error Handling and Logging

*   **Non-ELF or wrong architecture magic** Log a warning message and return -1.
*   **Missing avrOS symbols** Log an informational message and return 0 with an empty `AvrOsSymbolIndex`; the server continues in bare-metal stub mode.
*   **Memory allocation failure** Return -1; the caller falls back to bare-metal stub mode.

## 7. Detailed Design for [src/fsm_mapper.c](../src/fsm_mapper.c)

### 7.1 Purpose and Responsibilities
[src/fsm_mapper.c](../src/fsm_mapper.c) translates the runtime state of avrOS FSM registration tables — read from the target via UPDI — into an internal introspection snapshot (per-FSM name, active flag, and current-state function pointer).

*   Use the `AvrOsSymbolIndex` to read FSM state bytes from the target's SRAM over UPDI.
*   Assign a stable GDB thread ID to each registered FSM entry.
*   Identify and report which FSM is currently executing as the active GDB thread.
*   Record each FSM's name, active flag, and current-state function pointer in the introspection snapshot.
*   Cache the thread list and invalidate it whenever the CPU resumes.

### 7.2 External Interfaces
#### 7.2.1 Public C API (src/fsm_mapper.h)

All functions exported from `src/fsm_mapper.c` and declared in `src/fsm_mapper.h`:

```c
/* Thread-list management */
int  fsm_build_thread_list(FsmContext *ctx,
                           const AvrOsSymbolIndex *idx,
                           int updi_fd);
    /* Requires: CPU halted. Returns: thread count (>= 0); -1 on UPDI error. */
void fsm_invalidate(FsmContext *ctx);
    /* Marks ctx->valid = false; called immediately after updi_run()/updi_step(). */

/* Per-thread queries */
int  fsm_get_active_thread(const FsmContext *ctx);
    /* Returns: GDB thread ID of active FSM; 0 if not identified. */
```

The `FsmContext` struct is declared in `src/fsm_mapper.h` and must be zero-initialised by the caller before the first `fsm_build_thread_list()` call.


### 7.3 Internal Structure
#### 7.3.1 Key Data Structures

**`FsmThread`** — Descriptor for a single avrOS FSM virtual thread. One per registered FSM entry; stored in the static array `FsmContext.threads`.

| Field | Type | Description |
| ----- | ---- | ----------- |
| `gdb_id`    | `int`      | Stable GDB thread ID (1-based); assigned in the order FSM entries appear in the FLASH table and never re-assigned within a session. |
| `name`      | `char[32]` | Null-terminated human-readable FSM name, read from the FLASH-resident `name_ptr` field of the avrOS FSM table entry. |
| `state_fn`  | `uint32_t` | Current state function pointer as a FLASH word address; set to the value of the FSM's SRAM state variable at thread-list build time; used directly as the virtual PC. |
| `is_active` | `bool`     | True when this FSM is the currently executing task, as identified by comparing the FSM's `stateMachine` SRAM pointer against the avrOS `currStateMachine` scheduler variable. |

**`FsmContext`** contains a statically allocated array of `FSM_MAX_THREADS` (32) `FsmThread` entries, eliminating heap allocation in the thread build path.


#### 7.3.2 Key Functions

*   **`int fsm_build_thread_list(FsmContext *ctx, const AvrOsSymbolIndex *idx, int updi_fd)`**
    *   Purpose: Read the avrOS FSM registration table and SRAM state variables from the target via UPDI and populate the virtual thread cache.
    *   Pre-condition: Target CPU is halted; `idx->fsm_table_addr` and `idx->fsm_table_count` are valid; `updi_fd` is a valid UPDI serial fd.
    *   Post-condition: `ctx->threads[0..thread_count-1]` contain populated `FsmThread` descriptors; `ctx->active_id` identifies the running FSM; `ctx->valid` is set to true.
    *   Return Value: Number of virtual threads populated on success; -1 on UPDI read failure.
    *   Logic:
        1.  Read `idx->fsm_table_count` consecutive 9-byte `fsmStateMachineDescr_t` records from FLASH at `idx->fsm_table_addr` using `updi_mem_read()`. Each descriptor contains, in order: `name` (2-byte FLASH ptr), `stateMachine` (2-byte SRAM ptr), `handler` (2-byte fn ptr), `priority` (1 byte), `instance` (2-byte u16).
        2.  If a descriptor's `stateMachine` pointer is NULL (0x0000), treat the entry as an avrOS initializer slot and skip it without consuming a GDB thread ID.
        3.  For each non-NULL `stateMachine` pointer, read 2 bytes from `stateMachine + 9` (SRAM); this is the `currState` field of `fsmStateMachine_t` and yields the current state function pointer (FLASH word address), which is zero-extended into `thread->state_fn`.
        4.  Read the null-terminated FSM name from the FLASH address given by the descriptor's `name` pointer (up to 31 characters) and store in `thread->name`.
        5.  Read the 2-byte SRAM value at `idx->current_fsm_addr` (avrOS `currStateMachine`); compare it to each retained entry's `stateMachine` pointer to identify the active FSM and set `thread->is_active` and `ctx->active_id`. If no entry matches, `ctx->active_id` is 0.
        6.  Assign `thread->gdb_id = produced_index + 1` for each retained thread (GDB thread IDs are 1-based and never reuse a slot from a NULL-skipped initializer entry).

*   **`int fsm_get_active_thread(const FsmContext *ctx)`** — Return the GDB thread ID of the currently executing FSM.
*   **`void fsm_invalidate(FsmContext *ctx)`** — Clear the cached thread list; called on every CPU resume.

#### 7.3.3 Parsing Strategy / Algorithm

**Thread list build sequence:**

Each avrOS FSM is represented in FLASH by a 9-byte `fsmStateMachineDescr_t` entry placed into the `FSM_TABLE` linker section by the `FSM_STATE_MACHINE(...)` registration macro:

```c
typedef struct {
    char                  *name;          /* FLASH pointer to display name string */
    fsmStateMachine_t     *stateMachine;  /* SRAM pointer to the FSM instance (NULL = init slot) */
    fsmStateHandler_t      handler;       /* FLASH function pointer */
    uint8_t                priority;
    uint16_t               instance;
} fsmStateMachineDescr_t;  /* 9 bytes, no padding */
```

`fsm_build_thread_list()` reads `idx->fsm_table_count` consecutive entries starting at `idx->fsm_table_addr`. Entries with a NULL `stateMachine` pointer are skipped (these are avrOS initializer slots, not user FSMs). For each retained entry, the function dereferences `stateMachine + 9` in SRAM to obtain the current state function pointer (the `currState` field of `fsmStateMachine_t`) and reads the FLASH `name` pointer to obtain the FSM display name. All UPDI reads are performed while the CPU is halted.

**Cache invalidation:** `fsm_invalidate()` sets `ctx->valid = false`. The RSP layer calls `fsm_invalidate()` immediately after `updi_run()` or `updi_step()` so that the stale thread list is not served to GDB after execution resumes. The cache is rebuilt on the next `fsm_build_thread_list()` call, which the RSP layer triggers when GDB halts the target again.

**AVR GDB register layout (g-packet order):**

| GDB register index | AVR register | Size | Notes |
| ------------------ | ------------ | ---- | ----- |
| 0-31 | R0-R31 | 1 byte each | General-purpose; zeroed for non-active threads. |
| 32 | SREG | 1 byte | Status register; zeroed for non-active threads. |
| 33 | SPL | 1 byte | Stack pointer low byte; live value for active thread only. |
| 34 | SPH | 1 byte | Stack pointer high byte; live value for active thread only. |
| 35 | PC | 4 bytes LE | Set to `thread->state_fn` (FLASH word address) for all threads. |

Total g-packet payload: 39 bytes × 2 hex chars = 78 hex characters + NUL. (Registers 0–34 each contribute 1 byte; register 35 (PC) contributes 4 bytes.)

**UPDI read budget per `fsm_build_thread_list()` call (N retained threads):**

| Operation | UPDI transactions | Data volume |
| --------- | ----------------- | ----------- |
| Read `currStateMachine` (2 bytes) | 1 read | 2 bytes |
| Read FSM descriptor per entry (9 bytes each) | N reads | 9N bytes |
| Read `currState` per retained entry (2 bytes each) | N reads | 2N bytes |
| Read FLASH name string per retained entry (up to 32 chars) | N reads | <=32N bytes |
| **Total** | 3N+1 transactions | <=43N+2 bytes |

**Test approach for `src/fsm_mapper.c`:** Unit-testable by providing a pre-populated `AvrOsSymbolIndex` and a mock `updi_mem_read()` returning canned byte sequences. Test cases cover: GDB thread ID assignment (1-based), correct `state_fn` FLASH word-address derivation, active thread identification, `FSM_MAX_THREADS` cap with warning, cache-invalidation round-trip.

### 7.4 Dependencies

*   `src/updi.c` — `updi_mem_read()` for SRAM reads.
*   `src/elf_parser.c` — `AvrOsSymbolIndex` for table addresses and entry counts.

### 7.5 Error Handling and Logging

*   **UPDI read failure during thread build** Return -1; the RSP layer reports a GDB error packet to the client.
*   **Thread ID out of range** Return -1 and cause the RSP layer to send error response `E01`.
*   **FSM count exceeds FSM_MAX_THREADS** Cap the build at `FSM_MAX_THREADS` entries and log a warning; excess FSMs are silently omitted from the thread list.

## 8. Detailed Design for [src/monitor.c](../src/monitor.c)

### 8.1 Purpose and Responsibilities
[src/monitor.c](../src/monitor.c) implements the custom `monitor avros` sub-commands that provide non-intrusive, human-readable visibility into the state of avrOS system objects (named events and queues) via UPDI background reads.

*   Parse the `monitor avros <subcommand>` argument string.
*   Issue UPDI background reads to retrieve the relevant avrOS data structures without halting the CPU.
*   Format the decoded state as human-readable text and deliver it to the GDB client as a console-output packet.

### 8.2 External Interfaces
#### 8.2.1 Public C API (src/monitor.h)

Single function exported from `src/monitor.c` and declared in `src/monitor.h`:

```c
int monitor_dispatch(int rsp_fd, int updi_fd,
                     const AvrOsSymbolIndex *idx,
                     const char *cmd);
```

Parameters:
*   `rsp_fd` — connected GDB client socket fd; used to send O-packet responses via `rsp_send_packet()`.
*   `updi_fd` — UART serial fd; used for `updi_mem_read()` background reads.
*   `idx` — resolved avrOS symbol index from `elf_find_avros_tables()`; must not be NULL.
*   `cmd` — raw ASCII-hex-encoded payload string from a `qRcmd` RSP packet (not yet decoded).

Return values: 0 command recognised and executed; -1 UPDI read failure; -2 unrecognised sub-command.

#### 8.2.2 GDB Monitor Sub-commands

Two sub-commands are supported:

*   `monitor avros events` — list each named event registered in `EVNT_TABLE` with its current status flag value.
*   `monitor avros queues` — list each registered queue's capacity and per-element size from `QUE_TABLE`.


### 8.3 Internal Structure
#### 8.3.1 Key Functions

*   **`int monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx, const char *cmd)`**
    *   Purpose: Hex-decode the qRcmd payload, match the sub-command token, and invoke the appropriate handler.
    *   Pre-condition: `cmd` is the raw ASCII-hex-encoded command body extracted from a `qRcmd` RSP packet (not yet decoded).
    *   Return Value: 0 if the command was recognised and the handler returned without error; -1 on UPDI failure; -2 on unrecognised sub-command.
    *   Logic:
        1.  Hex-decode the ASCII hex pairs in `cmd` into a plain text command string.
        2.  Verify the string starts with the prefix `"avros "` (case-sensitive); if not, send a usage hint O-packet and return -2.
        3.  Extract the token following the prefix and compare it to `"events"` and `"queues"`.
        4.  Invoke the matching static handler or send an unknown-subcommand error O-packet.

*   **`static int cmd_events(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx)`**
    *   Purpose: Iterate the avrOS `EVNT_TABLE` and report each named event's current status flag to the GDB console.
    *   Logic:
        1.  Read `idx->event_count` consecutive `evntDescriptor_t` records (4 bytes each: `char *name`, `event_t *status`) from FLASH at `idx->event_table_addr` using `updi_mem_read()`.
        2.  For each descriptor, follow the FLASH `name` pointer to read the human-readable event name (NUL-terminated) and the SRAM `status` pointer to read the current 1-byte status value.
        3.  Append a line `"  <name>: <status>\n"` to an output buffer, then hex-encode and send as one or more RSP O-packets via `rsp_send_packet()`.

*   **`static int cmd_queues(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx)`**
    *   Purpose: Read each registered avrOS queue's capacity and per-element size and send a formatted table to the GDB console.
    *   Logic:
        1.  Read `idx->queue_count` consecutive `queDescriptor_t` records (10 bytes each: queue/buffer/event pointers + 2-byte capacity + 2-byte sizeOfElement) from FLASH at `idx->queue_table_addr` using `updi_mem_read()`.
        2.  For each descriptor, extract the `capacity` and `sizeOfElement` fields and format a one-line entry.
        3.  Hex-encode and send the formatted table as RSP O-packets.


#### 8.3.2 Parsing Strategy / Algorithm

**O-packet encoding:** All console output is delivered to the GDB client via RSP O-packets. The RSP specification requires the human-readable text to be hex-encoded: each ASCII character is converted to two hex digits. For example, the character `'A'` (0x41) is encoded as the two-character string `"41"`. `monitor_dispatch()` builds the human-readable text into a temporary 512-byte buffer, then hex-encodes it before calling `rsp_send_packet()` with the `O` prefix.

**Non-intrusive reads:** All three sub-commands use `updi_mem_read()` exclusively; none call `updi_halt()`. The UPDI background read mechanism allows SRAM to be sampled while the CPU is running, at the cost of a possible read-during-write data race for multi-byte fields. This is acceptable for the observational, non-critical nature of the monitor commands.

**avrOS runtime data structure layouts read by monitor commands:**

*Queue descriptor* (read by `cmd_queues()`, located in FLASH `QUE_TABLE`):
```c
typedef struct {
    void    *queue;          /* SRAM ring buffer pointer */
    void    *buffer;         /* SRAM element-storage pointer */
    event_t *event;          /* SRAM event flag pointer */
    uint16_t capacity;       /* maximum items the queue can hold */
    uint16_t sizeOfElement;  /* bytes per element */
} queDescriptor_t;  /* 10 bytes, FLASH */
```

*Event descriptor* (read by `cmd_events()`, located in FLASH `EVNT_TABLE`):
```c
typedef struct {
    char    *name;    /* FLASH pointer to event name string */
    event_t *status;  /* SRAM pointer to 1-byte status flag  */
} evntDescriptor_t;  /* 4 bytes, FLASH */
```

**O-packet size constraint:** A single RSP O-packet payload is limited to `RSP_PACKET_MAX` bytes (2048). For typical `cmd_events()` and `cmd_queues()` output the 512-byte text buffer is hex-encoded in chunks that each fit within one O-packet before calling `rsp_send_packet()`.

**Test approach for `src/monitor.c`:** Unit-testable by providing a mock `updi_mem_read()` returning canned bytes for the `EVNT_TABLE`/`QUE_TABLE` regions and capturing O-packet strings via a pipe replacing `rsp_fd`. Test cases cover: hex-decode of `qRcmd` payload, prefix rejection, both sub-command dispatch paths, correct event-name + status decoding, correct queue-field extraction, and O-packet encoding of all output characters.

### 8.4 Dependencies

*   `src/updi.c` — `updi_mem_read()` for non-intrusive SRAM reads.
*   `src/elf_parser.c` — `AvrOsSymbolIndex` for data structure addresses.

### 8.5 Error Handling and Logging

*   **Unrecognized sub-command** Return a human-readable error message to the GDB console output packet.
*   **UPDI read failure** Report partial data alongside an error message to the GDB console; do not abort the server.
## 9. Data Dictionary

*   **`AppConfig`** (defined in [src/main.c](../src/main.c)) — Application-wide configuration populated by parse_args().

    | Field | Type | Description |
    | ----- | ---- | ----------- |
| `serial_device` | `const char *` | Path to the UART serial device. |
| `elf_path` | `const char *` | Path to the AVR ELF binary. |
| `gdb_port` | `uint16_t` | TCP port for the GDB listener (default 1234). |
| `baud_rate` | `int` | UART baud rate (default 115200). |
| `load_flash` | `bool` | When true, flash the ELF to the target before attaching. |
*   **`AvrOsSymbolIndex`** (defined in [src/elf_parser.c](../src/elf_parser.c)) — Index of avrOS system table addresses resolved from the ELF symbol table.

    | Field | Type | Description |
    | ----- | ---- | ----------- |
| `fsm_table_addr` | `uint32_t` | FLASH word address of the avrOS FSM registration table (`__start_FSM_TABLE`). |
| `fsm_table_count` | `uint8_t` | Number of `fsmStateMachineDescr_t` entries in `FSM_TABLE` (size / 9). |
| `queue_table_addr` | `uint32_t` | FLASH word address of the queue registration table (`__start_QUE_TABLE`). |
| `queue_count` | `uint8_t` | Number of `queDescriptor_t` entries in `QUE_TABLE` (size / 10). |
| `event_table_addr` | `uint32_t` | FLASH word address of the named-event registration table (`__start_EVNT_TABLE`). |
| `event_count` | `uint8_t` | Number of `evntDescriptor_t` entries in `EVNT_TABLE` (size / 4). |
| `current_fsm_addr` | `uint32_t` | SRAM byte address of the avrOS scheduler `currStateMachine` pointer variable; used by fsm_mapper to identify the active thread. |
*   **`FsmThread`** (defined in [src/fsm_mapper.c](../src/fsm_mapper.c)) — Descriptor for a single avrOS FSM virtual thread, stored in the FsmContext thread array.

    | Field | Type | Description |
    | ----- | ---- | ----------- |
| `gdb_id` | `int` | Stable GDB thread ID (1-based); assigned at thread-list build time and never re-used within a session. |
| `name` | `char[32]` | Null-terminated human-readable FSM name read from the FLASH-resident table entry name pointer. |
| `state_fn` | `uint32_t` | Current state function pointer as a FLASH word address; used directly as the virtual PC in the synthesized register frame. |
| `is_active` | `bool` | True when this FSM is the currently executing task as identified from the avrOS current_fsm scheduler variable. |
*   **`ElfContext`** (defined in [src/elf_parser.c](../src/elf_parser.c)) — All ELF parsing state maintained between elf_open() and elf_close().

    | Field | Type | Description |
    | ----- | ---- | ----------- |
| `fd` | `int` | Open file descriptor for the ELF binary. |
| `ehdr` | `Elf32_Ehdr` | Cached ELF32 file header (magic, machine type, entry point, section/program header counts). |
| `symtab` | `Elf32_Sym *` | Heap-allocated copy of the .symtab section. |
| `sym_count` | `size_t` | Number of Elf32_Sym entries in symtab. |
| `strtab` | `char *` | Heap-allocated copy of the .strtab string table used for symbol name lookup. |
| `strtab_size` | `size_t` | Byte size of the strtab buffer. |
| `flash_base` | `uint32_t` | ELF VMA of the first PT_LOAD segment; used as the Harvard FLASH base for address conversion. |
| `flash_size` | `uint32_t` | Byte size of the FLASH load segment. |
| `sram_base` | `uint32_t` | ELF VMA of the SRAM segment. |
| `sram_size` | `uint32_t` | Byte size of the SRAM segment. |
*   **`FsmContext`** (defined in [src/fsm_mapper.c](../src/fsm_mapper.c)) — Runtime cache of the avrOS virtual thread list.

    | Field | Type | Description |
    | ----- | ---- | ----------- |
| `threads` | `FsmThread[FSM_MAX_THREADS]` | Statically allocated array of virtual thread descriptors (one per FSM entry). |
| `thread_count` | `int` | Number of valid entries populated in the threads array. |
| `active_id` | `int` | GDB thread ID of the currently executing FSM; 0 if none identified. |
| `valid` | `bool` | True when the cache reflects current target state; cleared to false by fsm_invalidate() on every CPU resume. |
*   **Compile-time constants** (in [src/updi.c](../src/updi.c)):

    | Name | Value | Purpose |
    | ---- | ----- | ------- |
| `UPDI_SYNCH` | 0x55 | UPDI synchronisation byte transmitted at the start of each command. |
| `UPDI_ACK` | 0x40 | Acknowledgement byte returned by the target after each received data byte. |
| `UPDI_MAX_BLOCK` | 256 | Maximum byte count for a single REPEAT+LD/ST burst transaction. |
| `UPDI_BREAK_BAUD` | 300 | Temporary baud rate used to assert the BREAK condition during link initialisation. |
| `UPDI_ERR_WP` | -2 | Distinct error code returned by updi_nvm_write_flash() when FLASH write protection is active. |
*   **Compile-time constants** (in [src/gdb_rsp.c](../src/gdb_rsp.c)):

    | Name | Value | Purpose |
    | ---- | ----- | ------- |
    | `RSP_PACKET_MAX`      | `2048` | Maximum RSP payload size in bytes; sized for a full AVR register dump plus `qXfer` XML overhead. |
    | `RSP_MAX_BREAKPOINTS` | `2`    | Number of AVR-Dx OCD hardware-breakpoint comparators tracked by the RSP session context (`BP0` and `BP1`). |

*   **Compile-time constants** (in [src/fsm_mapper.c](../src/fsm_mapper.c)):

    | Name | Value | Purpose |
    | ---- | ----- | ------- |
    | `FSM_MAX_THREADS` | `32` | Maximum number of virtual threads supported in a single session; caps the static `FsmContext` thread array. |

---

### Build Toolchain and Host Environment

**Host compiler requirements:**

| Requirement | Minimum | Notes |
| ----------- | ------- | ----- |
| C standard | C99 | `--std=c99`; no GNU extensions required. |
| GCC | 4.8 | Or Clang ≥ 3.4. |
| POSIX API | POSIX.1-2008 | `-D_POSIX_C_SOURCE=200809L` |
| ELF headers | `<elf.h>` | Provided by `glibc-headers` on Linux; bundled `elf.h` on macOS. |
| libc | any POSIX libc | No additional shared libraries required. |

**Recommended compile flags:**

```
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic
         -Wstrict-prototypes -Wmissing-prototypes -Wshadow
         -O2 -g
```

Debug builds add `-fsanitize=address,undefined` for runtime error detection.

**Supported host platforms:**

| Platform | Status | Notes |
| -------- | ------ | ----- |
| Linux (x86-64, ARM64) | Primary target | Tested on Ubuntu 22.04 and Raspberry Pi OS. |
| macOS (Intel, Apple Silicon) | Secondary target | Requires bundled `elf.h`; uses `/dev/cu.usbserial-*` device paths. |
| Windows (native) | Out of scope | See `doc/PVD.md §7.2`; WSL2 may work but is not supported. |

**Build targets (Makefile):**

| Target | Description |
| ------ | ----------- |
| `all` | Build the `avrOSdb` binary (default). |
| `clean` | Remove build artefacts. |
| `test` | Build and run the unit test suite. |
| `install` | Copy the binary to `$(PREFIX)/bin` (default `/usr/local/bin`). |

---

### Target MCU Memory Constraints

The following constraints govern the AVR DA/DB-family target assumed by the implementation:

| Parameter | Value | Notes |
| --------- | ----- | ----- |
| FLASH word size | 2 bytes | AVR is a 16-bit instruction machine; word addresses = byte addresses / 2. |
| FLASH page size | 512 bytes | Page boundary alignment required for `updi_nvm_write_flash()`. |
| Maximum FLASH | 128 KiB | AVR128DA/DB; upper bound for `ElfContext.flash_size`. |
| Maximum SRAM | 16 KiB | AVR128DA/DB; upper bound for `ElfContext.sram_size`. |
| FLASH base VMA | `0x0000` | ELF virtual address of the first instruction word. |
| SRAM base VMA | `0x0200` | Lowest SRAM address on AVR DA/DB (I/O registers below this). |
| UPDI base address | `0x1000` | NVM controller register base used for flash programming. |
| AVR BREAK opcode | `0x9598` | Two-byte instruction used for software breakpoints. |

---

### Static Memory Footprint Summary

Approximate static (BSS + data) memory consumed by each module in the host process:

| Module | Major static buffers | Approximate size |
| ------ | -------------------- | ---------------- |
| `src/main.c` | `AppConfig` struct (stack-allocated in `main`) | ~32 bytes |
| `src/updi.c` | `termios` structs, internal staging buffer | ~256 bytes |
| `src/gdb_rsp.c` | Receive + transmit packet buffers, breakpoint table | ~4.2 KiB |
| `src/elf_parser.c` | Heap: `.symtab` + `.strtab` (per session) | ~24 KiB (heap) |
| `src/fsm_mapper.c` | `FsmContext` static thread array (32 x `FsmThread`) | ~2.1 KiB |
| `src/monitor.c` | Output text staging buffer | ~512 bytes |
| **Total static** | | **~7 KiB static + ~24 KiB heap per session** |

---

### Module Integration Sequence

Modules should be integrated and verified in the following order to isolate failures:

| Step | Modules | Integration milestone | Verification method |
| ---- | ------- | --------------------- | ------------------- |
| 1 | `src/updi.c` alone | UPDI link initialisation and basic memory read/write | Loopback pseudo-terminal test; compare read-back bytes against written values. |
| 2 | `src/elf_parser.c` alone | ELF open, symbol scan, `AvrOsSymbolIndex` population | Offline test against a known AVR ELF binary; compare resolved addresses against `avr-nm` output. |
| 3 | `src/updi.c` + `src/elf_parser.c` | Halt, read FLASH/SRAM at resolved symbol addresses, resume | Live hardware: read FSM table bytes, verify against expected avrOS registration. |
| 4 | `src/fsm_mapper.c` | Thread-list build and register frame synthesis | Offline test with mock UPDI reads; live hardware to verify correct FSM names and state pointers. |
| 5 | `src/gdb_rsp.c` alone | TCP accept, packet codec, dispatch to stub handlers | Loopback socket: drive with `avr-gdb`'s `target extended-remote localhost:1234`, issue `info threads`. |
| 6 | `src/monitor.c` | `monitor avros events/queues` output | Live hardware: run target, issue monitor commands from GDB, verify output format and values. |
| 7 | All modules | Full session: attach, set breakpoint, halt, inspect threads, resume | End-to-end GDB script driven test against live AVR hardware. |

---

### Error Propagation Model

Errors are propagated upward through the module stack without retrying:

| Layer | Error type | Propagation rule |
| ----- | ---------- | ---------------- |
| `src/updi.c` | UART timeout, framing error, UPDI NAK | Return -1 to caller. No retry at this layer. |
| `src/elf_parser.c` | I/O error, bad ELF, malloc failure | Return -1; server enters bare-metal stub mode (no FSM threads). |
| `src/fsm_mapper.c` | UPDI read failure | Return -1; RSP layer sends `E02` error packet to GDB. |
| `src/gdb_rsp.c` | Socket error, checksum mismatch | NAK on mismatch; close socket on disconnect; send `E` packet on UPDI errors. |
| `src/monitor.c` | UPDI read failure | Send partial output with error message as O-packet; return -1 but do not abort server. |
| `src/main.c` | Fatal startup error | `exit(1)` with message to stderr. Mid-session UPDI loss: close client, await next connection. |
## 10. Traceability

The following table maps the high-level requirements in
[doc/HLRs.md](HLRs.md) and the low-level requirements in
[doc/LLRs.md](LLRs.md) to the design elements above. (Requirement IDs
should be reconciled against the latest revisions of those documents.)

| Requirement Theme | Design Section(s) |
| ----------------- | ----------------- |
| CLI and Initialization | §3 (src/main.c) |
| UPDI Physical Layer | §4 (src/updi.c) |
| GDB RSP Server | §5 (src/gdb_rsp.c) |
| ELF Parsing and Symbol Resolution | §6 (src/elf_parser.c) |
| FSM State Snapshot | §7 (src/fsm_mapper.c) |
| System Introspection | §8 (src/monitor.c) |
| Flash Programming | §4 (src/updi.c) |
| Console Bridge | §4 (src/updi.c) |
---
