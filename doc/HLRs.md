# High-Level Requirements

**Version:** 0.1
**Date:** 2026-05-16
**Author(s):** John Anderson

## 1. System Startup and Command-Line Interface

Requirements in this section govern how `avr-updi-gdb` is invoked, how it initialises its resources, and how quickly it becomes ready for a GDB connection.

*   <a id="HLR-001"></a>**HLR-001: CLI Argument Parsing.**
    The application shall accept the following command-line arguments: a required positional `<serial-device>` path, a required positional `<elf-file>` path, an optional `--port <port>` TCP port (default `1234`), an optional `--baud <baud>` UART baud rate (default `115200`), and an optional `--load` flag. Any unrecognised argument shall cause the application to print a usage message to `stderr` and exit with a non-zero status.
    *Trace:* [SDD Section 3.2.2](SDD.md).

*   <a id="HLR-002"></a>**HLR-002: Serial Device Initialisation.**
    The application shall open the specified UART serial device and configure it for UPDI communication (8N2, half-duplex) at the specified baud rate before accepting any GDB client connection.
    *Trace:* [SDD Section 3.1](SDD.md), [SDD Section 4.1](SDD.md).

*   <a id="HLR-003"></a>**HLR-003: GDB Listener Startup.**
    The application shall bind and listen on the configured TCP port for incoming GDB client connections. Exactly one GDB client connection shall be active at a time; after a client detaches the server shall re-enter the listening state.
    *Trace:* [SDD Section 3.1](SDD.md), [SDD Section 5.1](SDD.md).

*   <a id="HLR-004"></a>**HLR-004: ELF Flash Load Option.**
    When the `--load` flag is provided, the application shall erase and program the target FLASH with the content of the specified ELF binary via UPDI NVM write sequences before entering the GDB listener loop.
    *Trace:* [SDD Section 3.2.2](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-005"></a>**HLR-005: Attach Time Constraint.**
    The server shall complete initialisation, locate avrOS system tables in FLASH, and be ready to respond to GDB packets within 2.0 seconds of the GDB client connection being accepted.
    *Trace:* [SDD Section 2.1](SDD.md).

*   <a id="HLR-035"></a>**HLR-035: Graceful Shutdown and Resource Release.**
    On receipt of SIGINT, SIGTERM, a GDB `k` (kill) packet, or a GDB `D` (detach) packet, the application shall release all allocated resources in the following order: restore any patched FLASH breakpoint locations via UPDI NVM write, free the ELF symbol table heap allocation, close the GDB client socket file descriptor, close the UPDI serial device file descriptor, and close the TCP listener socket file descriptor. The server shall exit with status 0 on a clean shutdown and status 1 on a fatal initialisation failure.
    *Trace:* [SDD Section 3.3.3](SDD.md).

## 2. UPDI Physical Layer

Requirements in this section govern the hardware connection to the AVR target and all UPDI protocol operations.

*   <a id="HLR-006"></a>**HLR-006: UPDI Hardware Connection.**
    The application shall communicate with the AVR target exclusively over a TTL-level UART serial link using UPDI framing. The connection shall require only a 1 kΩ resistor between the host adapter TX/RX and the target UPDI pin; no JTAG controller, proprietary programmer, or external hardware debugger shall be required.
    *Trace:* [SDD Section 4.2.2](SDD.md).

*   <a id="HLR-007"></a>**HLR-007: Target Memory Read.**
    The application shall read arbitrary byte ranges from both the SRAM and FLASH address spaces of the connected AVR target via UPDI LD commands.
    *Trace:* [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-008"></a>**HLR-008: Target Memory Write.**
    The application shall write arbitrary byte ranges to the SRAM address space of the connected AVR target via UPDI ST commands.
    *Trace:* [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-009"></a>**HLR-009: FLASH Programming.**
    The application shall erase and program pages of the target FLASH using the UPDI NVM controller write sequence, enabling both the `--load` startup option and software breakpoint insertion.
    *Trace:* [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-010"></a>**HLR-010: Execution Control.**
    The application shall halt, resume, and single-step the AVR CPU core via the AVR On-Chip Debug (OCD) interface layered on top of UPDI, mapping directly to GDB continue, step, and stop operations. The Phase 2 UPDI module exposes `updi_halt`/`updi_run`/`updi_step` entry points as -1 stubs; full implementation is performed by the Phase 3 OCD layer using its own Microchip specification.
    *Trace:* [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-011"></a>**HLR-011: Non-Intrusive Background Memory Read.**
    The application shall read target memory regions via UPDI background reads while the CPU core is running, without issuing a halt. This mechanism shall be used for all system introspection operations that do not require a stopped core.
    *Trace:* [SDD Section 4.1](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-012"></a>**HLR-012: UPDI Console Bridge.**
    The application shall poll the avrOS software UART console channel via UPDI and forward any received bytes to host `stdout`, enabling interactive CLI sessions with the running firmware and automated test output capture without halting the CPU.
    *Trace:* [SDD Section 4.1](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-036"></a>**HLR-036: UPDI Link Initialisation Timing and Retry.**
    The UPDI initialisation sequence shall assert a BREAK condition by transmitting two consecutive zero (0x00) bytes at the session baud rate, holding the TX line low for well over the 24.6 µs UPDI minimum (datasheet §35.3.1.2), before transmitting the SYNCH character (0x55) and probing the link with `LDCS ASI_STATUSB`. If the target does not acknowledge the probe, the server shall retry the BREAK+SYNCH+LDCS sequence up to three times before declaring a link failure and returning -1 from `updi_open()`.
    *Trace:* [SDD Section 4.3.3](SDD.md).

*   <a id="HLR-037"></a>**HLR-037: UPDI Operation Timeout Bounds.**
    All UPDI polling loops shall implement explicit timeouts to prevent indefinite blocking: each `updi_mem_read()` `read()` call shall be guarded by a `select()` with a 100 ms deadline; the NVMPROG mode entry poll shall time out after 100 iterations (≈ 100 ms); the per-page NVM BUSY poll shall time out after 20 iterations (≈ 20 ms) per page. On any timeout the failing UPDI function shall return -1 rather than blocking or spinning.
    *Trace:* [SDD Section 4.3.3](SDD.md).

## 3. GDB Remote Serial Protocol Server

Requirements in this section govern the GDB RSP server behaviour, covering packet handling, register and memory access, breakpoint management, and session lifecycle.

*   <a id="HLR-013"></a>**HLR-013: RSP Server Accessibility.**
    The application shall implement a GDB Remote Serial Protocol server accessible via a standard `target extended-remote <host>:<port>` command from `avr-gdb` or any GDB-compatible front-end (VS Code Cortex-Debug, Zed DAP adapter).
    *Trace:* [SDD Section 5.2.2](SDD.md).

*   <a id="HLR-014"></a>**HLR-014: Register Read and Write.**
    The application shall respond to GDB register read (`g`) and write (`G`, `P`) packets, returning or updating the current AVR CPU register state (32 general-purpose registers, PC, SP, and status register) obtained from or written to the target via UPDI.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-015"></a>**HLR-015: Memory Read and Write.**
    The application shall respond to GDB memory read (`m`) and write (`M`, `X`) packets, mapping each request to the corresponding UPDI memory operation on the target. Requests addressing the FLASH region shall be routed to NVM read or write sequences as appropriate.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-016"></a>**HLR-016: Software Breakpoints.**
    The application shall implement software breakpoints by inserting a `BREAK` instruction at the requested FLASH address via UPDI NVM write, and restoring the original instruction on removal. The server shall support up to `RSP_MAX_BREAKPOINTS` (16) simultaneous software breakpoints; attempting to insert a breakpoint when the table is full shall return GDB error reply `E08`.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-017"></a>**HLR-017: Single-Step Execution.**
    The application shall respond to the GDB single-step (`s`/`S`) packet by executing exactly one AVR instruction on the target CPU via the UPDI step primitive and reporting the resulting stop reason.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-018"></a>**HLR-018: Continue Execution.**
    The application shall respond to the GDB continue (`c`/`C`) packet by resuming target CPU execution via the UPDI run primitive and blocking until a breakpoint, halt, or error condition is signalled.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-019"></a>**HLR-019: RSP Capability Negotiation and Lifecycle.**
    The application shall negotiate RSP capabilities with the GDB client via `qSupported` and shall handle the standard lifecycle packets `qAttached`, `vAttach`, `D` (detach), and `k` (kill). On detach the server shall re-enter the listen state without requiring a restart.
    *Trace:* [SDD Section 5.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-020"></a>**HLR-020: Editor-Agnostic Protocol.**
    The server shall communicate exclusively over standard GDB RSP. No IDE-specific protocol extensions (DAP, Cortex-Debug custom packets, or similar) shall be implemented inside the server; all IDE integration shall be the responsibility of the client-side adapter.
    *Trace:* [SDD Section 2.2](SDD.md), [SDD Section 5.1](SDD.md).

*   <a id="HLR-038"></a>**HLR-038: GDB Session Response Latency.**
    The GDB client socket shall be configured with `TCP_NODELAY` to disable Nagle's algorithm, ensuring that each RSP response packet is transmitted without artificial buffering delay. The listener socket shall be configured with `SO_REUSEADDR` so that the server may be restarted on the same port immediately after process exit without waiting for the TCP TIME_WAIT state to expire.
    *Trace:* [SDD Section 5.3.3](SDD.md).

## 4. ELF Parsing and Symbol Resolution

Requirements in this section govern how the server locates avrOS system tables in the target firmware and handles missing or incomplete debug information.

*   <a id="HLR-021"></a>**HLR-021: ELF Binary Parsing.**
    The application shall parse the AVR ELF binary provided at launch to validate its architecture, locate the symbol table section, and build an index of avrOS system table symbols (FSM registration table, queue table, event bitmask, and memory pool table).
    *Trace:* [SDD Section 6.1](SDD.md), [SDD Section 6.3.1](SDD.md).

*   <a id="HLR-022"></a>**HLR-022: Harvard Architecture Address Mapping.**
    The application shall apply the AVR Harvard architecture offset when converting ELF virtual memory addresses to physical FLASH word addresses, ensuring that all symbol references resolve to correct target locations regardless of the ELF's load segment layout.
    *Trace:* [SDD Section 6.3.1](SDD.md).

*   <a id="HLR-023"></a>**HLR-023: Fail-Safe Degradation on Missing Symbols.**
    When the ELF binary does not contain the expected avrOS system table symbols (e.g., firmware compiled without debug symbols, or non-avrOS firmware), the application shall log an informational diagnostic and degrade gracefully to a standard bare-metal GDB stub, rather than aborting the session or returning an error to the GDB client.
    *Trace:* [SDD Section 2.2](SDD.md), [SDD Section 6.5](SDD.md).

## 5. FSM Virtual Thread Mapping

Requirements in this section govern how the server translates the avrOS cooperative scheduler state into GDB virtual threads visible in the IDE's call-stack and thread panes.

*   <a id="HLR-024"></a>**HLR-024: FSM Thread Enumeration.**
    Upon halting the target, the application shall read the avrOS FSM registration table from target SRAM via UPDI and present each registered FSM entry as a distinct GDB virtual thread, identified by a stable thread ID.
    *Trace:* [SDD Section 7.1](SDD.md), [SDD Section 7.3.1](SDD.md).

*   <a id="HLR-025"></a>**HLR-025: Active Thread Identification.**
    The application shall identify the currently executing avrOS FSM from the runtime state read via UPDI and report it to the GDB client as the active thread. All register reads and memory operations issued without an explicit thread context shall operate on the active thread's context.
    *Trace:* [SDD Section 7.3.1](SDD.md).

*   <a id="HLR-026"></a>**HLR-026: Virtual Thread Register Frame.**
    Each virtual thread shall present a synthesized GDB register frame in which the program counter (PC) is set to the function pointer of the FSM's current state handler. For the active virtual thread, SP and SREG shall be read live from the target's hardware registers and inserted into the register frame. For all non-active virtual threads, SP and SREG shall be set to zero, as dormant FSMs do not maintain independent stacks in the avrOS cooperative scheduler model.
    *Trace:* [SDD Section 7.1](SDD.md), [SDD Section 7.3.1](SDD.md).

*   <a id="HLR-027"></a>**HLR-027: Complete FSM Thread Coverage.**
    All FSMs registered in the avrOS FSM table shall appear as virtual threads in the GDB client thread pane upon halting at or after `main()`, up to a maximum of `FSM_MAX_THREADS` (32) entries. If more than `FSM_MAX_THREADS` FSMs are registered, the server shall silently cap the virtual thread list at 32 and log a diagnostic warning; no registered FSM within the cap shall be omitted.
    *Trace:* [SDD Section 7.1](SDD.md), [SDD Section 7.3.1](SDD.md).

*   <a id="HLR-028"></a>**HLR-028: Stack-Free Thread Model.**
    The application shall not attempt to unwind stack frames for suspended FSMs. The execution context of a non-active virtual thread shall be defined solely by its state function pointer; the server shall never dereference a dormant stack pointer to infer thread state.
    *Trace:* [SDD Section 2.2](SDD.md), [SDD Section 7.1](SDD.md).

## 6. System Introspection

Requirements in this section govern the custom `monitor avros` commands that expose avrOS runtime object state to the developer without halting the target CPU.

*   <a id="HLR-029"></a>**HLR-029: Monitor Events Command.**
    The application shall implement a `monitor avros events` command that reads the avrOS event bitmask from the target via UPDI background read and returns a human-readable report to the GDB console, decoding each set bit to its symbolic event name, without halting the CPU core.
    *Trace:* [SDD Section 8.2.2](SDD.md), [SDD Section 8.3.1](SDD.md).

*   <a id="HLR-030"></a>**HLR-030: Monitor Queues Command.**
    The application shall implement a `monitor avros queues` command that reads each registered avrOS queue's head index, tail index, and current occupancy count from the target via UPDI background read and returns a human-readable report to the GDB console, without halting the CPU core.
    *Trace:* [SDD Section 8.2.2](SDD.md), [SDD Section 8.3.1](SDD.md).

*   <a id="HLR-031"></a>**HLR-031: Monitor Memory Pool Command.**
    The application shall implement a `monitor avros mempool` command that reads each registered avrOS memory pool's free-block count and total capacity from the target via UPDI background read and returns a human-readable report to the GDB console, without halting the CPU core.
    *Trace:* [SDD Section 8.2.2](SDD.md), [SDD Section 8.3.1](SDD.md).

*   <a id="HLR-032"></a>**HLR-032: Introspection Reliability.**
    The `monitor avros events` command shall accurately reflect the current pending event bits across at least 10,000 consecutive invocations of `monitor_dispatch()` with the `avros events` sub-command without producing a UPDI protocol desync, a corrupted response, or a server crash.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 8.1](SDD.md).

## 7. Platform and Build

Requirements in this section govern the host platforms the server must support and the constraints on its runtime dependencies.

*   <a id="HLR-033"></a>**HLR-033: Native Linux and macOS Build.**
    The application shall compile and execute natively on Linux (x86-64 and ARM) and macOS (x86-64 and Apple Silicon) using a C99-conforming toolchain. The build shall not require any platform-specific preprocessor workarounds beyond what the C99 standard and POSIX.1-2008 provide.
    *Trace:* [SDD Section 2.2](SDD.md).

*   <a id="HLR-034"></a>**HLR-034: Lean Host Runtime Dependencies.**
    The application runtime shall depend only on the host C standard library and POSIX serial and socket APIs. No Java runtime, Python interpreter, Electron-based framework, or other heavyweight dependency shall be required to launch or operate the server.
    *Trace:* [SDD Section 2.2](SDD.md).

*   <a id="HLR-039"></a>**HLR-039: Single-Threaded Event Loop Architecture.**
    The server shall multiplex all I/O using a single POSIX `select()`-based event loop with no POSIX threads. All module entry points shall be called synchronously from the event loop and shall return to it promptly. Long-blocking operations (NVM flash programming and UPDI link initialisation) shall be permitted only during server startup or in direct response to an explicit GDB command, and shall not block the event loop in any other context.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 2.2](SDD.md), [SDD Section 3.3.2](SDD.md).

*   <a id="HLR-040"></a>**HLR-040: Bounded Heap Allocation.**
    The server shall not allocate heap memory on any hot path (packet processing, UPDI reads, FSM enumeration, monitor commands). Heap allocation via `malloc` shall be used exclusively inside `elf_open()` to load the ELF symbol and string table sections; this allocation shall be bounded by the size of the ELF binary and shall occur at most once per GDB session. All heap memory shall be freed by `elf_close()` at session end or on error.
    *Trace:* [SDD Section 2.2](SDD.md), [SDD Section 9](SDD.md).

## 8. Installation and Documentation

Requirements in this section govern the Makefile installation targets and the user-facing documentation deliverables that accompany the compiled binary.

*   <a id="HLR-041"></a>**HLR-041: Makefile Install and Uninstall Targets.**
    The Makefile shall provide `install` and `uninstall` targets that accept a `PREFIX` variable (defaulting to `/usr/local`). The `install` target shall copy the compiled `avr-updi-gdb` binary to `$(PREFIX)/bin/` and the man page to `$(PREFIX)/share/man/man1/`. The `uninstall` target shall remove exactly those files. A `check-tools` target shall verify that all required host tools (`gcc`, `make`, `avr-gcc`, `avr-nm`) are available before any build step, printing a diagnostic and exiting non-zero if any required tool is absent.
    *Trace:* [SDD Section 2.2](SDD.md).

*   <a id="HLR-042"></a>**HLR-042: User Manual and Unix Man Page.**
    The project shall provide a user manual (`doc/UserManual.md`) and a Unix man page (`doc/avr-updi-gdb.1`). The man page shall be parseable by the standard `man` utility and shall document the command synopsis, all options, operands, exit codes, and at least one usage example. The `doc/UserManual.md` shall document prerequisites, build instructions, usage, CLI options, and connection wiring for the UPDI adapter.
    *Trace:* [SDD Section 2.2](SDD.md).

*   <a id="HLR-043"></a>**HLR-043: Distribution Package Bundle.**
    The Makefile shall provide a `bundle` target that produces native distribution packages for three target platforms: a Debian binary package (`dist/avr-updi-gdb_$(VERSION)_amd64.deb`) for Debian/Ubuntu Linux, an RPM binary package (`dist/avr-updi-gdb-$(VERSION)-1.x86_64.rpm`) for Red Hat/Fedora Linux, and a Homebrew formula (`dist/avr-updi-gdb.rb`) for macOS. Each package shall include the `avr-updi-gdb` binary and the man page. All output artefacts shall be written under the `dist/` directory. A `VERSION` variable (defaulting to the value extracted from `git describe`) shall parameterise every package version string.
    *Trace:* [SDD Section 2.2](SDD.md).
