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

**Status:** Phase 0 complete (commit `92857a5`). Phases 1–5 not yet started.

## Status

| Phase | Description | Status |
| ----- | ----------- | ------ |
| [0](#phase-0--project-scaffolding) | Directories, Unity test framework, Makefile, ELF portability shim | ✅ Complete (`92857a5`) |
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

`avr-updi-gdb` fills the gap between the AVR UPDI debug interface and standard GDB-based IDEs while adding first-class avrOS FSM task visibility. Without this stub, developers must choose between low-level UPDI tools with no source-level debugging, or generic GDB stubs that have no awareness of the avrOS cooperative task model. The result is that avrOS application developers cannot set breakpoints, inspect task state, or understand which FSM is running — the core debugging workflows that every RTOS user expects.

This implementation follows the complete specification stack authored in this repository (PVD → SDD → HLRs → LLRs → STP), which reached lint-clean status (0 errors, 0 warnings) before any source code was written. See [doc/PVD.md](PVD.md) for the full product vision.

## 2. Goals

1. Deliver a working `avr-updi-gdb` binary built from 6 C99 source modules (`main`, `updi`, `gdb_rsp`, `elf_parser`, `fsm_mapper`, `monitor`).
2. All 54 Low-Level Requirements fully implemented and verified by 106 passing tests across 7 test files.
3. Binary compiles without warnings under `-std=c99 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L`.
4. No heap allocation on the hot path; only `elf_open()` allocates (freed by `elf_close()` at session end).
5. Portable: builds and all tests pass on Linux (x86-64, ARM64) and macOS (Intel, Apple Silicon).
6. Runtime dependencies: only libc — verified by `ldd avr-updi-gdb` showing no libraries beyond libc.
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

Every commit runs: `make test` (builds and executes all unit test binaries) and `python3 tools/lint_project.py`. Both must exit 0. The integration test phase additionally runs `make` to produce the final `avr-updi-gdb` binary and verifies `ldd` output shows only libc.

### 5.4 Release Process

Source-only releases. Tag `vX.Y.Z` on `main` once all 106 tests pass and the binary builds clean. No prebuilt binaries are distributed; consumers build from source.

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
| `test_elf` | `src/elf_parser.c` | *(none)* | — |
| `test_updi` | `src/updi.c` | `select` | `-lutil` (Linux only; not needed on macOS) |
| `test_fsm` | `src/fsm_mapper.c` | `updi_mem_read` | — |
| `test_monitor` | `src/monitor.c`, `src/gdb_rsp.c`, `src/elf_parser.c` | `updi_mem_read` | — |
| `test_rsp` | `src/gdb_rsp.c`, `src/fsm_mapper.c`, `src/monitor.c` | `updi_mem_read`, `updi_halt`, `updi_run`, `updi_step`, `updi_nvm_write_flash`, `updi_console_poll`, `fsm_build_thread_list`, `fsm_get_registers`, `fsm_get_active_thread`, `fsm_invalidate`, `monitor_dispatch` | — |
| `test_main` | `src/main.c` | `updi_open`, `updi_close`, `updi_console_poll`, `rsp_listen`, `rsp_accept`, `rsp_close`, `rsp_recv_packet`, `rsp_dispatch`, `elf_open`, `elf_find_avros_tables`, `elf_close`, `fsm_build_thread_list`, `select` | — |
| `test_integration` | *(launches compiled `avr-updi-gdb` binary via `execv()`)* | *(none)* | — |

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

1. `src/elf_parser.h` — define `ElfContext` and `AvrOsSymbolIndex` structs; declare `elf_open()`, `elf_close()`, `elf_find_avros_tables()`, `elf_flash_addr()`.
2. `src/elf_parser.c` — validate ELF magic + `ELFCLASS32` + `EM_AVR`; scan `PT_LOAD` segments for `flash_base`/`sram_base`; `malloc` `.symtab` + `.strtab`; single O(sym\_count) scan for the 8 avrOS sentinel names; `elf_flash_addr(vma) = (vma − flash_base) / 2`; graceful return on partial symbol match; `elf_close()` frees all heap and sets pointers to NULL.
3. `tests/fixtures/avros_full.c` — defines all 8 avrOS sentinel linker symbols via `__attribute__((section(...)))` or a linker script; compiled to `tests/fixtures/avros_full.elf` by the Makefile.
4. `tests/fixtures/avros_partial.c` — defines only 4 of the 8 symbols; tests graceful degradation.
5. `tests/fixtures/not_avr.c` — compiled for a non-AVR target (e.g. `--target=elf32-i386`) to produce an ELF with `e_machine != EM_AVR`.
6. `tests/test_elf.c` — 13 Unity tests covering: magic rejection, `EM_AVR` check, correct `flash_base`/`sram_base`, all 8 symbol names found with correct address conversions, partial symbol set, `malloc` failure injection, `elf_close()` resource-free correctness. Linked against `src/elf_parser.c tests/unity/unity.c` with no `--wrap` flags.

**Acceptance:** `make test` runs `tests/test_elf` and reports 13/13 tests passing.

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
3. `tests/test_integration.c` — 4 integration tests. Builds the real `avr-updi-gdb` binary as a Makefile prerequisite; launches it via `fork()`/`execv()` with a PTY as the UART device and a local TCP port; a helper thread simulates the AVR side (responds to UPDI BREAK+SYNCH, handles `updi_mem_read` sequences); a raw TCP socket connects as the GDB client and sends RSP packets; tests measure startup latency (HLR-005), verify RSP packet purity (HLR-020), check build portability (HLR-033), and verify no non-libc dependencies (HLR-034).

**Acceptance:** `make test` runs all 7 test binaries; all 106 tests pass (14 + 23 + 11 + 13 + 28 + 14 + 4). `make` builds the final `avr-updi-gdb` binary without warnings. `ldd avr-updi-gdb` shows only libc. `python3 tools/lint_project.py` reports 0 errors, 0 warnings.

**CLI argument specification:**

`avr-updi-gdb [--port <port>] [--baud <baud>] [--load] <serial-device> <elf-file>`

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
| 4 | Monitor + RSP | XL — 15 RSP handler table entries, 28 test cases, O-packet hex encoding, circular header dependency |
| 5 | Application Entry Point + Integration | L — `main.c` itself is thin; integration test harness (fork/PTY/TCP) is the dominant effort |

## 11. Out-of-Scope Follow-ups

*   **macOS CI (GitHub Actions).** A workflow that runs `make test` on `macos-latest` to catch portability regressions; depends on `brew install avr-gcc` being available on the hosted runner.
*   **ASAN/UBSan CI job.** A second CI job that repeats `make test` with `-fsanitize=address,undefined` to catch memory errors and undefined behaviour; already supported by `make ASAN=1` but not wired into CI.
*   **Live hardware integration test.** An end-to-end script that connects a real AVR DA/DB target via a USB-serial adapter, attaches `avr-gdb`, and verifies `info threads` output and `monitor avros events` decoding.
*   **Console bridge live testing.** `updi_console_poll()` reads the avrOS UART ring buffer; verifying this against a live target running actual UART output is deferred to post-implementation.
*   **Flash-load smoke test.** Exercising the full `--load` code path (ELF → NVM write → verify) on hardware is deferred; the NVM write logic is covered by unit tests but not by a write-then-read-back live test.
