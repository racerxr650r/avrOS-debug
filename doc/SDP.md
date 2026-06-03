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

**Status:** Phases 0–7 complete. Phase 8 (non-FLASH NVM programming), Phase 9 (CI-grade loader & link diagnostics), Phase 10 (GDB protocol completion / avarice feature parity), and Phase 11 (RSP capability honesty & multiprocess+ correctness) planned.

## Status

| Phase | Description | Status |
| ----- | ----------- | ------ |
| [0](#phase-0--project-scaffolding) | Directories, Unity test framework, Makefile, ELF portability shim | ✅ Complete (`92857a5`) |
| [1](#phase-1--elf-parser) | `src/elf_parser.h/.c` + 14 unit tests | ✅ Complete (`6ef89d2`) |
| [2](#phase-2--updi-physical-layer) | `src/updi.h/.c` + 23 unit tests | 🔲 Not started |
| [3](#phase-3--fsm-mapper) | `src/fsm_mapper.h/.c` + 11 unit tests | 🔲 Not started |
| [4](#phase-4--monitor--rsp) | `src/monitor.h/.c` + `src/gdb_rsp.h/.c` + 42 unit tests | 🔲 Not started |
| [5](#phase-5--application-entry-point--integration) | `src/main.c` + 18 tests (14 unit + 4 integration) | 🔲 Not started |
| [6](#phase-6--installation-targets--documentation) | `make install/uninstall/check-tools/bundle` + user manual + man page + 8 install tests | 🔲 Not started |
| [7](#phase-7--device-signature-diagnostic-mode) | `--device` flag + `updi_read_device_info()` + family lookup + 6 tests | 🔲 Not started |
| [8](#phase-8--non-flash-nvm-programming) | EEPROM / FUSE / USERROW / LOCKBIT programming from ELF segments | 🔲 Not started |
| [10](#phase-10--gdb-protocol-completion--avarice-feature-parity) | `vFlash*` + true SW breakpoints + watchpoints + monitor verbs + extended-remote | 🔲 Not started |
| [11](#phase-11--rsp-capability-honesty--multiprocess-correctness) | Proper `multiprocess+` thread-ID parsing, `qThreadExtraInfo` FSM labels, `swbreak+`/`hwbreak+` stop tags, richer `qSupported`, `vKill` listener-survival, exit/disconnect logging, `vCont;r` range-step | 🔲 Not started |

## 0. Required Tools for Development

### Required

| Tool | Version | Purpose |
| ---- | ------- | ------- |
| `gcc` or `clang` | ≥ gcc 4.8 / clang 3.4 | C99 host compiler for the `avrOSdb` binary and all unit tests |
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

`avrOSdb` fills the gap between the AVR UPDI debug interface and standard GDB-based IDEs while adding first-class avrOS FSM task visibility. Without this stub, developers must choose between low-level UPDI tools with no source-level debugging, or generic GDB stubs that have no awareness of the avrOS cooperative task model. The result is that avrOS application developers cannot set breakpoints, inspect task state, or understand which FSM is running — the core debugging workflows that every RTOS user expects.

This implementation follows the complete specification stack authored in this repository (PVD → SDD → HLRs → LLRs → STP), which reached lint-clean status (0 errors, 0 warnings) before any source code was written. See [doc/PVD.md](PVD.md) for the full product vision.

## 2. Goals

1. Deliver a working `avrOSdb` binary built from 6 C99 source modules (`main`, `updi`, `gdb_rsp`, `elf_parser`, `fsm_mapper`, `monitor`).
2. All 54 Low-Level Requirements fully implemented and verified by 106 passing tests across 7 test files.
3. Binary compiles without warnings under `-std=c99 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L`.
4. No heap allocation on the hot path; only `elf_open()` allocates (freed by `elf_close()` at session end).
5. Portable: builds and all tests pass on Linux (x86-64, ARM64) and macOS (Intel, Apple Silicon).
6. Runtime dependencies: only libc — verified by `ldd avrOSdb` showing no libraries beyond libc.
7. `python3 tools/lint_project.py` continues to report 0 errors, 0 warnings throughout the implementation.

## 3. Non-Goals

*   **Windows native build.** WSL2 may work but is untested and unsupported (see [doc/PVD.md](PVD.md) §7.2).
*   **IDE adapter layers.** Cortex-Debug and Zed DAP integration are out of scope for the initial implementation.
*   **Multi-client GDB support.** The server accepts a single GDB connection at a time; concurrent clients are not addressed.
*   **UPDI retry/recovery.** Physical-layer errors return −1 to the caller; no automatic retry or reconnect logic is implemented at this layer.
*   **AVR XMEGA or ATtiny targets.** Only the AVR DA/DB family (128 KiB FLASH, 16 KiB SRAM) is in scope.

## 4. Design — see the SDD

The detailed software architecture is documented in the [Software Design Document](SDD.md). Key components:

*   `src/updi.c` — UPDI physical layer; UART serial management, BREAK/SYNCH initialisation, memory read/write bursts (REPEAT+LD/ST), halt/run/step execution control, NVM flash programming, and UPDI console bridge.
*   `src/elf_parser.c` — ELF32 parser; locates the 8 avrOS sentinel symbols in the `.symtab` to produce the `AvrOsSymbolIndex` of FLASH/SRAM table addresses.
*   `src/fsm_mapper.c` — avrOS FSM-to-GDB virtual thread translator; maps each registered FSM to a GDB thread ID and synthesises a per-thread g-packet register frame with PC set to the FSM's current state function pointer.
*   `src/monitor.c` — custom `monitor avros events|queues|mempool` sub-command handler; reads avrOS runtime data via non-intrusive UPDI background reads and delivers formatted output as GDB O-packets.
*   `src/gdb_rsp.c` — GDB Remote Serial Protocol server; TCP socket lifecycle, RSP packet codec, and a 15-entry command dispatch table covering all standard debug operations.
*   `src/main.c` — application entry point; CLI argument parsing (`AppConfig`), single-threaded `select()`-based event loop, SIGINT/SIGTERM handler (`g_quit`), and ordered teardown.

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

Every commit runs: `make test` (builds and executes all unit test binaries) and `python3 tools/lint_project.py`. Both must exit 0. The integration test phase additionally runs `make` to produce the final `avrOSdb` binary and verifies `ldd` output shows only libc.

### 5.4 Release Process

Source-only releases. Tag `vX.Y.Z` on `main` once all 106 tests pass and the binary builds clean. No prebuilt binaries are distributed; consumers build from source.

## 6. Testing Strategy

| Level | Scope | Tools | Coverage Target |
| ----- | ----- | ----- | --------------- |
| Unit | Per-module isolation — each source module tested independently against mock/stub dependencies | [Unity](https://github.com/ThrowTheSwitch/Unity) (vendored as `tests/unity/unity.c`) + `gcc`/`ld` `--wrap` linker mocking for POSIX symbols and inter-module calls | 100% LLR coverage (54 LLRs, 102 unit tests) |
| Integration | Full `avrOSdb` binary launched via `fork()`/`execv()` with PTY UART and loopback TCP socket | Custom C harness (`tests/test_integration.c`) | All 4 integration tests tracing to HLR-005, HLR-020, HLR-033, HLR-034 |

Tests are traced to Low-Level Requirements in [doc/Project.xml](Project.xml)
and reported in the [Software Test Plan](STP.md) and
[Traceability Matrix](Traceability.md).

**Mocking strategy:** The `gcc`/`ld` `--wrap` linker trick replaces individual symbols at link time for each test binary. For example, `tests/test_fsm` is linked with `-Wl,--wrap,updi_mem_read`; the test defines `__wrap_updi_mem_read()` to drain a pre-filled canned-byte queue, keeping `src/fsm_mapper.c` under test while UPDI hardware is absent.

**ELF test fixtures:** Three minimal AVR ELF binaries (`tests/fixtures/avros_full.elf`, `avros_partial.elf`, `not_avr.elf`) are generated at test-build time by `avr-gcc`. These are not committed as binaries; the Makefile compiles them from `tests/fixtures/*.c` sources.

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

### Phase 0 — Project Scaffolding

> **Status: ✅ Complete** — commit `92857a5` on branch `1-phase-0-project-scaffolding` (2026-05-16).
> `make all`: 0 errors, 0 warnings. `make test`: 7/7 binaries run, 0 failures. `lint_project`: 0 errors, 0 warnings.

1. Create directories: `src/`, `tests/`, `tests/fixtures/`, `tests/unity/`.
2. Vendor Unity test framework: download `unity.c`, `unity.h`, `unity_internals.h` from ThrowTheSwitch/Unity into `tests/unity/`.
3. Create `Makefile` with targets `all`, `clean`, `test`, `install`. The `all` target compiles all `src/*.c` with `-std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Wshadow -O2 -g`. The `test` target builds ELF fixtures via `avr-gcc`, then builds and runs all test binaries; each test binary carries its own `--wrap` symbol list. Platform detection via `$(shell uname -s)` adds `-lutil` for `openpty()` on Linux.
4. Create `src/elf.h` — minimal bundled ELF type definitions (`Elf32_Ehdr`, `Elf32_Shdr`, `Elf32_Phdr`, `Elf32_Sym`, `ELFMAG`, `ELFCLASS32`, `EM_AVR 0x0053`, `SHT_SYMTAB`, `SHN_UNDEF`, `PT_LOAD`). Used by `src/elf_parser.c` on macOS; Linux uses the system `<elf.h>` via `#ifdef __linux__` guard.

**Acceptance:** `make all` succeeds (even with empty `.c` stub files); `make clean` removes all build artefacts; `make test` compiles and runs (stubs may fail; the infrastructure must work).

**Per-test linker `--wrap` symbol lists** — each test binary must link exactly these symbols:

| Test binary | Source files linked (besides `tests/unity/unity.c`) | `--wrap` symbols | Extra link flags |
| ----------- | --------------------------------------------------- | ---------------- | ---------------- |
| `test_elf` | `src/elf_parser.c` | `malloc` | — |
| `test_updi` | `src/updi.c` | `select` | `-lutil` (Linux only; not needed on macOS) |
| `test_fsm` | `src/fsm_mapper.c` | `updi_mem_read` | — |
| `test_monitor` | `src/monitor.c`, `src/gdb_rsp.c`, `src/elf_parser.c` | `updi_mem_read` | — |
| `test_rsp` | `src/gdb_rsp.c`, `src/fsm_mapper.c`, `src/monitor.c` | `updi_mem_read`, `updi_halt`, `updi_run`, `updi_step`, `updi_nvm_write_flash`, `updi_console_poll`, `fsm_build_thread_list`, `fsm_get_registers`, `fsm_get_active_thread`, `fsm_invalidate`, `monitor_dispatch` | — |
| `test_main` | `src/main.c` | `updi_open`, `updi_close`, `updi_console_poll`, `rsp_listen`, `rsp_accept`, `rsp_close`, `rsp_recv_packet`, `rsp_dispatch`, `elf_open`, `elf_find_avros_tables`, `elf_close`, `fsm_build_thread_list`, `select` | — |
| `test_integration` | *(launches compiled `avrOSdb` binary via `execv()`)* | *(none)* | — |

**`src/elf.h` portability shim — complete required content** (guarded by `#ifndef AOD_ELF_H`):

*Typedefs:* `Elf32_Half` (`uint16_t`), `Elf32_Word` (`uint32_t`), `Elf32_Off` (`uint32_t`), `Elf32_Addr` (`uint32_t`).

*Identification macros:* `ELFMAG "\177ELF"`, `SELFMAG 4`, `EI_CLASS 4`, `ELFCLASS32 1`, `EM_AVR 0x0053`, `ET_EXEC 2`.

*Program header:* `PT_LOAD 1`.

*Section header:* `SHT_SYMTAB 2`, `SHT_STRTAB 3`, `SHN_UNDEF 0`.

*Symbol macros:* `ELF32_ST_BIND(i) ((i)>>4)`, `ELF32_ST_TYPE(i) ((i)&0xf)`, `STT_OBJECT 1`, `STT_FUNC 2`, `STB_GLOBAL 1`.

*Structs (packed order matters — match the ELF spec byte layout exactly):*
- `Elf32_Ehdr` — `e_ident[16]`, `e_type`, `e_machine`, `e_version`, `e_entry`, `e_phoff`, `e_shoff`, `e_flags`, `e_ehsize`, `e_phentsize`, `e_phnum`, `e_shentsize`, `e_shnum`, `e_shstrndx` (total 52 bytes).
- `Elf32_Phdr` — `p_type`, `p_offset`, `p_vaddr`, `p_paddr`, `p_filesz`, `p_memsz`, `p_flags`, `p_align` (total 32 bytes).
- `Elf32_Shdr` — `sh_name`, `sh_type`, `sh_flags`, `sh_addr`, `sh_offset`, `sh_size`, `sh_link`, `sh_info`, `sh_addralign`, `sh_entsize` (total 40 bytes).
- `Elf32_Sym` — `st_name`, `st_value`, `st_size`, `st_info`, `st_other`, `st_shndx` (total 16 bytes).

*Platform guard in `src/elf_parser.c`:* `#ifdef __linux__\n#include <elf.h>\n#else\n#include "elf.h"\n#endif`

---

### Phase 1 — ELF Parser

> **Status: ✅ Complete** — commit `6ef89d2` on branch `2-phase-1-elf-parser` (2026-05-16).
> `make all`: 0 errors, 0 warnings. `make test`: test_elf reports 14/14 tests passing. `lint_project`: 0 errors, 0 warnings. `make ASAN=1 test`: no AddressSanitizer errors or memory leaks.

> **Implementation note:** During Phase 1 the per-test linker `--wrap` symbol list for `test_elf` was updated from *(none)* to `malloc` to support the malloc-failure injection test (LLR-ELF-02). The `TEST_WRAP_test_elf` variable in the Makefile now carries `malloc`; this is reflected in the Phase 0 table above.

1. `src/elf_parser.h` — define `ElfContext` and `AvrOsSymbolIndex` structs; declare `elf_open()`, `elf_close()`, `elf_find_avros_tables()`, `elf_flash_addr()`.
2. `src/elf_parser.c` — validate ELF magic + `ELFCLASS32` + `EM_AVR`; scan `PT_LOAD` segments for `flash_base`/`sram_base`; `malloc` `.symtab` + `.strtab`; single O(sym\_count) scan for the 8 avrOS sentinel names; `elf_flash_addr(vma) = (vma − flash_base) / 2`; graceful return on partial symbol match; `elf_close()` frees all heap and sets pointers to NULL.
3. `tests/fixtures/avros_full.c` — defines all 8 avrOS sentinel linker symbols via `__attribute__((section(...)))` or a linker script; compiled to `tests/fixtures/avros_full.elf` by the Makefile.
4. `tests/fixtures/avros_partial.c` — defines only 4 of the 8 symbols; tests graceful degradation.
5. `tests/fixtures/not_avr.c` — compiled for a non-AVR target (e.g. `--target=elf32-i386`) to produce an ELF with `e_machine != EM_AVR`.
6. `tests/test_elf.c` — 13 Unity tests covering: magic rejection, `EM_AVR` check, correct `flash_base`/`sram_base`, all 8 symbol names found with correct address conversions, partial symbol set, `malloc` failure injection, `elf_close()` resource-free correctness. Linked against `src/elf_parser.c tests/unity/unity.c` with no `--wrap` flags.

**Acceptance:** `make test` runs `tests/test_elf` and reports 14/14 tests passing.

**`ElfContext` struct (declared in `src/elf_parser.h`, zero-init before `elf_open()`):**

| Field | Type | Set by |
| ----- | ---- | ------ |
| `fd` | `int` | `elf_open()` |
| `ehdr` | `Elf32_Ehdr` | `elf_open()` — full header cached |
| `symtab` | `Elf32_Sym *` | `elf_open()` — heap alloc |
| `sym_count` | `size_t` | `elf_open()` |
| `strtab` | `char *` | `elf_open()` — heap alloc |
| `strtab_size` | `size_t` | `elf_open()` |
| `flash_base` | `uint32_t` | `elf_open()` — VMA of first `PT_LOAD` segment |
| `flash_size` | `uint32_t` | `elf_open()` |
| `sram_base` | `uint32_t` | `elf_open()` — VMA of second `PT_LOAD` segment |
| `sram_size` | `uint32_t` | `elf_open()` |

**`AvrOsSymbolIndex` struct (declared in `src/elf_parser.h`, zero-init before `elf_find_avros_tables()`):**

| Field | Type | Populated by symbol |
| ----- | ---- | ------------------- |
| `fsm_table_addr` | `uint32_t` | `__avros_fsm_table_start` — FLASH word address |
| `fsm_table_count` | `uint8_t` | `(__avros_fsm_table_end_vma - __avros_fsm_table_start_vma) / 4` |
| `queue_table_addr` | `uint32_t` | `__avros_queue_table_start` — FLASH word address |
| `queue_count` | `uint8_t` | `(__avros_queue_table_end_vma - __avros_queue_table_start_vma) / 4` |
| `event_mask_addr` | `uint32_t` | `__avros_event_mask` — SRAM byte address (no Harvard offset) |
| `mempool_table_addr` | `uint32_t` | `__avros_mempool_table_start` — FLASH word address |
| `mempool_count` | `uint8_t` | `(__avros_mempool_table_end_vma - __avros_mempool_table_start_vma) / 4` |
| `current_fsm_addr` | `uint32_t` | `__avros_current_fsm` — SRAM byte address (no Harvard offset) |

**Exact sentinel symbol strings scanned in `elf_find_avros_tables()`** — these are the verbatim C string literals to `strcmp()` against `&ctx->strtab[sym->st_name]`:

```
"__avros_fsm_table_start"      "__avros_fsm_table_end"
"__avros_queue_table_start"    "__avros_queue_table_end"
"__avros_event_mask"
"__avros_mempool_table_start"  "__avros_mempool_table_end"
"__avros_current_fsm"
```

Entry count formula: the `_end` VMA minus `_start` VMA divided by `4` (size of `avros_fsm_entry_t` on AVR: two 2-byte pointers). Skip symbols with `sym->st_shndx == SHN_UNDEF` or `sym->st_name == 0` to avoid false matches.

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

**`updi_open()` `termios` initialisation sequence (all steps required; order matters):**

1. `open(device, O_RDWR | O_NOCTTY | O_NONBLOCK)` to get the fd, then `fcntl(fd, F_SETFL, 0)` to clear `O_NONBLOCK` and restore blocking mode.
2. `tcgetattr(fd, &tty)` to read current terminal settings.
3. `cfmakeraw(&tty)` — disables all line-processing, echo, and canonical mode.
4. 8N2 framing: `tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CSTOPB`; `tty.c_cflag &= ~PARENB`.
5. Enable receiver: `tty.c_cflag |= CLOCAL | CREAD`.
6. 100 ms inter-byte timeout: `tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1` (tenths of seconds — any idle gap ≥ 100 ms triggers a framing error in callers).
7. Baud: `cfsetispeed(&tty, Bxxx); cfsetospeed(&tty, Bxxx)` using the `Bxxx` constant for the requested rate.
8. `tcsetattr(fd, TCSANOW, &tty)`.

**BREAK condition generation** (step 3 of the UPDI init sequence in `updi_open()`):

1. Lower to `B300`: `cfsetispeed/cfsetospeed(&tty, B300)` + `tcsetattr(TCSANOW)`.
2. `write(fd, "\x00", 1)` — one `0x00` byte at 300 baud occupies the line for ~33 ms (well above the 24.6 µs UPDI minimum BREAK).
3. `tcdrain(fd)` — block until the byte is fully transmitted before raising baud again.
4. Restore operating baud: `cfsetispeed/cfsetospeed(&tty, Bxxx)` + `tcsetattr(TCSANOW)`.
5. Transmit `UPDI_SYNCH` (`0x55`) and read + discard the one echo byte via `updi_write_bytes()`.

**Private helper (must NOT appear in `src/updi.h`):**

```c
static int updi_write_bytes(int fd, const uint8_t *buf, size_t n);
```

Writes `n` bytes then reads and discards exactly `n` echo bytes from the half-duplex RX line (every transmitted byte is looped back on RX by the hardware). If fewer than `n` echo bytes arrive within the 100 ms `VTIME` timeout, return -1. Every UPDI command encoder calls this helper instead of `write()` directly.

**UPDI command opcode encoding** (frame = `SYNCH 0x55` then one or more command bytes):

| Mnemonic | Byte value | Purpose in this module |
| -------- | ---------- | ---------------------- |
| `LDCS rd, cs` | `0x80 \| cs` | Read ASI control/status register `cs` |
| `STCS cs, rr` | `0xC0 \| cs` | Write value `rr` to ASI register `cs` |
| `LD rd, ptr++` | `0x24` | Burst read with pointer auto-increment |
| `ST ptr++, rr` | `0x64` | Burst write with pointer post-increment |
| `REPEAT n` | `0xA0`, `n-1` | Set burst count; next LD/ST repeats `n` times |
| `KEY` | `0xE0` | Transmit 8-byte unlock key (e.g. `"NVMProg "`) |
| `STS addr, rr` | `0x44` | Store single byte to 16-bit address (NVM CTRLA) |

**ASI register map** (accessed via `LDCS`/`STCS`; used by `updi_halt`, `updi_run`, `updi_step`, `updi_nvm_write_flash`):

| Name | Offset | Key bits |
| ---- | ------ | -------- |
| `ASI_CTRLA` | `0x02` | bit 2 = IBD (inter-byte delay) — set at UPDI init |
| `ASI_RESET_REQ` | `0x08` | write `0x59` = request reset; write `0x00` = release |
| `ASI_SYS_STATUS` | `0x0B` | bit 3 = STOPPED (CPU halted); bit 4 = NVMPROG |
| `ASI_SYS_CTRL` | `0x0C` | bit 0 = RSTSYS (reset request); write `0x01` to halt |

**NVM controller registers** (accessed via `updi_mem_write`/`updi_mem_read` at these absolute addresses):

| Name | Address | Purpose |
| ---- | ------- | ------- |
| `NVMCTRL_CTRLA` | `0x1000` | Write command: `0x03` = ERWP (Erase + Write Page) |
| `NVMCTRL_STATUS` | `0x1002` | bit 0 = BUSY; bit 2 = WRERROR (write-protect active) |

**Timing budget:**

| Operation | Timeout | Mechanism |
| --------- | ------- | --------- |
| Per-byte read | 100 ms | `termios VTIME=1` |
| `updi_halt()` poll interval | 1 ms | `nanosleep({0, 1000000})` |
| `updi_halt()` total | 50 ms | 50 polls × 1 ms; return -1 on expiry |
| NVM NVMPROG wait | 100 ms | Poll `ASI_SYS_STATUS` bit 4 |
| NVM page BUSY poll | 20 ms per page | Poll `NVMCTRL_STATUS` bit 0 |

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

**avrOS FSM FLASH table entry layout** (`avros_fsm_entry_t` — 4 bytes on AVR, two 2-byte pointers):

```c
typedef struct {
    uint16_t *state;    /* SRAM addr of the current-state fn-ptr variable (2 bytes) */
    const char *name;   /* FLASH addr of null-terminated name string (2 bytes) */
} avros_fsm_entry_t;
```

`fsm_build_thread_list()` reads `idx->fsm_table_count * 4` bytes starting at `idx->fsm_table_addr` in one `updi_mem_read()` call to get all entries. Each `state` field is then dereferenced via a separate 2-byte `updi_mem_read()` to obtain the current state function pointer (a FLASH word address), and each `name` pointer is dereferenced via a `updi_mem_read()` of up to 31+1 bytes.

**g-packet register buffer specification (critical — buffer must be exactly 79 bytes):**

The AVR GDB register frame contains 39 bytes encoded as 78 ASCII hex characters plus a NUL terminator. `reg_buf` must be at least 79 bytes.

| GDB reg index | Register | Bytes in buffer | Hex char positions | Notes |
| ------------- | -------- | --------------- | ------------------ | ----- |
| 0–31 | R0–R31 | 0–31 | `[0..63]` | Zeroed for all threads |
| 32 | SREG | 32 | `[64..65]` | Live for active thread; zero otherwise |
| 33 | SPL | 33 | `[66..67]` | Live for active thread; zero otherwise |
| 34 | SPH | 34 | `[68..69]` | Live for active thread; zero otherwise |
| 35 | PC | 35–38 | **`[70..77]`** | `thread->state_fn`, 4-byte **little-endian** |

PC encoding example: if `state_fn = 0x0000021A`, the 8 hex chars at positions 70–77 are `"1A020000"` (byte 0 = `0x1A` → `"1A"`, byte 1 = `0x02` → `"02"`, bytes 2–3 = `0x00` → `"0000"`).

The `reg_buf` is a plain `char` array (not `uint8_t *`) — `fsm_get_registers()` writes ASCII hex directly using `snprintf` or equivalent. The caller in `gdb_rsp.c` passes it straight to `rsp_send_packet()`.

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

**Complete `RspHandlers` dispatch table — all 15 entries:**

| Handler field | Packet prefix(es) | Action summary |
| ------------- | ----------------- | -------------- |
| `on_halt_reason` | `?` | Return `T05thread:<active_id>;` |
| `on_read_regs` | `g` | `fsm_get_registers(g_thread)` → 78-char hex string |
| `on_write_regs` | `G<hex>` | Parse 78 hex chars; update live SREG/SP via `updi_mem_write()` |
| `on_read_mem` | `m <addr>,<len>` | `updi_mem_read()` → hex-encoded bytes |
| `on_write_mem` | `M <addr>,<len>:<data>` | Hex-decode `<data>` → `updi_mem_write()` |
| `on_continue` | `c`, `vCont;c[:<tid>]` | `updi_run()` + `fsm_invalidate()` → wait for halt → `T05...` |
| `on_step` | `s`, `vCont;s[:<tid>]` | `updi_step()` + `fsm_invalidate()` → `T05thread:<id>;` |
| `on_insert_bp` | `Z0,<addr>,<kind>` | Read + save 2-byte word at `addr`; write `0x9598` via `updi_nvm_write_flash()` |
| `on_remove_bp` | `z0,<addr>,<kind>` | Restore saved word via `updi_nvm_write_flash()`; clear table slot |
| `on_thread_info` | `qfThreadInfo`, `qsThreadInfo` | Enumerate thread IDs from `FsmContext`; `m<id>,<id>,...` then `l` |
| `on_thread_extra` | `qThreadExtraInfo,<id>` | Return `FsmThread.name` hex-encoded (O-packet ASCII encoding) |
| `on_set_thread_g` | `H g <id>` | Store `g_thread = id` → `OK` |
| `on_set_thread_c` | `H c <id>` | Store `c_thread = id` → `OK` |
| `on_monitor` | `qRcmd,<hex>` | Hex-decode `<hex>` payload → `monitor_dispatch()` |
| `on_detach` | `D`, `k` | `updi_run()`; clear breakpoint table; close `gdb_fd`; reset `g_thread`/`c_thread` to 0 |

**Inline-handled packets** (fixed responses, not in `RspHandlers`):

| Packet | Response |
| ------ | -------- |
| `qSupported` | `"PacketSize=800;QStartNoAckMode+;multiprocess-;vContSupported+"` |
| `qAttached` | `"1"` |
| `QStartNoAckMode` | `"OK"` then set internal no-ack flag (stop sending `+`/`-`) |
| Unknown packet | `""` (empty response — mandatory per RSP spec) |

**Breakpoint table entry struct and AVR BREAK opcode:**

```c
struct { uint32_t addr; uint16_t saved_word; } bp_table[RSP_MAX_BREAKPOINTS]; /* 96 bytes BSS */
```

AVR BREAK opcode: `0x9598` (16-bit instruction, written as little-endian bytes `{0x98, 0x95}` to FLASH). Insertion requires the CPU to be halted (`updi_halt()` is called before `Z0` handler runs if not already stopped). Breakpoint table full → return error packet `E08`.

**O-packet encoding** for `monitor_dispatch()` and `on_thread_extra`: each ASCII byte → two uppercase hex chars. Example: `"OK\n"` → `"4F4B0A"`. Build plain text into a 512-byte staging buffer first, then hex-encode the whole buffer in one pass before calling `rsp_send_packet(rsp_fd, "O" + encoded)`.

**`qRcmd` hex-decode procedure** in `monitor_dispatch()`: the payload after `qRcmd,` is a hex-encoded ASCII command string. Decode pairs of hex chars → bytes to recover the plain text command. Verify it starts with `"avros "` (6 chars, case-sensitive) before dispatching.

---

### Phase 5 — Application Entry Point + Integration

1. `src/main.c` — implement `AppConfig` struct; `parse_args()` with defaults (`gdb_port=1234`, `baud_rate=115200`) and `fprintf(stderr, ...); exit(1)` on error; `event_loop()` with `select()` on up to 3 fds (listen socket, UPDI fd, GDB client fd); SIGINT/SIGTERM handler sets `volatile sig_atomic_t g_quit = 1` only; teardown order: `rsp_close(gdb_fd)` → `rsp_close(listen_fd)` → `elf_close()` → `updi_close()`.
2. `tests/test_main.c` — 14 Unity tests. `fork()` + `waitpid()` pattern for `exit()` path tests; pipes capture stderr output; signal test calls the handler function directly and checks `g_quit == 1`. Linked with `--wrap` flags for all 5 module init/close functions plus `--wrap,select`.
3. `tests/test_integration.c` — 4 integration tests. Builds the real `avrOSdb` binary as a Makefile prerequisite; launches it via `fork()`/`execv()` with a PTY as the UART device and a local TCP port; a helper thread simulates the AVR side (responds to UPDI BREAK+SYNCH, handles `updi_mem_read` sequences); a raw TCP socket connects as the GDB client and sends RSP packets; tests measure startup latency (HLR-005), verify RSP packet purity (HLR-020), check build portability (HLR-033), and verify no non-libc dependencies (HLR-034).

**Acceptance:** `make test` runs all 7 test binaries; all 106 tests pass (14 + 23 + 11 + 13 + 28 + 14 + 4). `make` builds the final `avrOSdb` binary without warnings. `ldd avrOSdb` shows only libc. `python3 tools/lint_project.py` reports 0 errors, 0 warnings.

**CLI argument specification:**

`avrOSdb [--port <port>] [--baud <baud>] [--load] <serial-device> <elf-file>`

| Argument | Type | Default | Validation in `parse_args()` |
| -------- | ---- | ------- | ---------------------------- |
| `--port <port>` | `uint16_t` | `1234` | `1 ≤ port ≤ 65535`; print usage + `exit(1)` on error |
| `--baud <baud>` | `int` | `115200` | Must be a positive integer; baud-to-`Bxxx` mapping validated at `updi_open()` time |
| `--load` | flag | `false` | No argument; enables `updi_nvm_write_flash()` before entering event loop |
| `<serial-device>` | `const char *` | (required) | Positional; missing → usage + `exit(1)` |
| `<elf-file>` | `const char *` | (required) | Positional; missing → usage + `exit(1)` |

**Exit code table:**

| Code | Condition |
| ---- | --------- |
| `0` | Normal exit after SIGINT/SIGTERM or `k` (kill) packet |
| `1` | Bad arguments, serial device open failure, or TCP socket bind failure |
| `1` | `--load` flash write failure (UPDI error or write-protect) |

**`AppConfig` struct fields:**

| Field | Type | Description |
| ----- | ---- | ----------- |
| `serial_device` | `const char *` | Path to UART device (argv pointer; not copied) |
| `elf_path` | `const char *` | Path to AVR ELF binary (argv pointer; not copied) |
| `gdb_port` | `uint16_t` | TCP port for GDB listener |
| `baud_rate` | `int` | UART baud rate |
| `load_flash` | `bool` | When true, flash the ELF before attaching |
| `listen_fd` | `int` | Passive TCP listener socket fd |
| `gdb_fd` | `int` | Active GDB client fd; `-1` when no client connected |
| `updi_fd` | `int` | UART serial device fd |

**`event_loop()` decision tree (6 steps per iteration):**

1. **Check `g_quit`** at top of loop — if set by SIGINT/SIGTERM handler, `return` immediately.
2. **Build `fd_set`**: always add `listen_fd`; add `gdb_fd` and `updi_fd` only when `gdb_fd >= 0`. Recompute `maxfd = max(listen_fd, gdb_fd, updi_fd)` before each call.
3. **`select(maxfd+1, &rds, NULL, NULL, NULL)`** — no timeout; block until at least one fd is readable.
4. **`listen_fd` ready** and `gdb_fd == -1`: call `rsp_accept()` → `elf_open()` → `elf_find_avros_tables()` → `fsm_build_thread_list()` → store result in `cfg->gdb_fd`.
5. **`updi_fd` ready**: call `updi_console_poll()` → `write(STDOUT_FILENO, buf, n)`.
6. **`gdb_fd` ready**: call `rsp_recv_packet()`; on 0 bytes (disconnect) close + set `gdb_fd = -1`; otherwise `rsp_dispatch()`.

**Teardown sequence** (guaranteed order on all exit paths including `exit(1)` from `parse_args()`):

1. `rsp_close(cfg.gdb_fd)` if `gdb_fd >= 0`
2. `rsp_close(cfg.listen_fd)`
3. `elf_close(&elf_ctx)`
4. `updi_close(cfg.updi_fd)`
5. `return exit_code` from `main()`

---

### Phase 6 — Installation Targets & Documentation

1. **`make check-tools`** — pre-flight target that validates all required host tools are present (`gcc`/`cc`, `make`, `avr-gcc`, `avr-nm`). Prints a diagnostic naming each missing tool and exits non-zero if any are absent. This target must complete successfully before any build attempt.
2. **`make install`** — installs the compiled `avrOSdb` binary to `$(PREFIX)/bin/` (default `PREFIX=/usr/local`) and the man page to `$(PREFIX)/share/man/man1/`. Creates missing intermediate directories via `install -d`. Binary installed mode 0755; man page mode 0644.
3. **`make uninstall`** — removes `$(PREFIX)/bin/avrOSdb` and `$(PREFIX)/share/man/man1/avrOSdb.1` with `rm -f`. Idempotent — exits 0 even if files are already absent.
4. **`make bundle`** — produces native distribution packages under `dist/` for two platforms:
   - `dist/avrOSdb_$(VERSION)_amd64.deb` — Debian/Ubuntu binary package built with `dpkg-deb`. Includes binary (mode 0755) and man page (mode 0644). `DEBIAN/control` declares `Package`, `Version`, `Architecture: amd64`, `Maintainer`, `Description`.
   - `dist/avrOSdb-$(VERSION)-1.x86_64.rpm` — Red Hat/Fedora RPM built with `rpmbuild`. Generated `.spec` declares `Name`, `Version`, `Release`, `Summary`, `License`, `%install`, `%files`.
   A `VERSION` variable (default: `git describe --tags --always`) parameterises all three package version strings. This is found in the file called `VERSION` in the project root directory
5. **`doc/UserManual.md`** — hand-authored user manual covering: prerequisites + minimum versions, build instructions (`make`, `make test`, `make install`), connection wiring for the UPDI serial adapter (1 kΩ resistor, TX/RX orientation), all CLI options, at least two complete usage examples (one with `--load`, one without), and a desciption of how to integrate with VS Code's built in debuger.
6. **`doc/avrOSdb.1`** — Unix man page in `groff` format. Required sections: NAME, SYNOPSIS, DESCRIPTION, OPTIONS, OPERANDS, EXIT STATUS, EXAMPLES, SEE ALSO. Must parse cleanly under `man -l doc/avrOSdb.1` on Linux.
7. **`tests/test_install.c`** — 8 integration-style tests: (a) `check-tools` exits non-zero on missing tool; (b) `make install` places binary at correct prefix path; (c) `make install` places man page and it renders without error; (d) `make uninstall` removes installed files; (e) user manual exists and contains all required section headings; (f) `make bundle` produces a valid `.deb`; (g) `make bundle` produces a valid `.rpm`; (h) `make bundle` produces a valid Homebrew formula.

**Acceptance:** `make check-tools` exits 0 when all tools are present. `make install PREFIX=/tmp/test` and `make uninstall PREFIX=/tmp/test` succeed. `make bundle VERSION=0.1.0` produces all three artefacts under `dist/`. `man -l doc/avrOSdb.1` exits 0. `make test` runs `tests/test_install` and reports 8/8 passing. `python3 tools/lint_project.py` reports 0 errors, 0 warnings.

**`make install` implementation pattern:**

```makefile
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin
MANDIR  := $(PREFIX)/share/man/man1

install: all
	install -d $(BINDIR) $(MANDIR)
	install -m 0755 $(BINFILE) $(BINDIR)/avrOSdb
	install -m 0644 doc/avrOSdb.1 $(MANDIR)/avrOSdb.1

uninstall:
	rm -f $(BINDIR)/avrOSdb $(MANDIR)/avrOSdb.1

check-tools:
	@command -v $(CC)      >/dev/null 2>&1 || { echo "ERROR: C compiler not found ($(CC))"; exit 1; }
	@command -v avr-gcc    >/dev/null 2>&1 || { echo "ERROR: avr-gcc not found"; exit 1; }
	@command -v avr-nm     >/dev/null 2>&1 || { echo "ERROR: avr-nm not found"; exit 1; }
	@echo "All required tools found."
```

**`doc/avrOSdb.1` man page required sections:**

| Section | Content |
| ------- | ------- |
| `NAME` | `avrOSdb — UPDI-to-GDB stub with avrOS FSM awareness` |
| `SYNOPSIS` | `avrOSdb [--port port] [--baud baud] [--load] serial-device elf-file` |
| `DESCRIPTION` | Overview of UPDI bridging and avrOS FSM virtual threads |
| `OPTIONS` | `--port`, `--baud`, `--load` with types, defaults, and constraints |
| `OPERANDS` | `serial-device` and `elf-file` positional arguments |
| `EXIT STATUS` | Codes 0 and 1 with conditions |
| `EXAMPLES` | `avrOSdb /dev/ttyUSB0 firmware.elf` and `avrOSdb --load --port 1234 /dev/ttyUSB0 firmware.elf` |
| `SEE ALSO` | `avr-gdb(1)`, `avrdude(1)` |

### Phase 7 — Device-Signature Diagnostic Mode

1. **`--device` CLI flag** — when present on the command line, `avrOSdb` shall enter a one-shot diagnostic mode that opens the UPDI link, reads the target SIGROW and ASI status registers, prints a verbose human-readable report to `stdout`, and exits without binding the GDB listener. The `<elf-file>` operand shall be optional in this mode. Mode is mutually exclusive with `--load`; specifying both is a usage error.

2. **`updi_read_device_info()`** — new public function in `src/updi.c`/`src/updi.h`. Returns a populated `UpdiDeviceInfo` struct (DEVICEID0..2, REVID, 10-byte SERNUM, ASI_SYS_STATUS, ASI_KEY_STATUS, ASI_STATUSB, and any UPDI errors encountered during the read). The function shall be non-destructive — no CPU halt and no NVM activity — and shall return 0 on success or a negative UPDI error code on failure. SIGROW is read from physical address 0x1100; REVID from SYSCFG offset 0x0F01.

3. **Verbose error reporting.** Every UPDI operation invoked from `--device` mode shall, on failure, print a diagnostic line to `stderr` that names the operation, the address attempted, the bytes received (if any), and a human-readable interpretation of the UPDI status flags. The intent is that a wiring or fuse problem (UPDIDIS asserted, line stuck low, no SYNCH response) produces an actionable message rather than a silent `exit 1`.

4. **Device-family lookup table.** A static table in `src/main.c` maps the 3-byte SIGROW signature to a human-readable AVR DA/DB/DD/EA part name. Unknown signatures shall be printed as `unknown device (signature %02X %02X %02X)`. The table covers at minimum: `AVR32DA28`, `AVR64DA28/32/48`, `AVR128DA28/32/48/64`, `AVR32DB28`, `AVR64DB28/32/48`, `AVR128DB28/32/48/64`.

5. **Documentation updates.**
   - `doc/UserManual.md` shall gain a new top-level section (between current §4 Command-Line Invocation and §5 Examples) describing `--device` and showing one annotated example output.
   - `doc/avrOSdb.1` shall add `--device` under OPTIONS and a third example under EXAMPLES.

6. **`tests/test_device.c`** — 6 tests:
   - (a) `parse_args` accepts `--device` and clears the `<elf-file>` mandatory-operand rule.
   - (b) `parse_args` rejects `--device` combined with `--load` (exit 1, diagnostic on stderr).
   - (c) `updi_read_device_info()` returns success and the expected struct fields when fed a mocked SIGROW byte stream over a PTY.
   - (d) `updi_read_device_info()` returns a negative error code and `g_last_updi_error` carries a descriptive string when the target sends a NAK.
   - (e) End-to-end: subprocess invocation with `--device` and a mock-PTY-backed serial device produces stdout output containing `Signature:`, `Family:`, `UPDI status:`, and `Serial:` lines.
   - (f) The device-mode path does **not** call `rsp_listen()` (verified by linking against a stub that aborts the test if invoked).

**Acceptance:** `make test` shows `tests/test_device` passing 6/6. `avrOSdb --device /dev/ttyUSB0` against real hardware prints a recognisable signature and exits 0. Against a disconnected adapter the same command exits 1 with a diagnostic that explicitly names the failed UPDI operation (BREAK, SYNCH, or first CS read). `python3 tools/lint_project.py` reports 0 errors, 0 warnings after Project.xml is updated with HLR-044 and the new LLRs.

**CLI synopsis after Phase 7:**

```
avrOSdb [--port port] [--baud baud] [--load] <serial-device> <elf-file>
avrOSdb --device [--baud baud] <serial-device> [elf-file]
```

**Example output (real `AVR128DA48`):**

```
$ avrOSdb --device /dev/ttyUSB0
Serial device:   /dev/ttyUSB0
Baud rate:       115200
UPDI link:       up (SYNCH ack in 412 µs)
Signature:       1E 97 0A
Family:          AVR128DA48
Revision:        A6
Serial:          00 1A 2B 3C 4D 5E 6F 70 81 92
UPDI status:     SYS_STATUS=0x82  KEY_STATUS=0x10  STATUSB=0x00
NVM state:       idle, no programming key active
```

---

### Phase 8 — Non-FLASH NVM Programming

**Motivation.** `--load` currently programs only the FLASH window. Any `.eeprom`, `.fuse`, `.lock`, or `.user_signatures` segment present in the ELF is either silently skipped (when its VMA falls above `sram_base`) or, worse, fed to `updi_nvm_write_flash()` with a non-FLASH address — neither result reaches the intended NVM. Phase 8 generalises `--load` to dispatch every programmable ELF segment to the correct NVMCTRL command path while leaving the existing FLASH path byte-for-byte unchanged.

**Scope summary.**

| ELF section | UPDI window (AVR-Dx) | NVMCTRL command | Granularity | New routine |
| ----------- | -------------------- | --------------- | ----------- | ----------- |
| `.text`, `.data` (LMA) | `0x800000`–`0x80FFFF`+ | `FLWR` (existing) | 512 B page | *(unchanged)* |
| `.eeprom` | `0x814000`–`0x8143FF` | `EEERWR` / `EEBER` | 1 B / 1 B | `updi_nvm_write_eeprom()` |
| `.user_signatures` (USERROW) | `0x810080`–`0x8100FF` | `EEERWR` | 1 B | `updi_nvm_write_userrow()` |
| `.fuse` | `0x820000`–`0x82001F` | `EEERWR` (FUSES live in the EEPROM-mapped region on Dx) | 1 B | `updi_nvm_write_fuses()` |
| `.lock` | `0x820040`–`0x820043` | `EEERWR` (chip-erase required first) | 1 B | `updi_nvm_write_lockbits()` |
| `.signature` (SIGROW) | `0x811080`+ | *(read-only — skipped, never an error)* | — | — |

Exact base addresses are part-specific; the dispatcher classifies segments by address window (not by ELF section name) so the same logic works across AVR-Dx variants.

1. **`src/updi.h` / `src/updi.c`** — add four NVM routines mirroring `updi_nvm_write_flash()` semantics (re-enter NVMPROG, BUSY poll, leave CPU halted):
   - `int updi_nvm_write_eeprom(int fd, uint32_t addr, const uint8_t *data, size_t len);` — emit NVMCTRL `EEERWR` command (`0x13`) and write `data` byte-by-byte; BUSY poll bounded at 20 ms per byte burst.
   - `int updi_nvm_write_fuses(int fd, uint32_t addr, const uint8_t *data, size_t len);` — same NVM command sequence but with an explicit window guard: refuse any address outside `[UPDI_FUSES_BASE, UPDI_FUSES_BASE + UPDI_FUSES_SIZE)`.
   - `int updi_nvm_write_userrow(int fd, uint32_t addr, const uint8_t *data, size_t len);` — same again with the USERROW window guard.
   - `int updi_nvm_write_lockbits(int fd, uint32_t addr, const uint8_t *data, size_t len);` — same again; additionally checks `ASI_SYS_STATUS` for the post-chip-erase state and returns `UPDI_ERR_LOCKED` (new error code) if the precondition is not met.
   - New constants in `src/updi.h`: `UPDI_EEPROM_BASE`, `UPDI_EEPROM_SIZE`, `UPDI_USERROW_BASE`, `UPDI_USERROW_SIZE`, `UPDI_FUSES_BASE`, `UPDI_FUSES_SIZE`, `UPDI_LOCK_BASE`, `UPDI_LOCK_SIZE`, `UPDI_SIGROW_BASE`, `UPDI_SIGROW_SIZE`, `UPDI_ERR_LOCKED -3`.
   - Refactor the NVMPROG-entry + BUSY-poll boilerplate shared with `updi_nvm_write_flash()` into a private `nvm_eeprom_write_bytes()` helper so the four EEPROM-class writers stay short.

2. **`src/main.c` `load_flash_segments()` → `load_segments()`.** Replace the current single-call dispatcher with a window classifier that routes each `PT_LOAD` segment to the correct NVM routine:
   - `[UPDI_FLASH_BASE, UPDI_FLASH_BASE + flash_size)` → `updi_nvm_write_flash()` (existing path, byte-for-byte unchanged).
   - `[UPDI_EEPROM_BASE, UPDI_EEPROM_BASE + UPDI_EEPROM_SIZE)` → `updi_nvm_write_eeprom()`.
   - `[UPDI_USERROW_BASE, UPDI_USERROW_BASE + UPDI_USERROW_SIZE)` → `updi_nvm_write_userrow()`.
   - `[UPDI_FUSES_BASE, UPDI_FUSES_BASE + UPDI_FUSES_SIZE)` → `updi_nvm_write_fuses()`.
   - `[UPDI_LOCK_BASE, UPDI_LOCK_BASE + UPDI_LOCK_SIZE)` → `updi_nvm_write_lockbits()` (require `--erase`; reject with diagnostic otherwise).
   - `[UPDI_SIGROW_BASE, UPDI_SIGROW_BASE + UPDI_SIGROW_SIZE)` → skipped with informational log line on `stdout` (`SIGROW segment ignored (read-only)`).
   - SRAM segments (VMA in `[sram_base, sram_base + sram_size)`) → skipped (existing `.data` initialiser behaviour preserved).
   - Any other address → hard error: print `error: segment vaddr 0x%06x not in any programmable NVM window` to `stderr` and return -1.
   The current "vaddr ≥ sram_base ⇒ skip" filter is replaced by the explicit window classifier; the SRAM skip is now one branch among many rather than the default.

3. **Lockbit safety guard.** `updi_nvm_write_lockbits()` shall refuse to write the AVR-Dx UPDIDIS pattern (the lock value that disables the UPDI link permanently from the host's perspective until the next chip-erase via the high-voltage UPDI sequence) unless an explicit new CLI flag `--allow-lock-updi` is present. Without the flag the function returns `UPDI_ERR_LOCKED` and `main.c` prints a single-line diagnostic naming the segment and exit code 1.

4. **Post-load OCD re-entry.** Writing FUSES typically requires a CPU reset to take effect. After `load_segments()` completes, if any non-FLASH segment was written, `main.c` shall re-issue `updi_enter_debug()` before falling through to `rsp_listen()` so the OCD state is consistent.

5. **TraceR doc updates** (next free IDs at edit time):
   - SDD §3.x (main.c) — replace the `load_flash_segments()` paragraph with the multi-window dispatcher description and timing table additions.
   - SDD §4.x (updi.c) — extend the function-interface block with the four new entry points and the new constants; add EEPROM / FUSE / USERROW BUSY-timeout rows to the timing table.
   - **HLR-046 "Non-FLASH NVM Programming"** — new HLR enumerating which ELF section windows are programmed and the required NVMCTRL commands.
   - **HLR-047 "Lockbit Programming Safety Interlock"** — new HLR requiring `--allow-lock-updi` for lockbit writes that disable UPDI access.
   - **HLR-004 "ELF Flash Load Option"** — broadened to "ELF Load Option" with explicit list of programmable section windows.
   - **HLR-009 "FLASH Programming"** — wording generalised to retain FLASH as one NVM kind among several.
   - New LLRs (illustrative): `LLR-UPDI-NN..NN+3` for the four NVM routines + window guards; `LLR-MAIN-NN` for the `load_segments()` window-classifier dispatcher and SIGROW-skip behaviour.

6. **`tests/test_updi.c`** — per new NVM routine, add:
   - Success path — correct UPDI byte sequence (NVMPROG re-entry, EEERWR command, page burst, BUSY poll exit).
   - BUSY-timeout failure → return -1.
   - Out-of-window address → return -1 without driving any UPDI traffic.
   - For `updi_nvm_write_lockbits()` additionally: missing-chip-erase precondition → `UPDI_ERR_LOCKED`; UPDIDIS pattern without `--allow-lock-updi` → `UPDI_ERR_LOCKED`.

7. **`tests/test_main.c`** — extend with dispatcher tests:
   - Synthetic ELF whose `PT_LOAD` segments cover all five programmable windows → each `__wrap_updi_nvm_write_*` invoked exactly once with the correct address and payload length.
   - SIGROW-window segment → no NVM routine invoked; informational line printed on `stdout`.
   - Unknown-window segment → exit code 1, diagnostic on `stderr`, no NVM routine invoked.
   - Lockbit segment without `--erase` → exit code 1, diagnostic.
   - Post-load OCD re-entry: when any non-FLASH segment is written, `__wrap_updi_enter_debug` invocation count increases by one before `rsp_listen()` is called.

8. **`tests/fixtures/`** — add a small linker script (`tests/fixtures/all_nvm.ld`) and source (`tests/fixtures/all_nvm.c`) producing `tests/fixtures/all_nvm.elf` containing `.text`, `.data`, `.eeprom`, `.user_signatures`, `.fuse`, and `.lock` sections at their canonical AVR-Dx LMAs. Makefile rule mirrors the existing avros fixture build.

9. **Makefile** — `TEST_WRAP_test_updi` adds no new wraps (the new NVM routines are the units under test). `TEST_WRAP_test_main` adds `updi_nvm_write_eeprom`, `updi_nvm_write_userrow`, `updi_nvm_write_fuses`, `updi_nvm_write_lockbits`.

10. **CLI surface.** Add `--allow-lock-updi` to `parse_args()`. Help text and synopsis updated accordingly:

    ```
    avrOSdb [--port port] [--baud baud] [--erase] [--load] [--allow-lock-updi]
                 <serial-device> <elf-file>
    ```

11. **Documentation updates.**
    - `doc/UserManual.md` §5.2 "Flash and debug from cold" — enumerate non-FLASH sections programmed by `--load`; document the `--erase` requirement for lockbit segments; add a fuse-byte example.
    - `doc/UserManual.md` — new subsection "Programming fuses, EEPROM, and lockbits" describing the section→window mapping, the `--allow-lock-updi` interlock, and recovery from a UPDIDIS-locked target.
    - `doc/avrOSdb.1` — add `--allow-lock-updi` under OPTIONS; expand `--load` description with the section list.
    - `README.md` — replace the FLASH-only sentence with "programs FLASH, EEPROM, fuses, USERROW, and lockbits".

**Acceptance:**
- `make test` runs all suites including the extended `test_updi` and `test_main`; new tests pass.
- `python3 tools/lint_project.py` reports 0 errors, 0 warnings after Project.xml carries HLR-046, HLR-047, and the new LLRs.
- `build/avrOSdb --erase --load /dev/ttyUSB0 build/fixtures/all_nvm.elf` against real hardware programs every section and exits 0; reading the target back with `--device` shows the expected fuse / lock bytes.
- `build/avrOSdb --load /dev/ttyUSB0 build/fixtures/all_nvm.elf` (no `--erase`) against a chip with a `.lock` segment exits 1 with a diagnostic naming the missing `--erase`.

**Out of scope (explicit, deferred to a later phase):**
- Reading EEPROM / USERROW back for verification after a load.
- GDB `M`-packet runtime writes to non-FLASH NVM (the existing FLASH/data-space split in `dh_write_mem` is preserved; runtime fuse writes are a separate, dangerous feature).
- AVR Tiny / Mega-0 / EA family NVM support — the EEERWR command code and fuse base differ; out of scope per HLR-006 (AVR-Dx only).

**Risks.**
- **Lockbit programming is irreversible without a high-voltage UPDI re-enable** if the user locks UPDI out. The `--allow-lock-updi` interlock is the primary mitigation; the user manual must call this out prominently.
- **AVR-Dx variant address-window differences.** AVR DA / DB / DD have the same EEPROM / USERROW / FUSE / LOCK base addresses but differ in EEPROM size. The window-size constants in `src/updi.h` must use the smallest common size and reject over-large segments rather than silently truncating.
- **Post-write OCD state.** Some NVM commands leave the CPU in an indeterminate state. The post-load `updi_enter_debug()` re-issue (item 4) is the chosen mitigation; tests must verify it fires on every non-FLASH path.

---

### Phase 10 — GDB Protocol Completion / avarice Feature Parity

**Motivation.** Phases 1–8 implement the minimum subset of GDB RSP needed to attach, read state, single-step, set up to two hardware breakpoints, continue, interrupt, and detach. The legacy `avarice` JTAG/dW stub — which `avrOSdb` is intended to supersede for AVR-Dx UPDI targets — supports a substantially larger protocol surface. Phase 10 brings `avrOSdb` up to functional parity with that surface: equivalent `monitor` verbs, equivalent `(gdb) load` behaviour, equivalent watchpoint and software-breakpoint experience, and equivalent extended-remote lifecycle. Wire-level drop-in compatibility with `avarice`-specific scripts is an explicit non-goal — individual `.gdbinit` files, IDE launch configurations, and CI invocations may still need targeted edits to reach the same outcome. Tracked as GitHub issue #31; bound to HLR-053 … HLR-059 in Project.xml §12.

**Scope summary.**

| Feature | GDB packets / verbs | Source file(s) | HLR |
| ------- | ------------------- | -------------- | --- |
| Flash programming from GDB session | `vFlashErase`, `vFlashWrite`, `vFlashDone` | `gdb_rsp.c`, `updi.c` (reuse Phase 8 NVM path) | HLR-053 |
| True software breakpoints via flash `BREAK` opcode | `Z0`/`z0` (real, not aliased) | `gdb_rsp.c`, `updi.c` | HLR-054 |
| `avarice`-compatible monitor verbs | `monitor reset/halt/go/erase/chip-erase/version/bp-mode/help` | `monitor.c` | HLR-055 |
| Hardware data watchpoints | `Z2`/`Z3`/`Z4` (+ matching `z*`) | `gdb_rsp.c` (empty-packet reply; silicon does not expose data-WPs) | HLR-056 |
| Extended-remote lifecycle | `vRun`, `vAttach`, `vKill` + `multiprocess+` | `gdb_rsp.c` | HLR-058 |
| Protocol cleanup | `qC`, `qOffsets`, `T<tid>`, `R<XX>` | `gdb_rsp.c` | HLR-059 |

1. **`src/gdb_rsp.c` — `vFlash*` handlers (HLR-053).** Add three new handler entries to the RSP dispatcher table: `on_v_flash_erase`, `on_v_flash_write`, `on_v_flash_done`. Reject any erase range outside the FLASH window of the runtime-selected device (HLR-048) with reply `E22`; accumulate `vFlashWrite` payload into the existing page buffer; flush on `vFlashDone` and call `updi_enter_debug()` to leave the CPU halted at reset. Both HW and SW breakpoint shadows shall be cleared on `vFlashDone`. While a `vFlash*` transaction is in progress the server shall reply `E22` to any `g`/`G`/`m`/`M`/`c`/`s` packet and discard pending page buffers.

2. **`src/gdb_rsp.c` + `src/updi.c` — true SW breakpoints (HLR-054).** Add per-session shadow map `RspContext.sw_bp[N_SW_BP_MAX]` recording `(gdb_addr, flash_byte_addr, original_lo, original_hi, page_aligned_base)`. On `Z0` patch `0x9598` (little-endian) into the FLASH word at the target address; on `z0` restore the original opcode. Patch sequence: snapshot R0–R31, SREG, SP, PC via OCD register file → enter NVMPROG → page-aligned read-modify-write → re-enter OCD → restore registers. Refuse `Z0` in non-FLASH windows with `E22`. Operator opt-out via `monitor avros bp-mode hw-only` (HLR-055) reverts `Z0` to the Phase 1–8 HW-alias behaviour for the server-process lifetime.

3. **`src/monitor.c` — `avarice`-compatible monitor verbs (HLR-055).** Extend `monitor_dispatch()` to recognise the top-level verbs `reset`, `halt`, `go`, `erase`, `chip-erase`, `version`, `bp-mode`, and `help` in addition to the existing `avros` namespace. `monitor erase` / `chip-erase` require a new `--allow-erase` server-launch flag; without it the verb shall reply with an O-packet diagnostic followed by `E22`. `monitor reset` and `monitor chip-erase` shall invalidate the FSM thread cache before issuing silicon side-effects so `qfThreadInfo` is coherent on the next query. `monitor version` emits one O-packet carrying server version, git short SHA, build date, and active family name from HLR-048. `monitor help` emits one O-packet per recognised verb plus a closing `OK`.

4. **`src/gdb_rsp.c` — data watchpoints reply empty packet (HLR-056).** During Phase-10 hardware bring-up the planned `updi_ocd_set_data_bp()` / `_clear_data_bp()` primitives and the speculative `OCD_DABP*` / `OCD_CTRL2` register addresses were demonstrated to be ineffective: writes succeed at the UPDI level but the silicon never halts on a matching access. An exhaustive FF-bomb (see `doc/reference/guesswork.md`) finds no writable bits at the fabricated offsets, and three independent reference debuggers — Bloom, `feline-felicity/avr-absurd` (same SerialUPDI architecture), and Microchip's own `mraardvark/pyavrdebug` (CMSIS-DAP / Atmel-ICE stack) — all confirm no data-watchpoint hardware is exposed over UPDI. `pyavrdebug` in particular replies the empty packet (`$#00`) to Z2/Z3/Z4 in its `Z`-packet handler, which is the exact behaviour adopted here. The `dh_insert_bp()` / `dh_remove_bp()` handlers therefore reply the empty packet for `Z[234]` / `z[234]`; GDB transparently falls back to software watchpoints (single-step + memory poll). No watchpoint capability is advertised in `qSupported`. The fabricated `DABP0` / `DABP1` / `CTRL2` register defines and the two UPDI primitives are removed.

5. **`src/gdb_rsp.c` — extended-remote lifecycle (HLR-058).** Implement `vRun;<elf-path>;...` (re-parse the ELF, rebuild the FSM context, reply with a `T05` at the reset vector), `vAttach;<pid>` (reply `OK` + standard stop-reply; `pid` informational), `vKill;<pid>` (call `updi_run()`, close the GDB client fd, return to listen state without exiting the server). Existing `D` and `k` handlers unchanged. Extend `qSupported` with `multiprocess+;vRun+;vAttach+;vKill+`; remove the `multiprocess-` entry.

6. **`src/gdb_rsp.c` — protocol cleanup (HLR-059).** Add four small handlers: `qC` → `QC<tid>` (or `0` when no selected thread); `qOffsets` → `Text=0;Data=0;Bss=0`; `T<tid>` → `OK` when present in `FsmContext.threads[]`, else `E01`; `R<XX>` → behaves as `monitor reset` followed by `c` and reports the resulting stop packet. No execution-state changes beyond those explicit semantics.

7. **CLI surface.** Add `--allow-erase` to `parse_args()` (gates `monitor erase` / `chip-erase` per item 3). Help and synopsis updated accordingly. The `monitor avros bp-mode hw-only` runtime toggle (item 2) is per-session, not a CLI flag.

8. **Tests — `tests/test_rsp.c`.** Add cases for: vFlashErase rejection of out-of-window addresses; vFlashWrite page accumulation; vFlashDone re-arms OCD and clears the BP shadow; SW-BP install patches the right two bytes and removes restore the originals; `Z2`/`z3` reply the empty RSP packet (unsupported — HLR-056); `vRun` / `vAttach` / `vKill` lifecycle sequence; `qC` / `qOffsets` / `T<tid>` / `R` minimal handlers.

9. **Tests — `tests/test_monitor.c`.** Add cases for each new verb in HLR-055: `reset`, `halt`, `go`, `chip-erase` (refused without `--allow-erase`, accepted with), `version` (O-packet content), `bp-mode hw-only` and `bp-mode sw` (state toggle visible to subsequent `Z0`), and `help` (one O-packet per known verb plus `OK`).

10. **Tests — `tests/test_updi.c`.** No new cases for data watchpoints (HLR-056 is implemented entirely in `gdb_rsp.c` as an empty-packet reply; no UPDI primitive exists).

11. **Tests — `tests/test_main.c`.** Add a case verifying that `--allow-erase` is parsed into `AppConfig` and forwarded to the monitor dispatcher.

12. **Documentation updates.**
    - `doc/UserManual.md` — new subsection "Feature parity with `avarice`" listing the supported monitor verbs and the `--allow-erase` flag, and noting that `.gdbinit` / IDE / CI scripts written for `avarice` may need targeted edits. Update the breakpoint section: explain HW vs SW BP modes and the `monitor avros bp-mode` toggle.
    - `doc/avr-updi-gdb.1` — add `--allow-erase` under OPTIONS; document the new monitor verbs under MONITOR COMMANDS.
    - `README.md` — add "feature-parity replacement for `avarice` on AVR-Dx UPDI" to the feature bullets.
    - SDD §5.x (gdb_rsp.c) and §8.x (monitor.c) — extend the function-interface tables with the new handler entries and the new monitor verbs; SDD overview Phase 10 paragraph already in place.

**Acceptance:**
- `make test` runs all suites including the extended `test_rsp`, `test_monitor`, `test_updi`, `test_main`; new tests pass.
- `python3 tools/lint_project.py` reports 0 errors, 0 warnings after every HLR-053 … HLR-059 is traced from at least one new LLR and one new test.
- Against real hardware: `(gdb) load` from inside an `avr-gdb` session reflashes the target and resumes at the reset vector cleanly. `(gdb) break <func>` × 5 (more than two simultaneous breakpoints) all hit independently. `(gdb) watch <var>` halts on the next write — GDB transparently uses software watchpoints (single-step + memory poll) because UPDI silicon exposes no data-watchpoint hardware (HLR-056). `monitor reset`, `monitor halt`, `monitor version` all behave as documented. An `avr-gdb` session targeting `avrOSdb` exercises the same workflow (attach → load → run → break → watch → monitor → detach) that an `avarice`-based session does, even if the literal `.gdbinit` text differs.

**Out of scope (explicit, deferred to a later phase):**
- Non-stop / asynchronous-execution mode (`vCont` already supports the synchronous subset).
- TLS or authenticated GDB transport.
- Tracepoints (`QTDP`, `QTStart`, `qTBuffer`).
- Reverse execution (`bs`, `bc`).
- Multi-target debugging of more than one physical AVR per server instance.

**Risks.**
- **SW-breakpoint save/restore correctness across NVMPROG.** Entering NVMPROG resets the CPU; if register snapshot/restore is incomplete the user's execution state is silently corrupted. Mitigation: cover R0–R31, SREG, SP, and PC in the snapshot, and add a Phase-10 regression test that single-steps across a SW-BP install/remove and asserts every GPR is unchanged.
- **FLASH wear from `Z0` storms.** Some IDEs install and remove the same breakpoint on every step. Mitigation: cache the shadow entry across remove → insert at the same address, performing the FLASH write only on the first install. Document the wear consideration in the user manual.
- **`vRun` semantics on a single-target stub.** GDB's `vRun` expects a fresh process; we re-parse the ELF in place. If the operator passes a fundamentally different ELF (e.g. wrong AVR family) the post-`vRun` register frame will be incoherent. Mitigation: `vRun` shall validate the ELF's e_machine and the per-family family code before accepting the swap, replying `E22` on mismatch.

---

### Phase 11 — RSP Capability Honesty & `multiprocess+` Correctness

**Motivation.** A live-wire packet trace of `avr-gdb` (VS Code `cppdbg`) attaching to `avrOSdb` after Phase 10 (`/tmp/rsp.transcript`, captured 2026-05-20) uncovered several places where the server's advertised capabilities and its actual packet handling disagree, plus several quality-of-life gaps. Most importantly, the server advertises `multiprocess+` in its `qSupported` reply but rejects every `Hgp<PID>.<TID>` / `Hcp<PID>.<TID>` and every `qThreadExtraInfo,p<PID>.<TID>` packet with `E01`. The visible consequence in VS Code is that the avrOS-FSM-as-GDB-thread feature — the headline product differentiator documented in `doc/UserManual.md` §5.6 — silently fails: the Call Stack view shows ten numbered threads with no labels, no state names, and no working thread switch. Several other smaller issues (missing `swbreak:` / `hwbreak:` stop tags, missing `qXfer:memory-map:read+`, no range-step support, `vKill` killing the server itself, silent shutdown) compound to make the stub feel less polished than its protocol surface implies. Phase 11 closes this honesty gap. Tracked as GitHub issue [#34](https://github.com/racerxr650r/avrOS-debug/issues/34); bound to **HLR-060 … HLR-066** in `doc/Project.xml` §12 (to be authored via the `tracer` skill before any source change lands).

**Architectural constraint.** The design and implementation of every Phase 11 work item shall meet the **Layered Architecture** requirement defined in `doc/SDD.md` §2.2 and `doc/PVD.md` §6: source modules remain organised into the four protocol layers (entry/event-loop, transport/hardware (UPDI), protocol (GDB RSP), and application (ELF/FSM/monitor)) with strictly downward call direction; no lower-layer header shall `#include` a higher-layer header, and no lower-layer function shall call a higher-layer function. The single permitted upward path remains dependency-inversion via the `RspHandlers` callback table in `src/gdb_rsp.h`. Concretely for Phase 11: the multiprocess parser, stop-cause classifier, memory-map XML emitter, range-step driver, and lifecycle logger all live in the layer that owns their concern (RSP-layer for parsing/XML/logging, UPDI-layer for stop-cause and range-step primitives, application-layer for FSM-label formatting). Any work item that appears to require a layer-crossing call shall instead be implemented by extending the `RspHandlers` table — never by upward `#include` or by reaching across modules. Compliance is verified by the `.h` include-graph check in `tools/lint_project.py` and by code review per `doc/SDP.md` §5.2.

**Runtime constraint — single shared stack.** avrOS is a **cooperative, run-to-completion FSM scheduler**: every FSM executes on the **same hardware stack** — the one set up by the C-runtime init code (`__stack` symbol from the linker map, growing down from `RAMEND`). There is no per-FSM stack, no context-switch save area, and no preempted register frame stored anywhere in SRAM. At any halt, **only the currently-running FSM has a live call stack**; the other N−1 FSMs are quiescent between dispatches and have no meaningful frame to unwind. This shapes the entire multiprocess / `qThreadExtraInfo` design and is a hard correctness constraint:
  - The `g`-packet for the *running* TID returns the real OCD register file (R0–R31, SREG, SP, PC) verbatim. The `g`-packet for every *non-running* TID shall return a synthesised frame whose **PC = the FSM's `state` function pointer** (next dispatch entry), **SP = the live shared SP**, **R0–R31 = 0x00** (no preserved context exists), and **SREG = 0x00**. This matches what the user actually sees on the next dispatch and avoids the invariant-violating alternative of fabricating a per-FSM register frame.
  - The `m`-packet (memory read) is **always served from live silicon** at the requested address — it shall *not* be filtered or rewritten per selected thread. Stack-walking by GDB will therefore read the same shared stack regardless of which TID is selected; the selected TID only changes which `g`-packet frame seeds the unwind, not the memory it walks through. **Backtraces of non-running FSMs are intentionally shallow** (one frame: the FSM `state` function). This is documented behaviour, not a bug, and shall be called out in the `qThreadExtraInfo` label as a `(quiescent)` suffix and in `doc/UserManual.md` §5.6.
  - `Hgp<PID>.<TID>` for a non-running TID **shall not** synthesise a fake SP from any per-FSM control block (there is no such SP to read — the FSM was last entered and exited via the shared stack and any stack frame it once had has been unwound). Implementations shall not invent pointers into the shared stack on behalf of quiescent FSMs.
  - The stop-cause classifier (HLR-062) reports the cause only against the **running** TID. Quiescent FSMs are never the proximate cause of a halt and shall never appear as the `thread:` of a `T05swbreak:` / `T05hwbreak:` reply.
  - **Acceptance test for `tests/test_fsm.c`:** `fsm_mapper_synth_frame(quiescent_tid)` returns PC = `fsm.state`, SP = live SP, all GPRs and SREG zero; the running TID's frame is byte-for-byte equal to the OCD register-file dump; no path in `gdb_rsp.c` or `fsm_mapper.c` ever reads from or writes to a fabricated per-FSM stack region.

**Scope summary.**

| # | Area | GDB packets / behaviour | Source file(s) | HLR |
| - | ---- | ----------------------- | -------------- | --- |
| 1 | Multiprocess thread-ID parsing | `Hgp<PID>.<TID>`, `Hcp<PID>.<TID>`, `T<pPID.TID>`, accept `p0.0`/`p-1.-1` as "any" | `gdb_rsp.c`, `fsm_mapper.c` | HLR-060 |
| 2 | FSM thread labels | `qThreadExtraInfo,p<PID>.<TID>` returns hex-encoded `"FSM <name> (state=<sym>)"` | `monitor.c` (or new `fsm_extra.c`), `fsm_mapper.c` | HLR-061 |
| 3 | Stop-reason tags | `qSupported` reply gains `swbreak+;hwbreak+`; halt replies become `T05swbreak:;thread:pPID.TID;` / `T05hwbreak:;…;` | `gdb_rsp.c`, `updi.c` (stop-cause classifier) | HLR-062 |
| 4 | Memory-map advertisement | `qXfer:memory-map:read+`; static XML built from the runtime-selected device-table entry | `gdb_rsp.c`, `updi.c` (device-table accessor) | HLR-063 |
| 5 | `vKill` listener survival | `vKill;<pid>` halts target, closes client socket, **returns to `accept()` loop** (does not `exit(0)`) | `main.c`, `gdb_rsp.c` | HLR-064 |
| 6 | Lifecycle logging | One stderr line on each: client-connect, client-disconnect (with reason: `D`/`vKill`/EOF/error), graceful server exit, fatal error | `gdb_rsp.c`, `main.c` | HLR-065 |
| 7 | Range-step | Advertise `vCont;c;C;s;S;r;t`; implement `vCont;r<start>,<end>:<tid>` as repeated OCD single-step bounded by the half-open range | `gdb_rsp.c`, `updi.c` | HLR-066 |
| 8 | Soft quality fixes | Accept `Hg p0.0` / `Hc p0.0` without `E01`; cache `qfThreadInfo` result between halts; cosmetic `qC` reply tied to selected thread | `gdb_rsp.c` | rolls into HLR-060 / HLR-061 |
| 9 | VS Code `launch.json` recommendation | Research and publish a recommended `cppdbg` `launch.json` block that exercises Phase 11 features end-to-end (FSM threads visible in Call Stack, memory-map honoured, range-step active) | `doc/UserManual.md`, sample under `doc/reference/launch.json` | HLR-067 |

1. **`src/gdb_rsp.c` — multiprocess thread-ID parsing (HLR-060).** Replace the current scalar `parse_thread_id()` with `parse_mp_thread_id(const char *s, uint32_t *pid_out, uint32_t *tid_out, bool *any_out)` that recognises:
   - bare decimal `0` or `-1`  → `any = true`
   - bare hex `<TID>` (legacy)  → `pid = g_pid`, `tid = …`
   - multiprocess `p<PID>.<TID>` with `p0.0`, `p-1.-1`, `p<PID>.0`, `p<PID>.-1` all mapped to `any = true` (i.e. "any thread in that process")
   Update `dh_h_packet()` (handles `Hg`/`Hc`/`Hs`), `dh_t_alive()` (`T<tid>`), the `?` / `c` / `s` stop replies, `vCont` action targets, and `vAttach;<pid>` / `vKill;<pid>` to use the new parser. The server's process ID shall be a fixed compile-time constant exposed as `RSP_PID` (currently observed by GDB as `0xa410`) — formalise this and stop reading the value from uninitialised stack. **Acceptance test:** an `Hgp<RSP_PID>.<TID>` for every TID in `FsmContext.threads[]` returns `OK`, and every subsequent `g` returns the per-thread frame built by `fsm_mapper`.

2. **`src/monitor.c` (or new `src/fsm_extra.c`) — `qThreadExtraInfo` FSM labels (HLR-061).** Implement a `qThreadExtraInfo,p<PID>.<TID>` handler that resolves `TID` through `fsm_mapper_thread_to_fsm()`, formats `"FSM <fsm_name> (state=<state_sym>) ticks=<n>"` (≤ 64 bytes), and replies with the ASCII bytes hex-encoded per the RSP spec. TID 1 (the CPU thread) shall return `"CPU"`. Unknown TIDs return the empty packet (not `E01`). Add a corresponding entry to the RSP dispatch table.

3. **`src/gdb_rsp.c` — stop-reason tags (HLR-062).** Extend `qSupported` reply with `swbreak+;hwbreak+`. Add `RspContext.last_stop_cause` of type `enum { SC_NONE, SC_SWBREAK, SC_HWBREAK, SC_STEP, SC_INTR, SC_VFLASH }` set by the OCD post-halt classifier in `updi_wait_halt()` (BREAK opcode at PC ⇒ SC_SWBREAK; HW comparator match ⇒ SC_HWBREAK; single-step counter exhausted ⇒ SC_STEP; STOP issued by host ⇒ SC_INTR). `format_stop_reply()` emits `T05swbreak:;thread:pPID.TID;` / `T05hwbreak:;…;` accordingly; SC_INTR maps to `T02` (SIGINT). **Acceptance test:** a SW-BP hit produces `T05swbreak:;thread:p<PID>.1;`; a `hbreak` hit produces `T05hwbreak:;…;`.

4. **`src/gdb_rsp.c` — memory-map advertisement (HLR-063).** Add `qXfer:memory-map:read+` to the `qSupported` reply and implement the `qXfer:memory-map:read::<offset>,<length>` handler. The XML body is generated once at server start from the active device-table entry (Phase 7 / HLR-048) and pinned in a static buffer:
   ```xml
   <memory-map>
     <memory type="flash"  start="0x000000" length="N"><property name="blocksize">512</property></memory>
     <memory type="ram"    start="0x800000" length="M"/>
     <memory type="rom"    start="0x810080" length="0x80"/>   <!-- USERROW -->
     <memory type="rom"    start="0x811080" length="0x80"/>   <!-- SIGROW  -->
     <memory type="ram"    start="0x814000" length="0x400"/>  <!-- EEPROM  -->
     <memory type="rom"    start="0x820000" length="0x20"/>   <!-- FUSES   -->
     <memory type="rom"    start="0x820040" length="0x04"/>   <!-- LOCK    -->
   </memory-map>
   ```
   The standard `qXfer` chunking protocol (`m<data>` / `l<data>`) shall be honoured with the existing `PacketSize=800` budget.

5. **`src/main.c` + `src/gdb_rsp.c` — `vKill` listener survival (HLR-064).** `dh_v_kill()` shall: (a) `updi_halt()` the target, (b) clear all SW-BP shadows and release both HW comparators, (c) reply `OK`, (d) close the client `fd`, (e) **return** to the `select()` loop instead of calling `quit_main_loop()`. The server shall continue to listen on the configured TCP port. The `--prog` mode (HLR / Phase 8) is unaffected: it never opens a listener. Document the new lifecycle in `doc/UserManual.md` §5.6 step 11. **Acceptance test:** an integration test connects, sends `vKill;<pid>`, observes `OK`, the socket close, and then reconnects successfully and runs `vAttach` without restarting the server.

6. **`src/gdb_rsp.c` + `src/main.c` — lifecycle logging (HLR-065).** Add one-line stderr emissions at each lifecycle transition:
   - `avrOSdb: listening on :%u\n` (server ready, after `--load` completes)
   - `avrOSdb: client connected from %s:%u\n` (after `accept()`)
   - `avrOSdb: client disconnected (%s)\n` where reason is one of `D`, `vKill`, `EOF`, or `read error: <errno-text>`
   - `avrOSdb: shutting down (%s)\n` where reason is `SIGINT`, `SIGTERM`, or `fatal: <text>`
   No new flag is added; emissions go to stderr at always-on level. Replace the current silent post-`verify: OK` quiescence with the explicit `listening` line.

7. **`src/gdb_rsp.c` + `src/updi.c` — `vCont;r` range-step (HLR-066).** Replace the current `vCont?` reply (`vCont;c;s`) with `vCont;c;C;s;S;r;t`. Implement `vCont;r<start>,<end>:<tid>` by issuing OCD single-steps in a tight loop while `start ≤ PC < end`, with an upper bound of `N_RANGE_STEP_MAX` steps (e.g. 4096) to prevent runaway. `vCont;t` (stop) maps to `updi_halt()`. `vCont;C`/`S` accept-then-ignore the signal byte (AVR has no Unix signals to deliver), preserving the targeted execution semantics.

8. **CLI surface.** No new flags. Phase 11 is purely protocol-side.

8a. **VS Code `launch.json` recommendation (HLR-067).** Research and publish a known-good `cppdbg` configuration that exercises every Phase 11 feature end-to-end against `avrOSdb`, replacing the current minimal block in active use:
   ```json
   {
     "version": "0.2.0",
     "configurations": [
       {
         "name": "Debug AVR via avrOSdb",
         "type": "cppdbg",
         "request": "launch",
         "program": "${workspaceFolder}/build/firmware.elf",
         "miDebuggerPath": "/usr/bin/avr-gdb",
         "miDebuggerServerAddress": "localhost:1234",
         "cwd": "${workspaceFolder}",
         "MIMode": "gdb",
         "externalConsole": false,
         "setupCommands": [
           { "text": "set architecture avr" },
           { "text": "set print pretty on" }
         ]
       }
     ]
   }
   ```
   Items to investigate, justify, and either include or explicitly reject in the recommended block:
   - `"request"`: `"launch"` vs `"attach"`. `launch` re-runs the target on every F5 (re-issues `vRun` if the server advertised it, else resets via `monitor reset`); `attach` connects without disturbing the running target. Decide which is the better default for Phase-11 avrOSdb (the server is long-lived after HLR-064, so `attach` may now be more honest); document both with a "when to use which" guide.
   - `"setupCommands"`: add `{"text": "set remotetimeout 30"}` (UPDI flash erase can exceed the default 2 s); add `{"text": "set mem inaccessible-by-default off"}` once HLR-063 ships the memory-map (then GDB will know I/O regions are valid); add `{"text": "set non-stop off"}` (Phase 11 is stop-mode only — see Out-of-Scope).
   - `"customLaunchSetupCommands"` vs `"setupCommands"`: `launch` configs need the former when the server already manages flashing (suppresses `cppdbg` issuing its own `load`); document the distinction.
   - `"stopAtEntry"` / `"stopAtConnect"`: `stopAtConnect: true` ensures the IDE pauses at the reset vector after attach so the user can set breakpoints before `continue`. Recommend on by default.
   - `"logging"`: `{"engineLogging": true, "trace": true, "traceResponse": true}` for the user-facing troubleshooting recipe (currently absent from the manual).
   - `"preLaunchTask"`: optional `tasks.json` entry that builds the ELF and starts `avrOSdb` if not already running; document but mark optional.
   - `"miDebuggerArgs"`: investigate whether passing `--nx` is needed to suppress per-user `.gdbinit` interference on shared dev hosts.
   - `"targetArchitecture"`: VS Code-specific hint; verify whether it adds value beyond `set architecture avr` in `setupCommands`.
   - `"avoidWindowsConsoleRedirection"`, `"externalConsole"`: confirm the headless-Linux default and whether any change is needed for WSL/macOS hosts.
   - **Phase-11-specific verification:** with the recommended block, confirm that VS Code's Call Stack view shows all FSMs with their `qThreadExtraInfo` labels, that the Memory view obeys the advertised memory-map regions, and that source-level `next` uses range-step (engine log shows `vCont;r`).
   - **Deliverable:** a fully-commented `doc/reference/launch.json` sample, a copy-pasteable block in `doc/UserManual.md` §6, and a one-paragraph rationale per non-default setting. The result must work unchanged against an unmodified VS Code + `cppdbg` install on Linux; macOS / Windows-host caveats documented separately.

9. **Tests — `tests/test_rsp.c`.** Add cases for: (a) `parse_mp_thread_id()` accepting every bare-decimal, bare-hex, and `p<PID>.<TID>` form including `p0.0` / `p-1.-1`; (b) `Hgp<PID>.<TID>` selecting the right thread and the subsequent `g` returning the matching synthesised frame; (c) `qThreadExtraInfo` returning a hex-encoded label for every known TID and the empty packet for unknown TIDs; (d) `qSupported` reply containing `swbreak+;hwbreak+;qXfer:memory-map:read+`; (e) `qXfer:memory-map:read::0,800` returning a well-formed `<memory-map>` body; (f) a simulated SW-BP halt emitting `T05swbreak:;thread:p<PID>.1;`; (g) `vKill;<pid>` closing the client socket but leaving the listener live (assert the listener `fd` is still in the `select()` set); (h) `vCont?` advertising `r` and a `vCont;r<lo>,<hi>:p<PID>.1` driving repeated single-steps until PC leaves the range.

10. **Tests — `tests/test_monitor.c`.** Add a case that the new `qThreadExtraInfo` path renders the active FSM `state=` symbol name correctly when the state pointer points at a function with debug info, and renders `state=0x????` (raw hex) when symbol resolution fails.

11. **Tests — `tests/test_fsm.c`.** Add a case asserting that `fsm_mapper_thread_to_fsm()` is keyed by TID alone (the PID component is informational), so future PID changes do not silently regress.

12. **Tests — `tests/test_main.c`.** Add a case asserting the lifecycle log lines from item 6 are emitted in order on a connect → `vKill` → reconnect sequence.

13. **Documentation updates.**
    - `doc/UserManual.md` §5.6 — step 11 rewritten to reflect that `vKill` no longer exits the server; the troubleshooting table gains "Server exits immediately after VS Code stop" → "fixed in Phase 11; before that, restart `avrOSdb` after every `kill`". The `info threads` example output is verified against an actual capture and updated if the FSM-label format changed.
    - `doc/avrOSdb.1` — `qSupported` capability list updated under `PROTOCOL`; `vCont` action list updated.
    - `doc/SDD.md` §5 (gdb_rsp.c) — new handler entries in the dispatch table; the multiprocess parser and the `qThreadExtraInfo` handler get §5.x subsections.
    - `doc/Project.xml` §12 — author HLR-060 … HLR-066 via the `tracer` skill, link each to its LLRs and to the new tests above.
    - `doc/SDP.md` — flip the Phase 11 row in the Status table from "🔲 Not started" to "🚧 In progress" / "✅ Complete" as work lands.

**Acceptance:**
- `make test` runs all suites including the extended `test_rsp`, `test_monitor`, `test_fsm`, `test_main`; new tests pass.
- `python3 tools/lint_project.py` reports 0 errors, 0 warnings after every HLR-060 … HLR-066 is traced from at least one new LLR and one new test.
- Re-running the 2026-05-20 packet-trace experiment (avrOSdb under socat, VS Code `cppdbg` attached) yields:
  - zero `E01` replies to any `Hg`, `Hc`, or `qThreadExtraInfo` packet;
  - VS Code's Call Stack view shows each FSM with its name and current state symbol;
  - the server's stderr emits one `listening`, one `client connected`, one `client disconnected (vKill)` line per session and stays alive across `vKill`;
  - stop replies for SW-BP hits carry `swbreak:;` and for `hbreak` hits carry `hwbreak:;`;
  - `(gdb) info mem` shows the device's FLASH / SRAM / EEPROM regions sourced from the server's memory-map XML;
  - source-level `next` over a tight loop runs measurably faster than against Phase 10 (range-step in action).

**Out of scope (explicit, deferred to a later phase):**
- Non-stop / asynchronous-execution mode (still requires substantial reentrancy work in the UPDI layer).
- `QXfer:features:read+` (target-description XML) — the AVR built-in description in GDB is adequate for AVR-Dx; revisit when a non-stock register set is exposed.
- `QPassSignals+` / `QProgramSignals+` / `ConditionalBreakpoints+` — no compelling user demand on a bare-metal target.
- `QNonStop+` — deferred until the UPDI run-control state machine supports asynchronous stop replies.
- Multi-client GDB support (still a Phase-0 non-goal).

**Risks.**
- **Backward compatibility of stop replies.** Older `avr-gdb` builds (≤ 10) may not understand `swbreak:`/`hwbreak:` tags. Mitigation: only emit the tags when the client advertised the matching `swbreak+`/`hwbreak+` in its own `qSupported` (the spec requires this gating anyway); fall back to bare `T05thread:…;` otherwise.
- **Memory-map XML drift vs. real silicon.** The XML is generated from the device table; if the table is wrong (e.g. EEPROM size for a part variant), GDB will refuse legitimate memory accesses with "memory not available". Mitigation: cover every device-table entry with a `tests/test_updi.c` case that round-trips the generated XML through a strict parser; fail CI on any drift.
- **Range-step runaway.** A pathologically tight `vCont;r` with a bad upper bound would step the target indefinitely. Mitigation: hard cap at `N_RANGE_STEP_MAX` steps with a defensive `T05` halt-and-report if the cap is hit; expose the count in a new `monitor diag last-range-step` verb.
- **`vKill` listener survival vs. resource leaks.** Keeping the server alive across many connect / disconnect cycles risks leaking SW-BP shadow entries, file descriptors, or NVMPROG state. Mitigation: a dedicated `rsp_session_reset()` called from `dh_v_kill()` zeroes every per-session structure; a Phase-11 stress test connects-disconnects 100× and asserts the process RSS and open-fd count are unchanged.

### Phase 12 — Demote FSM Threading to Introspection + Breakpoint Regression Net

> **Status: ✅ Complete — merged via PR [#43](https://github.com/racerxr650r/avrOS-debug/pull/43) (merge commit `b6e391f`) into `develop`, 2026-06-03 (issue #42).** All four increments shipped: (1) spec/design restructure (SDD + HLRs/LLRs/STP/Traceability), (2) GDB-thread-surface removal (`gdb_rsp.c`/`fsm_mapper.c`/`main.c`), (3) introspection (`monitor avros tasks` + the flash-LMA read fix for the `0xFF`-name bug), (4) captured-session replay regression net (`tests/fixtures/rsp/break_hit_bt.log` + host replay test) and `hw-test-gdb` G14. `make`: 0 warnings; `make test`: all suites pass; `lint_project`: 0 errors / 0 warnings. Hardware-verified on AVR128DA28 (`/dev/ttyAMA2`): `bt` returns a clean single-thread backtrace (no avr-gdb crash) and `monitor avros tasks` lists the FSMs. Supersedes the FSM-as-GDB-thread design of Phase 11.

**Motivation.** A live-wire `--log-rsp` capture pair taken 2026-06-02 (`logs/avrOSdb-rsp.log` with FSM threads on vs `logs/avrOSdb-rsp-no-fsm.log` with `--no-fsm-threads`), together with a 63 MB `avr-gdb` **core dump** from the same session, pins the root cause of the long-running breakpoint "whack-a-mole": the breakpoints themselves work (the wire shows a clean `c → T05swbreak:;thread:1;` and the BREAK opcode patched into flash), but as soon as the front-end runs `bt` with FSM threads enabled, **avr-gdb itself crashes**. Every FSM pseudo-thread is presented to GDB with an all-zero register frame (`g → 0…0`, PC = 0) and a garbage name (`qThreadExtraInfo` returns `FSM` + 31 bytes of `0xFF` + ` [quiescent] state=0x0000`); GDB's unwinder then walks a PC = 0 frame into garbage and segfaults. `--no-fsm-threads` only hides the symptom.

This is not a fixable detail — it is a **model mismatch**. avrOS is a cooperative, run-to-completion FSM scheduler on a **single shared stack** (see Phase 11, "Runtime constraint"): a quiescent FSM has *no suspended call stack to unwind*. Presenting it as a GDB thread hands the front-end an object it will inevitably try to backtrace and read frame-locals from, with nothing real underneath. Even fully fixed (PC = state-function pointer), `bt` would unwind that function against an SP/frame that was never its call frame — producing a plausible-but-fictional backtrace, which is arguably worse than a crash for a debugger. The value of the feature (which task is active, each task's state, queues/events) is real but does not require GDB's thread/unwind machinery to deliver. **Decision (2026-06-03): demote FSM awareness from GDB virtual threads to first-class introspection surfaced through `monitor` commands.** This eliminates the entire GDB-unwinder crash class structurally and removes the breakpoint↔FSM-thread state coupling that drove the regressions.

**Architectural constraint.** Same Layered Architecture rule as Phase 11 (`doc/SDD.md` §2.2, `doc/PVD.md` §6): strictly downward calls, no upward `#include`, the `RspHandlers` table the only inversion path. FSM-state formatting remains in the application layer; the RSP layer loses all per-thread frame synthesis.

**Requirements impact (TraceR).**
- **Retire** HLR-024 (FSM Thread Enumeration), HLR-025 (Active Thread Identification), HLR-026 (Virtual Thread Register Frame), HLR-027 (Complete FSM Thread Coverage), HLR-028 (Stack-Free Thread Model), and HLR-061 (`qThreadExtraInfo` FSM label). Their downstream LLRs and tests are removed or repointed; Traceability must show no orphaned references.
- **Retire** HLR-060 (Multiprocess Thread-ID Parsing) and **revise** HLR-058 (RSP capability honesty) to **drop `multiprocess+`** from the advertised `qSupported` set. With multiprocess off, GDB uses bare thread-id forms (`Hg0`, `Hc-1`, `D`) and the server recognises exactly one valid thread (TID 1, the live CPU).
- **Add** a new HLR for **avrOS Introspection via `monitor`** (active task, per-task state, registration-table dump) and a new HLR for the **host-side RSP sequence-replay test harness** (below).

**Scope summary.**

| # | Area | Behaviour | Source file(s) | Requirement |
| - | ---- | --------- | -------------- | ----------- |
| 1 | Remove GDB-thread mapping | drop `multiprocess+` from `qSupported`; `qfThreadInfo → l` (single implicit thread — the proven `--no-fsm-threads` wire); drop the `qThreadExtraInfo` FSM path; `g` always the live OCD frame; delete per-thread synthetic frames | `gdb_rsp.c` | retire HLR-024..028/060/061; revise HLR-058 |
| 2 | FSM introspection provider | Repurpose `fsm_mapper` to produce a task report (name, active/quiescent, current-state symbol) — **not** GDB frames | `fsm_mapper.c/.h` | new HLR (introspection) |
| 3 | Fix FSM data reads (sub-bugs) | Correct the SRAM/flash reads so task **names are real ASCII** (not `0xFF`) and **state pointers are populated** (not 0) | `fsm_mapper.c`, `elf_parser.c` | new HLR (introspection) |
| 4 | `monitor` surface | `monitor avros tasks` lists all FSMs with active marker + state; integrate with existing `avros events`/`queues` | `monitor.c` | new HLR (introspection) |
| 5 | CLI cleanup | Remove `--no-fsm-threads`; add `--no-introspect` to suppress the introspection snapshot; remove the `enable_fsm_threads` thread plumbing | `main.c`, `gdb_rsp.c` | revise HLR-058 |
| 6 | Stringent hw-test + capture | Harden `hw-test-gdb` Group-G into the authoritative on-target gate (break→hit→`bt`→step→threads→`monitor avros tasks`→detach); capture each scenario's `--log-rsp` transcript to `tests/fixtures/rsp/` | `tests/hw/gdb_acceptance.py` | new HLR (test infra) |
| 7 | Host replay net (from captures) | Fake target replays the captured transcripts in CI — no hardware; new coverage added by re-running hw-test and committing the log | `tests/test_rsp_seq.c` (new) | new HLR (test infra) |
| 7 | Docs / PVD | Reframe the differentiator from "FSM threads in the Call Stack" to "avrOS introspection"; update `doc/UserManual.md` §5.6 and `doc/PVD.md` | `doc/*` | — |

1. **`src/gdb_rsp.c` + `src/main.c` — remove the GDB-thread mapping (retire HLR-024..028/060/061; revise HLR-058).** Drop `multiprocess+` from the `qSupported` reply so GDB uses bare thread-id forms (`Hg0`, `Hc-1`, `D`). `qfThreadInfo` replies `l` (single implicit thread — the exact wire the working `--no-fsm-threads` session used). Remove the FSM enumeration loop, the `qThreadExtraInfo` FSM-label handler, `select_stop_thread()` FSM rebinding, `refresh_fsm_threads_if_needed()`, the per-thread branch in `dh_read_regs()` (always serve the live OCD register file), and `parse_mp_thread_id()` in favour of bare-TID parsing where only TID 1 is valid. Stop replies remain `T05…thread:1;`. Remove `--no-fsm-threads` and the `enable_fsm_threads` plumbing; add `--no-introspect`. **Acceptance test (`tests/test_rsp.c`):** the `qSupported` reply contains no `multiprocess+`; `qfThreadInfo → l`; `g` always returns the OCD frame; no code path reads `FsmContext.threads[]` to build a register frame.

2. **`src/fsm_mapper.c/.h` — introspection provider, not thread source.** Replace `fsm_build_thread_list()`/`fsm_get_registers()` with `fsm_snapshot(FsmContext*, const AvrOsSymbolIndex*, int updi_fd)` producing a table of `{name, is_active, state_addr, state_sym}` for rendering only. No GDB ids, no register buffers.

3. **Fix the FSM data reads — the two sub-bugs (new introspection HLR).** Diagnose why task names read back as `0xFF` and state pointers as `0` (wrong base, missing GDB↔UPDI bit-23 translation, or struct-offset drift), and correct it so a snapshot on real hardware yields real ASCII names and non-zero state symbols. This is the substantive bug-fix the demote enables us to verify cheaply via `monitor`.

4. **`src/monitor.c` — `monitor avros tasks` (new introspection HLR).** Render the snapshot: one line per FSM with an active marker, name, and resolved current-state symbol; reuse the ELF symbol index for symbolization. Keep it non-intrusive (UPDI reads only).

5. **`tests/hw/gdb_acceptance.py` — stringent hw-test + RSP-log capture (the authoritative acceptance, and the fixture source).** Testing strategy for this phase and going forward: **`hw-test-gdb` is the project's primary correctness gate** for breakpoint/thread/stepping behaviour against real silicon, and it is the *source* of the host-side unit fixtures — not hand-authored transcripts. Harden the Group-G scenarios into a rigorous sequence: with avrOS firmware loaded, `monitor reset` → set breakpoint → `continue` → hit → **`bt`** → `stepi`/`step` → list threads → `monitor avros tasks` → `detach`, asserting a clean single-thread backtrace, real ASCII task names, correct active task, and no crash. Each scenario runs `avrOSdb --log-rsp` (or `AVROSDB_GDB_LOG_RSP=1`) and **captures the full RSP wire transcript** to `tests/fixtures/rsp/<scenario>.log`. These captures are the canonical record of real front-end behaviour (avr-gdb `-batch`, and cortex-debug via `showDevDebugOutput: raw`).

6. **`tests/test_rsp_seq.c` — host-side replay unit tests *generated from* the hw-test captures [step B of the diagnosis plan].** A programmable fake target (models flash words, PC, the two OCD comparators, and BREAK-on-execute) replays the RSP transcripts captured by step 5 (committed under `tests/fixtures/rsp/`) and asserts the server's replies match — turning each real hardware session into a hardware-free CI regression test. The headline fixture is the demoted equivalent of the 2026-06-02 crashing session (`qfThreadInfo → l`, single-thread backtrace, no degenerate frames, no all-`0xFF` label). Wired into `make test` so CI catches any regression of this class without hardware. The workflow for new coverage is: re-run the stringent `hw-test-gdb`, commit the emitted `<scenario>.log`, and add a one-line replay assertion — so hardware reality continuously seeds the host test suite.

7. **Docs / PVD.** Reframe the headline differentiator and rewrite `doc/UserManual.md` §5.6 around `monitor avros tasks`; note in `doc/PVD.md` that avrOS awareness is delivered via introspection, not GDB threads. Regenerate all five spec docs via the `tracer` skill.

**Acceptance.**
- **Stringent hw-test passes (hardware, `hw-test-gdb`):** the hardened Group-G sequence (avrOS firmware: `monitor reset` → break → hit → `bt` → step → threads → `monitor avros tasks` → detach) runs green — clean single-thread backtrace, **no avr-gdb crash**, clean detach — and **emits its RSP capture** to `tests/fixtures/rsp/`. (This is the scenario that produced the core dump.)
- **Introspection truthful (hardware):** `monitor avros tasks` lists real ASCII task names with the correct active task marked and non-zero current-state symbols — no `0xFF`, no all-zero.
- **CI net (host, `make test`):** `test_rsp_seq` replays the captured transcripts green; `qfThreadInfo` returns only TID 1; no FSM register-frame path remains.
- **Gate:** full unit suite passes, `make` builds with 0 warnings, `python3 tools/lint_project.py` reports 0 errors / 0 warnings, and `doc/Traceability.md` shows the retired HLRs removed with no orphaned trace references.

### Phase 13 — HW-Comparator Arbiter with a Reserved Single-Step Slot

> **Status: 🚧 Planned.** Tracked as GitHub issue [#45](https://github.com/racerxr650r/avrOS-debug/issues/45); own feature branch per §5.1. Requirement changes authored in `doc/Project.xml` via the `tracer` skill before source changes land.

**Motivation.** The two AVR-Dx OCD program-counter comparators (`OCD_BP0A`/`OCD_BP1A`, exposed as `RspContext.hw_bp_addr[2]`) are allocated ad-hoc — the last open structural cause of the long-running breakpoint "whack-a-mole" (the FSM-thread `bt` *crash* was resolved in Phase 12). Today `dh_insert_bp` (Z1/`hbreak`, `bp-mode hw-only`, and GDB's automatic HW breakpoints for flash / read-only addresses — observed as `Z1` in the Phase-12 capture) grabs the **first free slot**, while single-stepping over a 32-bit `LDS`/`STS` hardcodes **slot 1** (`updi_step_32bit(fd, 1, …)`) and clears it **without save/restore**. Consequences: (a) if a HW breakpoint occupies slot 1, a `stepi` over an `LDS`/`STS` silently disarms it in silicon while the shadow `hw_bp_addr[1]` still reports it armed (desync); (b) if both slots hold user HW breakpoints, stepping over an `LDS`/`STS` has no slot at all. Direct 32-bit `CALL`/`JMP` stepping no longer needs a slot (Phase-10/issue-#40 moved it to OCD emulation via `updi_ocd_emulate_cof_32bit()`), so the **only** remaining HW-BP-consuming step case is the 32-bit non-CoF `LDS`/`STS`.

**Design decision.** Introduce a **single comparator arbiter** that owns both slots and **permanently reserves slot 1 for the single-step-over workaround**. User/auto HW breakpoints may use **only slot 0**. Single-stepping over a 32-bit `LDS`/`STS` is then **always possible regardless of which user breakpoints are set** — the step slot is never contended. Cost: user-visible HW breakpoints drop from 2 to 1; SW breakpoints (FLASH `BREAK`, HLR-054) remain unlimited and are the default (`bp-mode sw`), and the recommended `set breakpoint auto-hw off` keeps flash breakpoints on the SW path, so normal debugging is unaffected.

**Architectural constraint.** Same Layered Architecture rule as Phases 11–12: the arbiter lives in the RSP layer (it owns `RspContext.hw_bp_addr[]`); UPDI-layer helpers (`updi_ocd_set_hw_bp`/`clear_hw_bp`, `updi_step_32bit`) take an explicit slot index and never choose one.

**Scope summary.**

| # | Area | Behaviour | Source file(s) |
| - | ---- | --------- | -------------- |
| 1 | Comparator arbiter | One owner of `hw_bp_addr[2]`: slot 0 = user HW-BP pool (capacity 1), slot 1 = reserved step slot. API: `hw_bp_user_insert/remove`, `hw_bp_step_acquire/release`, and a classifier that tells a step-slot stop from a user `hwbreak` | `gdb_rsp.c` |
| 2 | Z1/z1 insert/remove | User HW BPs allocate slot 0 only via the arbiter; `E08` when occupied (1 HW BP max) | `gdb_rsp.c` |
| 3 | `LDS`/`STS` step | `dh_step` + `updi_step_32bit` borrow the reserved step slot (1); no save/restore needed | `gdb_rsp.c`, `updi.c` |
| 4 | Stop classification / teardown | `classify_stop_cause`, `rsp_hw_bp_clear_all` route through the arbiter (a stop at the step slot is `SC_STEP`, not `SC_HWBREAK`) | `gdb_rsp.c` |
| 5 | Spec + docs | Revise the breakpoints HLR/LLRs for "1 user HW BP + 1 reserved step comparator". **User Manual:** capture the functional changes of **both Phase 12 and Phase 13** — replace all "FSM virtual thread / Call Stack / `info threads`" language with the introspection model (`monitor avros tasks`, single GDB thread, `--no-introspect`), and state the HW-BP count (1 user + 1 reserved step slot; SW breakpoints unlimited). The Phase-12 manual rewrite was deferred, so it is folded in here. | `doc/Project.xml`, `doc/UserManual.md` |
| 6 | Tests | Unit (2nd HW BP → `E08`; `stepi` over `LDS` with a user HW BP in slot 0 leaves it armed); host replay; stringent `hw-test-gdb` G-group | `tests/test_rsp.c`, `tests/hw/gdb_acceptance.py` |

**Plan (C → A → B).**

1. **(C) Confirm on the wire.** With the target connected, set two HW breakpoints (or one `hbreak` + GDB auto-HW), `stepi` over a 32-bit `LDS`/`STS`, and capture `--log-rsp` evidence that the slot-1 breakpoint is disarmed in silicon (desync) — the empirical pin, committed under `tests/fixtures/rsp/`.

2. **(A) Requirement restructure (tracer).** Revise HLR-016 (Breakpoints) and the relevant LLRs to specify exactly one user-allocatable HW comparator plus one reserved single-step comparator; document the `E08`-on-second-HW-BP contract and the "stepping always possible" guarantee. Render + lint to 0/0.

3. **(B) Implement + regression net.** Add the arbiter; route `dh_insert_bp`/`dh_remove_bp`/`dh_step`/`updi_step_32bit`/`classify_stop_cause`/`rsp_hw_bp_clear_all` through it; add the unit test, the host replay assertion, and a `hw-test-gdb` G-group (set `hbreak`, `stepi` over an `LDS`, assert the `hbreak` still fires).

**Acceptance.**
- **Unit:** a second HW-breakpoint request returns `E08`; `stepi` over a 32-bit `LDS`/`STS` with a user HW BP in slot 0 leaves that BP armed (shadow and silicon consistent).
- **Hardware (`hw-test-gdb`):** set an `hbreak`, `stepi` over an `LDS`/`STS`, and confirm the `hbreak` still fires afterward — no desync; stepping succeeds with the user HW BP present.
- **Gate:** full unit suite passes, `make` 0 warnings, `python3 tools/lint_project.py` 0 errors / 0 warnings.

## 9. Risks & Open Questions

*   **Half-duplex echo cancellation in UPDI tests.** Every byte transmitted over the UPDI UART is echoed back on the RX line by the hardware. PTY pairs do not auto-echo, so the PTY test harness must explicitly write back the echo bytes before injecting each simulated AVR response. If this is omitted, UPDI functions will block waiting to drain echoes that never arrive, causing PTY tests to time out even though the production logic is correct.

*   **macOS `<elf.h>` portability.** Linux glibc provides `<elf.h>`; macOS does not. The bundled `src/elf.h` shim must be guarded with `#ifdef __linux__ #include <elf.h> #else #include "elf.h" #endif` in `src/elf_parser.c`. Risk: if the `#ifdef` guard is accidentally omitted on a macOS build the compiler will fail with a missing-header error that may be non-obvious.

*   **avr-gcc availability for ELF fixtures.** The `tests/test_elf.c` test suite loads real `.elf` binaries generated by `avr-gcc`. If `avr-gcc` is not installed, the ELF fixture `make` rule will fail and block the entire test build. Mitigation: document the requirement prominently in §0, and consider adding a `make check-tools` target that validates availability before attempting a build.

*   **g-packet PC register encoding.** The GDB AVR register layout places the program counter at register index 35, encoded as a 4-byte little-endian value at hex positions 70–77 of the `g`-packet (78 hex chars total). An off-by-one in the buffer offset produces a silently malformed frame — `avr-gdb` will show a wrong PC without reporting an error. Mitigation: `tests/test_fsm.c` includes a dedicated assertion that verifies the exact byte positions independently of any PC value.

## 10. Estimated Effort

T-shirt sizes relative to Phase 0.

| Phase | Description | Effort |
| ----- | ----------- | ------ |
| 0 | Project Scaffolding | S — directory tree, Makefile, vendoring; no application logic |
| 1 | ELF Parser | M — well-understood binary format; fixture generation adds one-time avr-gcc complexity |
| 2 | UPDI Physical Layer | L — half-duplex echo cancellation, multi-opcode protocol, PTY harness with echo simulation |
| 3 | FSM Mapper | M — clean mock boundary via `--wrap`; most complexity is in fixture byte sequence design |
| 4 | Monitor + RSP | XL — 15 RSP handler table entries, 29 test cases, O-packet hex encoding, circular header dependency |
| 5 | Application Entry Point + Integration | L — `main.c` itself is thin; integration test harness (fork/PTY/TCP) is the dominant effort |
| 6 | Installation Targets + Documentation | S/M — Makefile targets are ~30 lines each; RPM spec and `.deb` control boilerplate add moderate complexity; Homebrew formula is straightforward Ruby; most effort is writing user manual and man page prose |
| 7 | Device-Signature Diagnostic Mode | S — one new UPDI helper, one new CLI flag, a family-name lookup table, a PTY-driven test file; touches only `main.c` and `updi.c`, no protocol changes |
| 8 | Non-FLASH NVM Programming | M — four new `updi_nvm_write_*` routines mirroring the existing FLASH path, a window-classifier dispatcher in `main.c`, one new CLI flag (`--allow-lock-updi`), and a multi-section ELF fixture; touches `updi.c`, `main.c`, and the test build only |
| 10 | GDB Protocol Completion (avarice Feature Parity) | L — six new HLRs spanning `vFlash*` flash-load handlers, a true SW-breakpoint path with FLASH save/restore across NVMPROG, empty-packet reply for data watchpoints (silicon does not expose the hardware — HLR-056), six new generic `monitor` verbs in `src/monitor.c`, extended-remote lifecycle packets (`vRun`/`vAttach`/`vKill`), and four small-protocol cleanup handlers; touches `gdb_rsp.c`, `monitor.c`, `updi.c`, and main wiring |

## 11. Out-of-Scope Follow-ups

*   **macOS CI (GitHub Actions).** A workflow that runs `make test` on `macos-latest` to catch portability regressions; depends on `brew install avr-gcc` being available on the hosted runner.
*   **ASAN/UBSan CI job.** A second CI job that repeats `make test` with `-fsanitize=address,undefined` to catch memory errors and undefined behaviour; already supported by `make ASAN=1` but not wired into CI.
*   **Live hardware integration test.** An end-to-end script that connects a real AVR DA/DB target via a USB-serial adapter, attaches `avr-gdb`, and verifies `info threads` output and `monitor avros events` decoding.
*   **Console bridge live testing.** `updi_console_poll()` reads the avrOS UART ring buffer; verifying this against a live target running actual UART output is deferred to post-implementation.
*   **Flash-load smoke test.** Exercising the full `--load` code path (ELF → NVM write → verify) on hardware is deferred; the NVM write logic is covered by unit tests but not by a write-then-read-back live test.
