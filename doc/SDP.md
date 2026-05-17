# Software Development Plan: avrOS-debug (aod)

**Version:** 0.1
**Date:** 2026-05-16
**Author(s):** John Anderson

> **How to use this template.** Each section below contains the
> *intent* of the section followed by **prompts** to elicit the
> content. Replace every `<placeholder>` and the prompt block with
> your own prose. Delete prompts once a section is filled in.
>
> The SDP is a living document — update the Status table and Phased
> Delivery section as work progresses. Link phase names in the Status
> table to their detailed descriptions in §8.

**Status:** Specification complete (SDD, HLRs, LLRs, STP, Traceability — 0 lint errors). Implementation not yet started.

## Status

| Phase | Description | Status |
| ----- | ----------- | ------ |
| [0](#phase-0--project-scaffolding) | Directories, Unity test framework, Makefile, ELF portability shim | 🔲 Not started |
| [1](#phase-1--elf-parser) | `src/elf_parser.h/.c` + 13 unit tests | 🔲 Not started |
| [2](#phase-2--updi-physical-layer) | `src/updi.h/.c` + 23 unit tests | 🔲 Not started |
| [3](#phase-3--fsm-mapper) | `src/fsm_mapper.h/.c` + 11 unit tests | 🔲 Not started |
| [4](#phase-4--monitor--rsp) | `src/monitor.h/.c` + `src/gdb_rsp.h/.c` + 41 unit tests | 🔲 Not started |
| [5](#phase-5--application-entry-point--integration) | `src/main.c` + 18 tests (14 unit + 4 integration) | 🔲 Not started |

## 0. Required Tools for Development

### Required

| Tool | Version | Purpose |
| ---- | ------- | ------- |
| `gcc` or `clang` | ≥ gcc 4.8 / clang 3.4 | C99 host compiler for the `avr-updi-gdb` binary and all unit tests |
| `make` | ≥ 3.81 | Build orchestration (`all`, `clean`, `test`, `install` targets) |
| `avr-gcc` | ≥ 12.0 | AVR cross-compiler; generates the `.elf` fixture binaries consumed by `tests/test_elf.c` |
| `avr-binutils` | matching `avr-gcc` | Provides `avr-nm` for verifying fixture symbol addresses against expected values |

### Optional

| Tool | Purpose |
| ---- | ------- |
| `glibc-headers` (Linux) | Provides `<elf.h>`; a bundled `src/elf.h` shim is used on macOS instead |
| AddressSanitizer / UBSan | Runtime error detection; enabled via `make ASAN=1` (`-fsanitize=address,undefined`) |
| `avr-gdb` | End-to-end smoke testing against a live or simulated AVR target |

## 1. Motivation

## 1. Motivation

`avr-updi-gdb` fills the gap between the AVR UPDI debug interface and standard GDB-based IDEs while adding first-class avrOS FSM task visibility. Without this stub, developers must choose between low-level UPDI tools with no source-level debugging, or generic GDB stubs that have no awareness of the avrOS cooperative task model. The result is that avrOS application developers cannot set breakpoints, inspect task state, or understand which FSM is running — the core debugging workflows that every RTOS user expects.

This implementation follows the complete specification stack authored in this repository (PVD → SDD → HLRs → LLRs → STP), which reached lint-clean status (0 errors, 0 warnings) before any source code was written. See [doc/PVD.md](PVD.md) for the full product vision.

## 2. Goals

## 2. Goals

1. Deliver a working `avr-updi-gdb` binary built from 6 C99 source modules (`main`, `updi`, `gdb_rsp`, `elf_parser`, `fsm_mapper`, `monitor`).
2. All 54 Low-Level Requirements fully implemented and verified by 106 passing tests across 7 test files.
3. Binary compiles without warnings under `-std=c99 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L`.
4. No heap allocation on the hot path; only `elf_open()` allocates (freed by `elf_close()` at session end).
5. Portable: builds and all tests pass on Linux (x86-64, ARM64) and macOS (Intel, Apple Silicon).
6. Runtime dependencies: only libc — verified by `ldd avr-updi-gdb` showing no libraries beyond libc.
7. `python3 tools/lint_project.py` continues to report 0 errors, 0 warnings throughout the implementation.

## 3. Non-Goals

## 3. Non-Goals

*   **Windows native build.** WSL2 may work but is untested and unsupported (see [doc/PVD.md](PVD.md) §7.2).
*   **IDE adapter layers.** Cortex-Debug and Zed DAP integration are out of scope for the initial implementation.
*   **Multi-client GDB support.** The server accepts a single GDB connection at a time; concurrent clients are not addressed.
*   **UPDI retry/recovery.** Physical-layer errors return −1 to the caller; no automatic retry or reconnect logic is implemented at this layer.
*   **AVR XMEGA or ATtiny targets.** Only the AVR DA/DB family (128 KiB FLASH, 16 KiB SRAM) is in scope.

## 4. Design — see the SDD

## 4. Design — see the SDD

The detailed software architecture is documented in the [Software Design Document](SDD.md). Key components:

*   `src/updi.c` — UPDI physical layer; UART serial management, BREAK/SYNCH initialisation, memory read/write bursts (REPEAT+LD/ST), halt/run/step execution control, NVM flash programming, and UPDI console bridge.
*   `src/elf_parser.c` — ELF32 parser; locates the 8 avrOS sentinel symbols in the `.symtab` to produce the `AvrOsSymbolIndex` of FLASH/SRAM table addresses.
*   `src/fsm_mapper.c` — avrOS FSM-to-GDB virtual thread translator; maps each registered FSM to a GDB thread ID and synthesises a per-thread g-packet register frame with PC set to the FSM's current state function pointer.
*   `src/monitor.c` — custom `monitor avros events|queues|mempool` sub-command handler; reads avrOS runtime data via non-intrusive UPDI background reads and delivers formatted output as GDB O-packets.
*   `src/gdb_rsp.c` — GDB Remote Serial Protocol server; TCP socket lifecycle, RSP packet codec, and a 15-entry command dispatch table covering all standard debug operations.
*   `src/main.c` — application entry point; CLI argument parsing (`AppConfig`), single-threaded `select()`-based event loop, SIGINT/SIGTERM handler (`g_quit`), and ordered teardown.

## 5. Development Process

## 5. Development Process

### 5.1 Branching Strategy

Trunk-based development. Feature work is done on short-lived branches (one branch per phase or sub-task) and merged to `main` via pull request once all acceptance criteria for that phase are met.

### 5.2 Code Review

All changes are reviewed before merge. The reviewer checklist:

1. `make` completes without warnings under `-Wall -Wextra -Wpedantic`.
2. `make test` runs all tests for the affected phase(s) and all pass.
3. `python3 tools/lint_project.py` reports 0 errors, 0 warnings.
4. No `<placeholder>` text remains in any touched file.

### 5.3 Continuous Integration

Every commit runs: `make test` (builds and executes all unit test binaries) and `python3 tools/lint_project.py`. Both must exit 0. The integration test phase additionally runs `make` to produce the final `avr-updi-gdb` binary and verifies `ldd` output shows only libc.

### 5.4 Release Process

Source-only releases. Tag `vX.Y.Z` on `main` once all 106 tests pass and the binary builds clean. No prebuilt binaries are distributed; consumers build from source.

## 6. Testing Strategy

## 6. Testing Strategy

| Level | Scope | Tools | Coverage Target |
| ----- | ----- | ----- | --------------- |
| Unit | Per-module isolation — each source module tested independently against mock/stub dependencies | [Unity](https://github.com/ThrowTheSwitch/Unity) (vendored as `tests/unity/unity.c`) + `gcc`/`ld` `--wrap` linker mocking for POSIX symbols and inter-module calls | 100% LLR coverage (54 LLRs, 102 unit tests) |
| Integration | Full `avr-updi-gdb` binary launched via `fork()`/`execv()` with PTY UART and loopback TCP socket | Custom C harness (`tests/test_integration.c`) | All 4 integration tests tracing to HLR-005, HLR-020, HLR-033, HLR-034 |

Tests are traced to Low-Level Requirements in [doc/Project.xml](Project.xml)
and reported in the [Software Test Plan](STP.md) and
[Traceability Matrix](Traceability.md).

**Mocking strategy:** The `gcc`/`ld` `--wrap` linker trick replaces individual symbols at link time for each test binary. For example, `tests/test_fsm` is linked with `-Wl,--wrap,updi_mem_read`; the test defines `__wrap_updi_mem_read()` to drain a pre-filled canned-byte queue, keeping `src/fsm_mapper.c` under test while UPDI hardware is absent.

**ELF test fixtures:** Three minimal AVR ELF binaries (`tests/fixtures/avros_full.elf`, `avros_partial.elf`, `not_avr.elf`) are generated at test-build time by `avr-gcc`. These are not committed as binaries; the Makefile compiles them from `tests/fixtures/*.c` sources.

## 7. Dependencies & Prerequisites

## 7. Dependencies & Prerequisites

| Dependency | Required By | Notes |
| ---------- | ----------- | ----- |
| `avr-gcc` ≥ 12.0 | Phase 1 test fixtures | Compiles `tests/fixtures/*.c` to `.elf` binaries consumed by `tests/test_elf.c`; not needed for the main binary |
| Unity test framework v2.6.x | All unit test phases (1–5) | Vendored into `tests/unity/` as three files (`unity.c`, `unity.h`, `unity_internals.h`); no system install required |
| `openpty()` / POSIX PTY | Phase 2 (UPDI tests) | Part of glibc on Linux (link with `-lutil`); in `<util.h>` on macOS (no extra link flag needed) |
| `<elf.h>` ELF type definitions | Phase 1 source + tests | System `<elf.h>` on Linux (from `glibc-headers` / `binutils-dev`); bundled `src/elf.h` portability shim used on macOS |
| Phase 1 + 2 headers | Phase 3 | `src/fsm_mapper.c` includes both `src/updi.h` and `src/elf_parser.h`; both must be finalised before Phase 3 begins |
| Phase 3 complete | Phase 4 (RSP) | `src/gdb_rsp.c` dispatches to `fsm_build_thread_list()` and `fsm_get_registers()`; these must exist before linking the RSP test binary |
| All prior phases complete | Phase 5 | `src/main.c` integrates all six modules; its test binary links against every module |

## 8. Phased Delivery

## 8. Phased Delivery

### Phase 0 — Project Scaffolding

1. Create directories: `src/`, `tests/`, `tests/fixtures/`, `tests/unity/`.
2. Vendor Unity test framework: download `unity.c`, `unity.h`, `unity_internals.h` from ThrowTheSwitch/Unity into `tests/unity/`.
3. Create `Makefile` with targets `all`, `clean`, `test`, `install`. The `all` target compiles all `src/*.c` with `-std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Wshadow -O2 -g`. The `test` target builds ELF fixtures via `avr-gcc`, then builds and runs all test binaries; each test binary carries its own `--wrap` symbol list. Platform detection via `$(shell uname -s)` adds `-lutil` for `openpty()` on Linux.
4. Create `src/elf.h` — minimal bundled ELF type definitions (`Elf32_Ehdr`, `Elf32_Shdr`, `Elf32_Phdr`, `Elf32_Sym`, `ELFMAG`, `ELFCLASS32`, `EM_AVR 0x0053`, `SHT_SYMTAB`, `SHN_UNDEF`, `PT_LOAD`). Used by `src/elf_parser.c` on macOS; Linux uses the system `<elf.h>` via `#ifdef __linux__` guard.

**Acceptance:** `make all` succeeds (even with empty `.c` stub files); `make clean` removes all build artefacts; `make test` compiles and runs (stubs may fail; the infrastructure must work).

---

### Phase 1 — ELF Parser

1. `src/elf_parser.h` — define `ElfContext` and `AvrOsSymbolIndex` structs; declare `elf_open()`, `elf_close()`, `elf_find_avros_tables()`, `elf_flash_addr()`.
2. `src/elf_parser.c` — validate ELF magic + `ELFCLASS32` + `EM_AVR`; scan `PT_LOAD` segments for `flash_base`/`sram_base`; `malloc` `.symtab` + `.strtab`; single O(sym\_count) scan for the 8 avrOS sentinel names; `elf_flash_addr(vma) = (vma − flash_base) / 2`; graceful return on partial symbol match; `elf_close()` frees all heap and sets pointers to NULL.
3. `tests/fixtures/avros_full.c` — defines all 8 avrOS sentinel linker symbols via `__attribute__((section(...)))` or a linker script; compiled to `tests/fixtures/avros_full.elf` by the Makefile.
4. `tests/fixtures/avros_partial.c` — defines only 4 of the 8 symbols; tests graceful degradation.
5. `tests/fixtures/not_avr.c` — compiled for a non-AVR target (e.g. `--target=elf32-i386`) to produce an ELF with `e_machine != EM_AVR`.
6. `tests/test_elf.c` — 13 Unity tests covering: magic rejection, `EM_AVR` check, correct `flash_base`/`sram_base`, all 8 symbol names found with correct address conversions, partial symbol set, `malloc` failure injection, `elf_close()` resource-free correctness. Linked against `src/elf_parser.c tests/unity/unity.c` with no `--wrap` flags.

**Acceptance:** `make test` runs `tests/test_elf` and reports 13/13 tests passing.

---

### Phase 2 — UPDI Physical Layer

1. `src/updi.h` — declare all 9 public functions; define constants `UPDI_SYNCH 0x55`, `UPDI_ACK 0x40`, `UPDI_MAX_BLOCK 256`, `UPDI_BREAK_BAUD 300`, `UPDI_ERR_WP -2`, and ASI register offsets.
2. `src/updi.c` — implement:
   - `updi_open()`: raw `termios` configuration + BREAK via 300-baud 0x00 write + restore baud + SYNCH 0x55 sequence.
   - `updi_write_bytes()` (private helper): writes N bytes then reads and discards N **echo bytes** (half-duplex hardware loopback — every transmitted byte appears on RX).
   - `updi_mem_read()`: REPEAT+LD burst; block-split at `UPDI_MAX_BLOCK`; `select()` with 100 ms timeout before each `read()`.
   - `updi_mem_write()`: REPEAT+ST burst; validate ACK after each byte.
   - `updi_halt()`, `updi_run()`, `updi_step()`: ASI register commands with poll loops.
   - `updi_nvm_write_flash()`: KEY sequence + NVMPROG poll + per-512-byte-page REPEAT+ST + ERWP command + BUSY poll; return `UPDI_ERR_WP` on write-protect bit.
   - `updi_console_poll()`: non-intrusive SRAM read of avrOS UART buffer.
3. `tests/test_updi.c` — 23 Unity tests. `openpty()` provides `(master_fd, slave_fd)`; the slave path is passed to `updi_open()`. The PTY harness reads commands from `master_fd` and injects simulated AVR responses — **echo bytes must be written back first** before each response, matching real half-duplex hardware. `__wrap_select()` is controlled by a global `g_select_force_timeout` flag to simulate UPDI timeout paths. Linked with `-Wl,--wrap,select` (and `-lutil` on Linux).

**Acceptance:** `make test` runs `tests/test_updi` and reports 23/23 tests passing.

---

### Phase 3 — FSM Mapper

1. `src/fsm_mapper.h` — define `FsmThread` struct (`gdb_id`, `name[32]`, `state_fn`, `is_active`) and `FsmContext` struct (`threads[FSM_MAX_THREADS]`, `thread_count`, `active_id`, `valid`); constant `FSM_MAX_THREADS 32`; declare the 4 public functions.
2. `src/fsm_mapper.c` — implement:
   - `fsm_build_thread_list()`: single block `updi_mem_read()` for the FSM table; per-entry dereference of SRAM state variable → FLASH word address; read name string; read `current_fsm_addr` to identify active thread; assign 1-based `gdb_id`; cap at `FSM_MAX_THREADS` with log warning.
   - `fsm_get_registers()`: 79-byte buffer (78 hex chars + NUL); PC at g-packet hex positions 70–77 (register index 35, 4-byte little-endian from `thread->state_fn`); live SREG/SPL/SPH at positions 64–69 for active thread only via `updi_mem_read()`; all other registers zero.
   - `fsm_get_active_thread()`: return `ctx->active_id`.
   - `fsm_invalidate()`: set `ctx->valid = false`.
3. `tests/test_fsm.c` — 11 Unity tests. `__wrap_updi_mem_read()` drains a pre-filled `g_updi_queue[]` byte array; tests verify: GDB thread ID assignment (1-based), FLASH word-address conversion from SRAM state pointer, active thread identification, cap at 32 entries with warning, PC encoding at exact hex positions 70–77, cache invalidation round-trip. Linked with `-Wl,--wrap,updi_mem_read`.

**Acceptance:** `make test` runs `tests/test_fsm` and reports 11/11 tests passing; a test explicitly asserts that `fsm_get_registers()` places the PC at buffer bytes 70–77.

---

### Phase 4 — Monitor + RSP

Both modules have a circular header dependency (`monitor.c` calls `rsp_send_packet()`; `gdb_rsp.c` dispatches to `monitor_dispatch()`). Resolve it by declaring both headers before implementing either `.c` file.

1. `src/monitor.h` — declare `monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex*, const char*)`. Include `src/elf_parser.h` for `AvrOsSymbolIndex`.
2. `src/gdb_rsp.h` — define `RspHandlers` struct (15 function pointers + `void *ctx`); declare `rsp_listen()`, `rsp_accept()`, `rsp_close()`, `rsp_recv_packet()`, `rsp_send_packet()`, `rsp_dispatch()`; constants `RSP_PACKET_MAX 2048`, `RSP_MAX_BREAKPOINTS 16`.
3. `src/monitor.c` — hex-decode the `qRcmd` payload; verify `"avros "` prefix; dispatch to `cmd_events()`, `cmd_queues()`, `cmd_mempool()` static helpers. O-packet encoding: each ASCII character → 2 hex digits; build into a 512-byte staging buffer then pass to `rsp_send_packet(rsp_fd, ...)`. Non-intrusive reads only — no `updi_halt()` calls.
4. `src/gdb_rsp.c` — static buffers `pkt_buf[2048]`, `rsp_buf[2048+8]`, `bp_table[16]`; `rsp_recv_packet()` with XOR checksum validation; `rsp_dispatch()` switching on `packet[0]` then string-comparing multi-character commands; all 15 handler entries; `SO_REUSEADDR` + `TCP_NODELAY` socket options; breakpoint table reset on GDB detach.
5. `tests/test_monitor.c` — 13 Unity tests. `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)` captures O-packet bytes; `__wrap_updi_mem_read()` injects canned SRAM data. Linked with `-Wl,--wrap,updi_mem_read`.
6. `tests/test_rsp.c` — 28 Unity tests. `socketpair()` drives both RSP send and receive ends; all UPDI, FSM, and monitor functions wrapped. Linked with all relevant `--wrap` flags.

**Acceptance:** `make test` runs `tests/test_monitor` (13/13) and `tests/test_rsp` (28/28) passing.

---

### Phase 5 — Application Entry Point + Integration

1. `src/main.c` — implement `AppConfig` struct; `parse_args()` with defaults (`gdb_port=1234`, `baud_rate=115200`) and `fprintf(stderr, ...); exit(1)` on error; `event_loop()` with `select()` on up to 3 fds (listen socket, UPDI fd, GDB client fd); SIGINT/SIGTERM handler sets `volatile sig_atomic_t g_quit = 1` only; teardown order: `rsp_close(gdb_fd)` → `rsp_close(listen_fd)` → `elf_close()` → `updi_close()`.
2. `tests/test_main.c` — 14 Unity tests. `fork()` + `waitpid()` pattern for `exit()` path tests; pipes capture stderr output; signal test calls the handler function directly and checks `g_quit == 1`. Linked with `--wrap` flags for all 5 module init/close functions plus `--wrap,select`.
3. `tests/test_integration.c` — 4 integration tests. Builds the real `avr-updi-gdb` binary as a Makefile prerequisite; launches it via `fork()`/`execv()` with a PTY as the UART device and a local TCP port; a helper thread simulates the AVR side (responds to UPDI BREAK+SYNCH, handles `updi_mem_read` sequences); a raw TCP socket connects as the GDB client and sends RSP packets; tests measure startup latency (HLR-005), verify RSP packet purity (HLR-020), check build portability (HLR-033), and verify no non-libc dependencies (HLR-034).

**Acceptance:** `make test` runs all 7 test binaries; all 106 tests pass (14 + 23 + 11 + 13 + 28 + 14 + 4). `make` builds the final `avr-updi-gdb` binary without warnings. `ldd avr-updi-gdb` shows only libc. `python3 tools/lint_project.py` reports 0 errors, 0 warnings.

## 9. Risks & Open Questions

## 9. Risks & Open Questions

*   **Half-duplex echo cancellation in UPDI tests.** Every byte transmitted over the UPDI UART is echoed back on the RX line by the hardware. PTY pairs do not auto-echo, so the PTY test harness must explicitly write back the echo bytes before injecting each simulated AVR response. If this is omitted, UPDI functions will block waiting to drain echoes that never arrive, causing PTY tests to time out even though the production logic is correct.

*   **macOS `<elf.h>` portability.** Linux glibc provides `<elf.h>`; macOS does not. The bundled `src/elf.h` shim must be guarded with `#ifdef __linux__ #include <elf.h> #else #include "elf.h" #endif` in `src/elf_parser.c`. Risk: if the `#ifdef` guard is accidentally omitted on a macOS build the compiler will fail with a missing-header error that may be non-obvious.

*   **avr-gcc availability for ELF fixtures.** The `tests/test_elf.c` test suite loads real `.elf` binaries generated by `avr-gcc`. If `avr-gcc` is not installed, the ELF fixture `make` rule will fail and block the entire test build. Mitigation: document the requirement prominently in §0, and consider adding a `make check-tools` target that validates availability before attempting a build.

*   **g-packet PC register encoding.** The GDB AVR register layout places the program counter at register index 35, encoded as a 4-byte little-endian value at hex positions 70–77 of the `g`-packet (78 hex chars total). An off-by-one in the buffer offset produces a silently malformed frame — `avr-gdb` will show a wrong PC without reporting an error. Mitigation: `tests/test_fsm.c` includes a dedicated assertion that verifies the exact byte positions independently of any PC value.

## 10. Estimated Effort

## 10. Estimated Effort

T-shirt sizes relative to Phase 0.

| Phase | Description | Effort |
| ----- | ----------- | ------ |
| 0 | Project Scaffolding | S — directory tree, Makefile, vendoring; no application logic |
| 1 | ELF Parser | M — well-understood binary format; fixture generation adds one-time avr-gcc complexity |
| 2 | UPDI Physical Layer | L — half-duplex echo cancellation, multi-opcode protocol, PTY harness with echo simulation |
| 3 | FSM Mapper | M — clean mock boundary via `--wrap`; most complexity is in fixture byte sequence design |
| 4 | Monitor + RSP | XL — 15 RSP handler table entries, 28 test cases, O-packet hex encoding, circular header dependency |
| 5 | Application Entry Point + Integration | L — `main.c` itself is thin; integration test harness (fork/PTY/TCP) is the dominant effort |

## 11. Out-of-Scope Follow-ups

## 11. Out-of-Scope Follow-ups

*   **macOS CI (GitHub Actions).** A workflow that runs `make test` on `macos-latest` to catch portability regressions; depends on `brew install avr-gcc` being available on the hosted runner.
*   **ASAN/UBSan CI job.** A second CI job that repeats `make test` with `-fsanitize=address,undefined` to catch memory errors and undefined behaviour; already supported by `make ASAN=1` but not wired into CI.
*   **Live hardware integration test.** An end-to-end script that connects a real AVR DA/DB target via a USB-serial adapter, attaches `avr-gdb`, and verifies `info threads` output and `monitor avros events` decoding.
*   **Console bridge live testing.** `updi_console_poll()` reads the avrOS UART ring buffer; verifying this against a live target running actual UART output is deferred to post-implementation.
*   **Flash-load smoke test.** Exercising the full `--load` code path (ELF → NVM write → verify) on hardware is deferred; the NVM write logic is covered by unit tests but not by a write-then-read-back live test.
