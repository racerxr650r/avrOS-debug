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
This document describes the design of the source modules that implement the avr-updi-gdb server:

*   [src/main.c](../src/main.c): Entry point, CLI argument parsing, and the top-level select()-based event loop.
*   [src/updi.c](../src/updi.c): UPDI physical layer: UART serial management, UPDI protocol framing, NVM flash programming, and UPDI console bridge.
*   [src/gdb_rsp.c](../src/gdb_rsp.c): GDB Remote Serial Protocol server: packet codec, command dispatch, and session lifecycle management.
*   [src/elf_parser.c](../src/elf_parser.c): ELF parser: locates avrOS FLASH-resident system tables and produces the AvrOsSymbolIndex.
*   [src/fsm_mapper.c](../src/fsm_mapper.c): avrOS FSM-to-GDB virtual thread translator: maps FSM state tables to GDB thread objects and synthesizes per-thread register frames.
*   [src/monitor.c](../src/monitor.c): Custom monitor command handler: implements the avros events, queues, and mempool sub-commands via non-intrusive UPDI reads.
*   [Makefile](../Makefile): Build orchestration: compile, test, install, uninstall, check-tools, and bundle (Debian .deb, Red Hat .rpm, Homebrew formula) targets.
*   [doc/avr-updi-gdb.1](../doc/avr-updi-gdb.1): Unix man page: reference documentation for the avr-updi-gdb command.

It does not cover the build system, IDE adapter layers (Cortex-Debug, Zed DAP), or Windows support, all of which are out of scope for the initial release (see `doc/PVD.md §7.2`).

### 1.3 Project Overview
`avr-updi-gdb` is a POSIX C99 GDB stub that bridges the UPDI debug interface of modern AVR microcontrollers (DA/DB families) to standard IDEs over the GDB Remote Serial Protocol (RSP). A developer launches the stub, points it at a serial device and an ELF binary, and connects any GDB-compatible front-end — VS Code with Cortex-Debug, Zed with its DAP adapter, or bare `avr-gdb` — using the standard `target extended-remote` command.

The server connects to the target via a TTL-level UART serial adapter with a 1 kΩ resistor on the UPDI line. No external JTAG programmer or proprietary debugger hardware is required — only a USB-to-serial adapter or direct Raspberry Pi UART pins.

Unlike conventional GDB stubs that expose a flat memory model, `avr-updi-gdb` provides native avrOS state-machine awareness. At attach time it parses the supplied ELF binary to locate the avrOS FSM registration tables in FLASH, then presents each registered finite state machine as a standard GDB virtual thread. The currently executing FSM appears as the active thread; all suspended FSMs appear as additional threads, each with a synthetic register frame whose PC points to the FSM's current state function pointer.

The project ships with a `make install` target that installs the compiled binary to `$(PREFIX)/bin/` and the accompanying Unix man page to `$(PREFIX)/share/man/man1/`. A `make check-tools` target validates that all required build tools (`gcc`, `make`, `avr-gcc`, `avr-binutils`) are present on the host before any compilation is attempted. A `make bundle` target produces native distribution packages for three platforms: a Debian binary package (`.deb`), a Red Hat RPM package (`.rpm`), and a Homebrew formula (`dist/avr-updi-gdb.rb`) for macOS. All output artefacts are written under the `dist/` directory. These targets ensure the project can be built, deployed, and distributed by a developer from a single `make` command sequence with no manual file copying.

### 1.4 Definitions, Acronyms, and Abbreviations
*   **UPDI:** Unified Program and Debug Interface — the single-wire debug protocol used by AVR DA/DB-family microcontrollers. Carried over a UART at the configured baud rate with a 1 kΩ isolation resistor on the UPDI pin.
*   **RSP:** GDB Remote Serial Protocol — the text-based packet protocol that `avr-gdb` uses to communicate with a remote stub over a socket or serial link.
*   **FSM:** Finite State Machine — the cooperative task unit in avrOS, represented at runtime by a function pointer into a FLASH-resident state table. All FSMs share the single hardware stack.
*   **ELF:** Executable and Linkable Format — the binary file produced by `avr-gcc` that carries machine code, DWARF debug information, and the symbol table used to locate avrOS system tables.
*   **avrOS:** A cooperative real-time operating system for AVR microcontrollers. Tasks are modelled as FSMs; inter-task communication uses queues, event bitmasks, and memory pools.
*   **Virtual Thread:** A GDB thread object synthesized from an avrOS FSM entry. Each virtual thread has a unique GDB thread ID and a synthesized register frame whose PC is set to the FSM's current state function pointer.
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
`avr-updi-gdb` is a single-process C99 application. Responsibilities are divided into six source modules arranged in protocol layers: an entry-point and event-loop layer, a hardware layer (UPDI), a network layer (GDB RSP), and three application-layer modules (ELF parser, FSM mapper, monitor handler).

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
*   **[src/fsm_mapper.c](../src/fsm_mapper.c)** — FSM mapper; reads UPDI memory using the symbol index and exposes avrOS FSMs as GDB virtual threads.
*   **[src/monitor.c](../src/monitor.c)** — Monitor handler; implements the custom `monitor avros events|queues|mempool` introspection commands without halting the CPU.

The startup and attach sequence proceeds as follows:

1.  `main()` parses argv, opens the UART serial device, and starts the GDB TCP listener socket.
2.  A GDB client connects; the RSP layer sends the initial hello and negotiates capabilities via `qSupported`.
3.  On the first `vAttach` or `?` packet, the ELF parser scans the supplied `.elf` file and populates the `AvrOsSymbolIndex` with avrOS FSM table addresses.
4.  The FSM mapper reads the FLASH-resident tables via UPDI background reads and builds the virtual thread list.
5.  Subsequent GDB packets (register reads, memory reads, breakpoints, step/continue) are dispatched through the RSP layer to the UPDI layer.
6.  `monitor avros` commands are intercepted by the monitor handler, which issues UPDI background reads without halting the CPU core.

### 2.2 Design Goals and Constraints
*   **FSM-First Visualization:** Virtual threads map 1-to-1 to avrOS FSM table entries. The server never attempts to unwind dormant stack frames; execution context is defined solely by the FSM's current state function pointer.
*   **Lean Host Architecture:** The server is a single C99 process. Runtime dependencies are limited to libc and the POSIX serial and socket APIs. No Java, Python virtualization, or Electron runtime is required on the host.
*   **Editor-Agnostic Core:** The server exposes only standard GDB RSP. IDE-specific integration (Cortex-Debug, Zed DAP adapter) is handled entirely by the GDB client; the server does not implement any IDE extension protocol.
*   **Non-Intrusive Polling:** `monitor avros` commands use UPDI background reads, which can be issued while the CPU is running. The core is halted only by an explicit user breakpoint or a step/continue boundary.
*   **Fail-Safe Address Resolution:** If ELF symbol resolution fails to locate the avrOS system tables, the server degrades gracefully to a standard bare-metal GDB stub rather than refusing to connect.
*   **Single-Threaded Concurrency Model:** The server uses a single POSIX `select()` event loop with no POSIX threads. All module entry points are synchronous and return to the event loop promptly. Long-blocking operations (NVM programming, UPDI link initialisation) are permitted only at startup or in direct response to an explicit GDB command, never during the event-loop hot path.
*   **Deterministic Memory Allocation:** Static and stack allocation are used for all hot-path data structures (`AppConfig`, `FsmContext`, RSP packet buffers, breakpoint table). The only heap (`malloc`) usage is in `elf_open()` to load the ELF symbol and string tables; this allocation is bounded by the size of the ELF binary and occurs once per session at attach time. `elf_close()` frees all heap memory on detach.
*   **POSIX C99 Build:** The stub is compiled with a host C99 compiler (gcc ≥ 4.8 or clang ≥ 3.4) against the POSIX.1-2008 API. No compiler-specific extensions, no C11 atomics, and no dynamic libraries beyond libc are required. The Makefile produces a standalone executable with no runtime package dependencies.
*   **Modular Test Isolation:** Each module exposes a public C API declared in a matching `.h` header. The UPDI layer can be exercised against a loopback serial device or pre-recorded byte stream. The ELF parser and FSM mapper can be driven against any AVR ELF binary without live hardware. The RSP layer can be tested over a loopback TCP socket. No module requires a live AVR target to run its unit tests.

## 3. Detailed Design for [src/main.c](../src/main.c)

### 3.1 Purpose and Responsibilities
[src/main.c](../src/main.c) is the entry point for the `avr-updi-gdb` executable, providing CLI argument parsing, resource initialization, and the top-level `select()`-based event loop that multiplexes the GDB client socket and the UPDI serial device.

*   Define `main()` and parse command-line arguments into an `AppConfig` struct.
*   Open the UART serial device and pass the file descriptor to the UPDI layer.
*   Create the GDB listener socket and pass it to the RSP layer.
*   Run the event loop, dispatching each ready file descriptor to the appropriate layer until the GDB client disconnects.
*   Release all resources on exit and return an appropriate exit code.

### 3.2 External Interfaces
#### 3.2.1 Public C API

`src/main.c` has no exported public API; all functions are `static` or `main()` itself. It is the top-level consumer of all other module APIs. The only externally visible symbol is:

```c
extern volatile sig_atomic_t g_quit;  /* set to 1 by SIGINT/SIGTERM handler */
```

This variable is checked at the top of each `event_loop()` iteration and is also accessible to any module that needs to detect a shutdown-in-progress condition.

#### 3.2.2 Command-Line Arguments

`avr-updi-gdb [--port <port>] [--baud <baud>] [--load] <serial-device> <elf-file>`

*   `--port <port>` — TCP port for the GDB listener (default: `1234`).
*   `--baud <baud>` — UART baud rate (default: `115200`).
*   `--load` — flash the ELF binary to the target before attaching.
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

## 4. Detailed Design for [src/updi.c](../src/updi.c)

### 4.1 Purpose and Responsibilities
[src/updi.c](../src/updi.c) implements the UPDI physical layer, managing the UART serial port and encoding/decoding UPDI protocol frames for memory access, execution control, NVM flash programming, and UPDI-based console bridging.

*   Open, configure (8N2, half-duplex), and close the UART serial device.
*   Encode and transmit UPDI commands (`STCS`, `LDCS`, `ST`, `LD`, `KEY`, `REPEAT`) over the UART.
*   Receive UPDI response frames and validate ACK/NAK bytes.
*   Provide non-intrusive background memory reads while the CPU is running.
*   Implement NVM write sequences for FLASH page erase and program.
*   Export execution-control primitives: halt, run, and single-step.
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
```

Error return convention: all functions return 0 on success and -1 on error (errno not set); `updi_nvm_write_flash()` additionally returns `UPDI_ERR_WP` (-2) when FLASH write-protection is active.

#### 4.2.2 Hardware Connection

Half-duplex UART at the configured baud rate (default 115200). The AVR UPDI pin is connected to both TX and RX on the host adapter through a 1 kΩ isolation resistor. The UPDI protocol requires 8N2 framing (8 data bits, no parity, 2 stop bits).


### 4.3 Internal Structure
#### 4.3.1 Key Data Structures

UPDI protocol constants used throughout the module:

| Constant | Value | Description |
| -------- | ----- | ----------- |
| `UPDI_SYNCH`       | `0x55` | UPDI synchronisation character sent after BREAK. |
| `UPDI_ACK`         | `0x40` | Acknowledgement byte returned by the target after each data byte. |
| `UPDI_MAX_BLOCK`   | `256`  | Maximum bytes transferred in a single REPEAT+LD/ST burst. |
| `UPDI_BREAK_BAUD`  | `300`  | Temporary baud rate used to generate the BREAK condition. |

UPDI ASI (Application System Interface) registers used for execution control:

| Register | Offset | Description |
| -------- | ------ | ----------- |
| `ASI_SYS_STATUS` | `0x0B` | System status; bit 3 = STOPPED (CPU halted). |
| `ASI_SYS_CTRL`   | `0x0C` | System control; bit 5 = CLKREQ. |
| `ASI_RESET_REQ`  | `0x08` | Write `0x59` to request CPU reset; write `0x00` to release. |
| `ASI_CTRLA`      | `0x02` | UPDI control register A; bit 2 = IBD (inter-byte delay enable). |

NVM controller registers (accessed via UPDI ST/LD at base address `0x1000`):

| Register | Offset | Description |
| -------- | ------ | ----------- |
| `NVMCTRL_CTRLA`  | `0x00` | NVM command register; write command code to initiate NVM operation. |
| `NVMCTRL_STATUS` | `0x02` | Status register; bit 0 = BUSY. |


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
        2.  For each block: transmit SYNCH byte (`0x55`), the REPEAT opcode with count `(block_len - 1)`, then the LD (pointer auto-increment) opcode and the 16-bit address in little-endian order.
        3.  Read `block_len` response bytes from the UART into the destination buffer, accumulating ACK bytes between data bytes as required by the UPDI framing spec.
        4.  Return -1 immediately on any `read()` timeout (no byte received within 100 ms) or on a framing error (unexpected byte in place of ACK).

*   **`int updi_mem_write(int fd, uint32_t addr, const uint8_t *buf, size_t len)`** — Write len bytes to target address addr via UPDI ST; return 0 or -1.
*   **`int updi_halt(int fd)`**
    *   Purpose: Halt the CPU core by asserting the UPDI ASI halt request and polling until STOPPED.
    *   Return Value: 0 when the CPU reports STOPPED within 50 ms; -1 on timeout or UPDI error.
    *   Logic:
        1.  Write `0x01` to `ASI_SYS_CTRL` to request a halt via the STCS opcode.
        2.  Poll `ASI_SYS_STATUS` via LDCS at 1 ms intervals until bit 3 (STOPPED) is set or 50 ms elapses.
        3.  Return 0 on success; return -1 on timeout.

*   **`int updi_run(int fd)`** — Resume CPU execution; return 0 or -1.
*   **`int updi_step(int fd)`** — Single-step one instruction and halt; return 0 or -1.
*   **`int updi_nvm_write_flash(int fd, uint32_t word_addr, const uint8_t *data, size_t len)`**
    *   Purpose: Erase and program one or more FLASH pages starting at word_addr using the UPDI NVM controller write-page sequence.
    *   Pre-condition: Target CPU is halted (`updi_halt()` has been called); `word_addr` is aligned to a FLASH page boundary; `len` is a non-zero multiple of the target FLASH page size (512 bytes for AVR DA/DB).
    *   Return Value: 0 on success; -1 on NVM timeout or UPDI communication error; `UPDI_ERR_WP` (a distinct negative constant) if FLASH write protection is detected in `NVMCTRL_STATUS`.
    *   Logic:
        1.  Enter NVM programming mode: transmit the UPDI KEY command with the 8-byte NVM key string `"NVMProg "`.
        2.  Poll `ASI_SYS_STATUS` until bit 4 (NVMPROG) is set; timeout after 100 ms.
        3.  For each FLASH page: write the page-sized data block via `updi_mem_write()` using a REPEAT+ST burst to the target page buffer address.
        4.  Issue the NVMCTRL ERWP (Erase + Write Page) command: write the command code `0x03` to `NVMCTRL_CTRLA`.
        5.  Poll `NVMCTRL_STATUS` bit 0 (BUSY) until clear; timeout after 20 ms per page.
        6.  Check `NVMCTRL_STATUS` bit 2 (WRERROR); if set, return `UPDI_ERR_WP`.
        7.  After all pages are written, exit NVM mode by writing the UPDI LDCS/STCS sequence to clear NVMPROG.

*   **`int updi_console_poll(int fd, char *buf, size_t cap)`** — Poll the UPDI console channel and copy any pending bytes to buf; return byte count or -1.

#### 4.3.3 Parsing Strategy / Algorithm

**UPDI link initialisation sequence (performed inside `updi_open()`):**

1.  Open the serial port with `O_RDWR | O_NOCTTY | O_NONBLOCK`; apply `fcntl()` to restore blocking mode with a 100 ms read timeout via `VTIME`.
2.  Configure `termios` for raw 8N2 mode: `cfmakeraw()`, set 2 stop bits (`CSTOPB`), disable parity, disable flow control, and apply `cfsetispeed()` / `cfsetospeed()` for the operating baud rate.
3.  Generate a BREAK condition: temporarily lower the baud rate to 300 baud, write a single `0x00` byte (which occupies the line for ≥ 24.6 µs — the UPDI minimum break duration), then flush and restore the operating baud rate.
4.  Transmit the SYNCH byte (`0x55`) and discard the half-duplex loopback echo.
5.  Transmit an `LDCS ASI_SYS_STATUS` command and read the response byte; verify the target is reachable. Retry the BREAK+SYNCH sequence up to three times before returning -1.

**Half-duplex echo cancellation:** Because TX and RX share the same physical wire, every transmitted byte is echoed back on the RX line. All UPDI transmit helpers skip one echo byte per transmitted byte before reading response data.

**UPDI timing budget:**

| Operation | Timeout | Notes |
| --------- | ------- | ----- |
| `read()` per byte | 100 ms (`VTIME=1`) | Applied via `termios` `VTIME`; any idle gap triggers a framing error. |
| `updi_halt()` poll interval | 1 ms | `nanosleep(1 ms)` between `LDCS ASI_SYS_STATUS` reads. |
| `updi_halt()` total timeout | 50 ms | Returns -1 if CPU does not enter STOPPED state within 50 polls. |
| `updi_nvm_write_flash()` NVMPROG wait | 100 ms | Timeout on `ASI_SYS_STATUS` bit 4 (NVMPROG). |
| `updi_nvm_write_flash()` page BUSY poll | 20 ms per page | Timeout on `NVMCTRL_STATUS` bit 0 (BUSY). |
| BREAK condition duration | ≥ 24.6 µs | One `0x00` byte at 300 baud = 33.3 µs; safely exceeds UPDI minimum. |

**UPDI command opcode encoding:** Each command frame begins with the SYNCH byte `0x55` followed by a command byte. Key opcodes:

| Opcode mnemonic | Byte value | Purpose |
| --------------- | ---------- | ------- |
| `LDCS rd, cs`   | `0x80\|cs` | Load Control/Status register `cs`. |
| `STCS cs, rr`   | `0xC0\|cs` | Store to Control/Status register `cs`. |
| `ST ptr++, rr`  | `0x64`     | Store with pointer post-increment (burst write). |
| `LD rd, ptr++`  | `0x24`     | Load with pointer post-increment (burst read). |
| `REPEAT`        | `0xA0`     | Set repeat count for next bulk transfer (n-1 in operand). |
| `KEY`           | `0xE0`     | Transmit 8-byte key to unlock a privileged mode. |

**Test approach for `src/updi.c`:** Unit-testable using a POSIX pseudo-terminal pair (`openpty()`): one end is passed to `updi_open()`, the other is driven by the test harness. Test cases cover: correct SYNCH/ACK exchange, BREAK regeneration, echo-cancellation correctness, block-split boundary at `UPDI_MAX_BLOCK`, halt-timeout expiry, NVM ERWP sequence byte order, and `UPDI_ERR_WP` detection.

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

/* Dispatch */
void rsp_dispatch(int fd, const char *packet, RspHandlers *h);
```

The `RspHandlers` struct is also declared in `src/gdb_rsp.h` and must be populated by the caller before passing to `rsp_dispatch()`. Socket options applied by `rsp_listen()`: `SO_REUSEADDR` (allow rapid server restart) and `TCP_NODELAY` (disable Nagle algorithm to minimise GDB round-trip latency).

#### 5.2.2 GDB Network Interface

TCP server socket on the configured port (default `1234`). Accepts exactly one client connection at a time; a new connection is accepted after the previous client detaches.


### 5.3 Internal Structure
#### 5.3.1 Key Data Structures

**`RspHandlers`** — Function pointer table dispatched by `rsp_dispatch()`. Each handler receives the raw packet payload string, the client socket fd, and a pointer to the shared application context.

| Field | Packet(s) handled | Description |
| ----- | ----------------- | ----------- |
| `on_halt_reason`  | `?`                       | Stop-reason query; returns `T05thread:<id>;` |
| `on_read_regs`    | `g`                       | Read all 35 AVR registers as hex. |
| `on_write_regs`   | `G`                       | Write all registers (used by GDB for register restore). |
| `on_read_mem`     | `m addr,len`              | Read memory; dispatches to UPDI for both FLASH and SRAM addresses. |
| `on_write_mem`    | `M addr,len:data`         | Write memory via UPDI ST. |
| `on_continue`     | `c`, `vCont;c`            | Resume target; calls `updi_run()` and `fsm_invalidate()`. |
| `on_step`         | `s`, `vCont;s`            | Single-step; calls `updi_step()`. |
| `on_insert_bp`    | `Z0 addr,kind`            | Insert software breakpoint by patching FLASH with AVR BREAK opcode. |
| `on_remove_bp`    | `z0 addr,kind`            | Remove software breakpoint by restoring the saved instruction word. |
| `on_thread_info`  | `qfThreadInfo`/`qsThreadInfo` | Enumerate virtual thread IDs from `FsmContext`. |
| `on_thread_extra` | `qThreadExtraInfo`        | Return FSM name string as hex-encoded ASCII. |
| `on_monitor`      | `qRcmd`                   | Hex-decode the command body and forward to `monitor_dispatch()`. |
| `on_detach`       | `D`, `k`                  | Detach: resume target and close client fd. |

**Breakpoint table:** `RSP_MAX_BREAKPOINTS` (16) software breakpoint slots are maintained as a static array of `{ uint32_t addr; uint16_t saved_word; }` structures. The AVR BREAK opcode (`0x9598`) is written to the target via `updi_nvm_write_flash()` on insertion and the saved instruction word is restored on removal.

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
*   **`void rsp_dispatch(int fd, const char *packet, RspHandlers *h)`**
    *   Purpose: Identify the RSP packet type from the first character (and optionally subsequent characters) of packet and invoke the matching handler in h.
    *   Logic:
        1.  Match the leading characters of `packet` against the handler table using a switch on `packet[0]` with secondary string comparisons for multi-character commands (`qS`, `qf`, `vC`, `Z0`, `z0`).
        2.  Invoke the matching handler function pointer from `h`, passing `fd`, `packet`, and the shared application context pointer.
        3.  If no handler matches, call `rsp_send_packet(fd, "")` to send the mandatory empty response `$#00`.
    *   Notes: `qSupported` and `qAttached` are handled inline (not via the `RspHandlers` table) because their responses are fixed strings that do not require target interaction.


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
| `bp_table[RSP_MAX_BREAKPOINTS]` | `struct { uint32_t addr; uint16_t saved_word; }[16]` | 96 B | Software breakpoint table. |
| `g_thread`, `c_thread` | `int` | 8 B | Selected GDB thread IDs for register and continue operations. |

Total static BSS in `src/gdb_rsp.c`: approximately 4.2 KiB.

**TCP socket options applied by `rsp_listen()`:**

*   `SO_REUSEADDR` — allows rapid server restart without waiting for TIME_WAIT to expire.
*   `TCP_NODELAY` — disables Nagle's algorithm; each `write()` is sent immediately, which is critical for GDB round-trip latency (GDB sends many small packets and waits for each response before continuing).

**Test approach for `src/gdb_rsp.c`:** Unit-testable using a pair of connected loopback sockets. Test cases cover: packet reception with correct and incorrect checksums, ACK/NAK behaviour before and after `QStartNoAckMode`, `rsp_dispatch()` routing to each handler, empty-response for unknown packets, breakpoint table insert/remove/overflow, and client disconnect detection.

### 5.4 Dependencies

*   `src/updi.c` — execution control and memory access.
*   `src/fsm_mapper.c` — virtual thread enumeration and per-thread register state.
*   `src/monitor.c` — custom `monitor avros` commands.
*   POSIX socket API.

### 5.5 Error Handling and Logging

*   **Checksum mismatch** Send `-` (NAK) and await retransmit from the GDB client.
*   **Unrecognized packet type** Send the empty response `$#00` as required by the RSP specification.
*   **Client disconnect** Close the client fd, reopen the listener, and wait for the next GDB connection.
*   **Breakpoint table full** Return GDB error packet `E08`; no breakpoint is installed.

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
        4.  For `_end` symbols, subtract the paired `_start` VMA and divide by the per-entry struct size to compute the entry count (e.g., `fsm_table_count`).
        5.  For the `__avros_event_mask` and `__avros_current_fsm` symbols (SRAM-resident variables), store the VMA directly without the Harvard offset.

*   **`uint32_t elf_flash_addr(const ElfContext *ctx, uint32_t vma)`** — Convert an ELF virtual memory address to a physical FLASH word address.

#### 6.3.3 Parsing Strategy / Algorithm

`elf_open()` validates and loads the ELF as follows:

1.  Read the first 4 bytes; verify the ELF magic (`0x7F 'E' 'L' 'F'`). Verify `e_ident[EI_CLASS] == ELFCLASS32`.
2.  Verify `e_machine == EM_AVR` (0x0053); reject non-AVR ELF files with a warning.
3.  Scan `e_phnum` program headers at offset `e_phoff`; for each `PT_LOAD` segment, record the segment VMA (`p_vaddr`) and size (`p_filesz`) to populate `flash_base` / `flash_size` (first LOAD segment) and `sram_base` / `sram_size` (second LOAD segment).
4.  Scan `e_shnum` section headers at offset `e_shoff`; find the section with `sh_type == SHT_SYMTAB`. Read its `sh_size / sizeof(Elf32_Sym)` entries into a heap buffer.
5.  Load the associated string table section (index given by `sh_link` on the `.symtab` section header) into a second heap buffer.

`elf_find_avros_tables()` scans the loaded symbol array for the following names:

| Symbol name | `AvrOsSymbolIndex` field | Address space |
| ----------- | ------------------------ | ------------- |
| `__avros_fsm_table_start`     | `fsm_table_addr`      | FLASH (word addr) |
| `__avros_fsm_table_end`       | (size computation)    | FLASH |
| `__avros_queue_table_start`   | `queue_table_addr`    | FLASH (word addr) |
| `__avros_queue_table_end`     | (size computation)    | FLASH |
| `__avros_event_mask`          | `event_mask_addr`     | SRAM (byte addr) |
| `__avros_mempool_table_start` | `mempool_table_addr`  | FLASH (word addr) |
| `__avros_mempool_table_end`   | (size computation)    | FLASH |
| `__avros_current_fsm`         | `current_fsm_addr`    | SRAM (byte addr) |

Entry counts (e.g., `fsm_table_count`) are derived by computing `(_end_vma - _start_vma) / sizeof(per_entry_type)` for each table, where the per-entry size is a compile-time constant defined in the avrOS headers.

**Heap memory budget for `elf_open()`:**

| Buffer | Formula | Typical size (1000-symbol AVR ELF) |
| ------ | ------- | ---------------------------------- |
| `ctx->symtab` | `sym_count x sizeof(Elf32_Sym)` = `sym_count x 16` B | ~16 KiB |
| `ctx->strtab` | `.strtab` section `sh_size` | ~8 KiB |
| Total | | ~24 KiB per session |

Both buffers are freed by `elf_close()`. Peak heap usage occurs between `elf_open()` and the first `elf_close()`.

**Parse complexity:** `elf_open()` performs O(e_phnum + e_shnum) file seeks plus two sequential block reads. `elf_find_avros_tables()` performs a single O(sym_count) linear scan with 8 string comparisons per symbol entry.

**Test approach for `src/elf_parser.c`:** Unit-testable using pre-built AVR ELF fixtures. Test cases cover: magic validation rejection, `EM_AVR` check, correct `flash_base` / `sram_base` extraction, all 8 avrOS symbol names found with correct address conversions, partial symbol set (graceful degradation), `malloc` failure via injection shim, and `elf_close()` resource-free correctness.

### 6.4 Dependencies

*   Standard C file I/O.
*   ELF header definitions from `<elf.h>` (Linux) or a bundled `elf.h` for macOS portability.

### 6.5 Error Handling and Logging

*   **Non-ELF or wrong architecture magic** Log a warning message and return -1.
*   **Missing avrOS symbols** Log an informational message and return 0 with an empty `AvrOsSymbolIndex`; the server continues in bare-metal stub mode.
*   **Memory allocation failure** Return -1; the caller falls back to bare-metal stub mode.

## 7. Detailed Design for [src/fsm_mapper.c](../src/fsm_mapper.c)

### 7.1 Purpose and Responsibilities
[src/fsm_mapper.c](../src/fsm_mapper.c) translates the runtime state of avrOS FSM registration tables — read from the target via UPDI — into GDB virtual threads, providing per-thread synthetic register frames to the RSP layer.

*   Use the `AvrOsSymbolIndex` to read FSM state bytes from the target's SRAM over UPDI.
*   Assign a stable GDB thread ID to each registered FSM entry.
*   Identify and report which FSM is currently executing as the active GDB thread.
*   Synthesize a minimal GDB register frame for each virtual thread, setting PC to the FSM's current state function pointer.
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
int  fsm_get_registers(const FsmContext *ctx, int thread_id, uint8_t *reg_buf);
    /* Fills reg_buf with a 78-character hex string (39 bytes binary, hex-encoded: R0-R31, SREG, SPL, SPH, PC in g-packet order); buf must be >= 79 bytes. */
    /* Returns: 0 on success; -1 if thread_id out of range. */
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
| `is_active` | `bool`     | True when this FSM is the currently executing task, as identified by comparing the FSM's SRAM entry address against the `__avros_current_fsm` scheduler variable. |

**`FsmContext`** contains a statically allocated array of `FSM_MAX_THREADS` (32) `FsmThread` entries, eliminating heap allocation in the thread build path.


#### 7.3.2 Key Functions

*   **`int fsm_build_thread_list(FsmContext *ctx, const AvrOsSymbolIndex *idx, int updi_fd)`**
    *   Purpose: Read the avrOS FSM registration table and SRAM state variables from the target via UPDI and populate the virtual thread cache.
    *   Pre-condition: Target CPU is halted; `idx->fsm_table_addr` and `idx->fsm_table_count` are valid; `updi_fd` is a valid UPDI serial fd.
    *   Post-condition: `ctx->threads[0..thread_count-1]` contain populated `FsmThread` descriptors; `ctx->active_id` identifies the running FSM; `ctx->valid` is set to true.
    *   Return Value: Number of virtual threads populated on success; -1 on UPDI read failure.
    *   Logic:
        1.  Read the FLASH-resident FSM registration table from `idx->fsm_table_addr` using `updi_mem_read()`; each entry is a fixed-size struct `{ uint16_t *state_var_sram_addr; const char *name_flash_ptr; }`.
        2.  For each table entry, read the 2-byte SRAM state variable from `state_var_sram_addr` to obtain the current state function pointer; convert to a FLASH word address and store in `thread->state_fn`.
        3.  Read the null-terminated FSM name from the FLASH address given by `name_flash_ptr` (up to 31 characters) and store in `thread->name`.
        4.  Read the 2-byte SRAM value at `idx->current_fsm_addr`; compare it to each table entry's `state_var_sram_addr` to identify the active FSM and set `thread->is_active` and `ctx->active_id`.
        5.  Assign `thread->gdb_id = loop_index + 1` for each thread (GDB thread IDs are 1-based).

*   **`int fsm_get_active_thread(const FsmContext *ctx)`** — Return the GDB thread ID of the currently executing FSM.
*   **`int fsm_get_registers(const FsmContext *ctx, int thread_id, uint8_t *reg_buf)`**
    *   Purpose: Synthesize a GDB g-packet register frame for the requested virtual thread and write it into reg_buf.
    *   Pre-condition: `ctx->valid` is true; `thread_id` is in the range [1, ctx->thread_count]; `reg_buf` points to a caller-allocated buffer of at least 79 bytes (39 binary bytes × 2 hex chars + NUL).
    *   Post-condition: `reg_buf` contains a 78-character hex string representing the 36 AVR GDB register values (R0–R31, SREG, SPL, SPH, PC) in GDB `g`-packet order.
    *   Return Value: 0 on success; -1 if thread_id is out of range.
    *   Logic:
        1.  Zero-initialise all 79 bytes of the register buffer.
        2.  Set the PC field (GDB register index 35, 4-byte little-endian) at hex positions 70–77 of the buffer to `thread->state_fn`.
        3.  If `thread->is_active` is true, read SREG (register 32), SPL (register 33), and SPH (register 34) from the target SRAM via `updi_mem_read()` and write them at hex positions 64–65, 66–67, and 68–69, respectively.
    *   Notes: Non-active threads return zeroed R0–R31 and SREG; only PC and SP are meaningful for suspended FSMs in the avrOS cooperative model.

*   **`void fsm_invalidate(FsmContext *ctx)`** — Clear the cached thread list; called on every CPU resume.

#### 7.3.3 Parsing Strategy / Algorithm

**Thread list build sequence:**

Each avrOS FSM is represented in FLASH by a linker-placed table entry of the form:

```c
typedef struct {
    fsm_state_fn_t *state;   /* pointer to SRAM variable holding current state fn */
    const char     *name;    /* pointer to FLASH string literal */
} avros_fsm_entry_t;
```

`fsm_build_thread_list()` reads `idx->fsm_table_count` consecutive entries starting at `idx->fsm_table_addr`, dereferences each SRAM state variable pointer to obtain the current state function pointer, and reads the FLASH name pointer to obtain the FSM display name. All UPDI reads are performed while the CPU is halted.

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

**UPDI read budget per `fsm_build_thread_list()` call (N threads):**

| Operation | UPDI transactions | Data volume |
| --------- | ----------------- | ----------- |
| Read FSM table (N x 4 bytes) | 1 block read | 4N bytes |
| Read SRAM state variable per thread (2 bytes each) | N reads | 2N bytes |
| Read FLASH name string per thread (up to 31 chars) | N reads | <=31N bytes |
| Read `current_fsm_addr` (2 bytes) | 1 read | 2 bytes |
| **Total** | 2N+2 transactions | <=37N+6 bytes |

**Test approach for `src/fsm_mapper.c`:** Unit-testable by providing a pre-populated `AvrOsSymbolIndex` and a mock `updi_mem_read()` returning canned byte sequences. Test cases cover: GDB thread ID assignment (1-based), correct `state_fn` FLASH word-address derivation, active thread identification, `FSM_MAX_THREADS` cap with warning, cache-invalidation round-trip, and `fsm_get_registers()` PC encoding verification.

### 7.4 Dependencies

*   `src/updi.c` — `updi_mem_read()` for SRAM reads.
*   `src/elf_parser.c` — `AvrOsSymbolIndex` for table addresses and entry counts.

### 7.5 Error Handling and Logging

*   **UPDI read failure during thread build** Return -1; the RSP layer reports a GDB error packet to the client.
*   **Thread ID out of range** Return -1 and cause the RSP layer to send error response `E01`.
*   **FSM count exceeds FSM_MAX_THREADS** Cap the build at `FSM_MAX_THREADS` entries and log a warning; excess FSMs are silently omitted from the thread list.

## 8. Detailed Design for [src/monitor.c](../src/monitor.c)

### 8.1 Purpose and Responsibilities
[src/monitor.c](../src/monitor.c) implements the custom `monitor avros` sub-commands that provide non-intrusive, human-readable visibility into the state of avrOS system objects (events, queues, and memory pools) via UPDI background reads.

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

Three sub-commands are supported:

*   `monitor avros events` — display the avrOS event bitmask with each bit decoded to its event name.
*   `monitor avros queues` — display head, tail, and count for each registered queue.
*   `monitor avros mempool` — display free-block count and capacity for each registered memory pool.


### 8.3 Internal Structure
#### 8.3.1 Key Functions

*   **`int monitor_dispatch(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx, const char *cmd)`**
    *   Purpose: Hex-decode the qRcmd payload, match the sub-command token, and invoke the appropriate handler.
    *   Pre-condition: `cmd` is the raw ASCII-hex-encoded command body extracted from a `qRcmd` RSP packet (not yet decoded).
    *   Return Value: 0 if the command was recognised and the handler returned without error; -1 on UPDI failure; -2 on unrecognised sub-command.
    *   Logic:
        1.  Hex-decode the ASCII hex pairs in `cmd` into a plain text command string.
        2.  Verify the string starts with the prefix `"avros "` (case-sensitive); if not, send a usage hint O-packet and return -2.
        3.  Extract the token following the prefix and compare it to `"events"`, `"queues"`, and `"mempool"`.
        4.  Invoke the matching static handler or send an unknown-subcommand error O-packet.

*   **`static int cmd_events(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx)`**
    *   Purpose: Read the avrOS event bitmask from SRAM and send a decoded human-readable listing to the GDB console.
    *   Logic:
        1.  Read 2 bytes from `idx->event_mask_addr` using `updi_mem_read()`.
        2.  For each of the 16 event bits (bit 0 = event 0, bit 15 = event 15), check if the bit is set and append a line `"  event<N>: SET\n"` or `"  event<N>: clear\n"` to an output buffer.
        3.  Hex-encode the output buffer and send it as one or more RSP O-packets via `rsp_send_packet()`.

*   **`static int cmd_queues(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx)`**
    *   Purpose: Read the head, tail, and occupancy count for each registered avrOS queue and send a formatted table to the GDB console.
    *   Logic:
        1.  Read `idx->queue_count` consecutive queue status structures from SRAM at `idx->queue_table_addr` using `updi_mem_read()`.
        2.  For each queue structure, extract the `head`, `tail`, and `count` fields and format a one-line entry.
        3.  Hex-encode and send the formatted table as RSP O-packets.

*   **`static int cmd_mempool(int rsp_fd, int updi_fd, const AvrOsSymbolIndex *idx)`**
    *   Purpose: Read the free-block count and total capacity for each registered avrOS memory pool and send a formatted summary to the GDB console.
    *   Logic:
        1.  Read `idx->mempool_count` consecutive pool status structures from SRAM at `idx->mempool_table_addr` using `updi_mem_read()`.
        2.  For each pool structure, extract the `free_count` and `capacity` fields and format a one-line entry showing both values and a percentage utilisation.
        3.  Hex-encode and send the formatted table as RSP O-packets.


#### 8.3.2 Parsing Strategy / Algorithm

**O-packet encoding:** All console output is delivered to the GDB client via RSP O-packets. The RSP specification requires the human-readable text to be hex-encoded: each ASCII character is converted to two hex digits. For example, the character `'A'` (0x41) is encoded as the two-character string `"41"`. `monitor_dispatch()` builds the human-readable text into a temporary 512-byte buffer, then hex-encodes it before calling `rsp_send_packet()` with the `O` prefix.

**Non-intrusive reads:** All three sub-commands use `updi_mem_read()` exclusively; none call `updi_halt()`. The UPDI background read mechanism allows SRAM to be sampled while the CPU is running, at the cost of a possible read-during-write data race for multi-byte fields. This is acceptable for the observational, non-critical nature of the monitor commands.

**avrOS runtime data structure layouts read by monitor commands:**

*Queue status struct* (read by `cmd_queues()`):
```c
typedef struct {
    uint8_t  head;     /* index of next read position */
    uint8_t  tail;     /* index of next write position */
    uint8_t  count;    /* number of items currently queued */
    uint8_t  capacity; /* maximum items the queue can hold */
} avros_queue_status_t;  /* 4 bytes, SRAM */
```

*Memory pool status struct* (read by `cmd_mempool()`):
```c
typedef struct {
    uint8_t  free_count; /* number of free blocks */
    uint8_t  capacity;   /* total blocks in the pool */
    uint16_t block_size; /* size of each block in bytes */
} avros_pool_status_t;   /* 4 bytes, SRAM */
```

**O-packet size constraint:** A single RSP O-packet payload is limited to `RSP_PACKET_MAX` bytes (2048). For `cmd_events()` output (16 lines x ~20 chars = ~320 bytes pre-encode = 640 hex chars), one O-packet suffices. For larger `cmd_queues()` and `cmd_mempool()` outputs the 512-byte text buffer is hex-encoded in chunks that each fit within one O-packet before calling `rsp_send_packet()`.

**Test approach for `src/monitor.c`:** Unit-testable by providing a mock `updi_mem_read()` returning canned SRAM bytes and capturing O-packet strings via a pipe replacing `rsp_fd`. Test cases cover: hex-decode of `qRcmd` payload, prefix rejection, all three sub-command dispatch paths, correct bit decoding in `cmd_events()`, correct queue/pool field extraction, and O-packet encoding of all output characters.

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
| `fsm_table_addr` | `uint32_t` | FLASH word address of the avrOS FSM registration table. |
| `fsm_table_count` | `uint8_t` | Number of FSM entries in the table. |
| `queue_table_addr` | `uint32_t` | FLASH word address of the queue registration table. |
| `queue_count` | `uint8_t` | Number of registered queues. |
| `event_mask_addr` | `uint32_t` | SRAM byte address of the avrOS event bitmask variable. |
| `mempool_table_addr` | `uint32_t` | FLASH word address of the memory pool registration table. |
| `mempool_count` | `uint8_t` | Number of registered memory pools. |
| `current_fsm_addr` | `uint32_t` | SRAM byte address of the avrOS scheduler current_fsm pointer variable; used by fsm_mapper to identify the active thread. |
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
    | `RSP_MAX_BREAKPOINTS` | `16`   | Capacity of the static software breakpoint table. |

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
| `all` | Build the `avr-updi-gdb` binary (default). |
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
| 6 | `src/monitor.c` | `monitor avros events/queues/mempool` output | Live hardware: run target, issue monitor commands from GDB, verify output format and values. |
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
| FSM Virtual Thread Mapping | §7 (src/fsm_mapper.c) |
| System Introspection | §8 (src/monitor.c) |
| Flash Programming | §4 (src/updi.c) |
| Console Bridge | §4 (src/updi.c) |
---
