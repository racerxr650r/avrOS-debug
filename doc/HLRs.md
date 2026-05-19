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

*   <a id="HLR-004"></a>**HLR-004: ELF Load Option.**
    When the `--load` flag is provided, the application shall classify every `PT_LOAD` segment of the supplied ELF binary by virtual address against the AVR-Dx unified UPDI address windows (FLASH, EEPROM, USERROW, FUSES, LOCK, SIGROW) and dispatch each segment to the matching UPDI NVM write routine before entering the GDB listener loop. Segments whose virtual addresses fall outside every programmable window shall cause `--load` to fail with a diagnostic; SIGROW segments shall be silently skipped (signature row is read-only); SRAM-resident segments shall be skipped without programming.
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
    The application shall erase and program pages of the target FLASH using the UPDI NVM controller write-page sequence (NVMCTRL command `ERWP` = `0x03`, page-aligned address, 512-byte pages padded with `0xFF`, per-page `FBUSY` poll on `NVMSTATUS` bit 0). FLASH programming is one of several NVM kinds dispatched by `--load`; see HLR-046 for the EEPROM/USERROW/FUSES/LOCK kinds and HLR-047 for the lockbit safety interlock. FLASH programming also underpins software breakpoint insertion.
    *Trace:* [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-010"></a>**HLR-010: Execution Control.**
    The application shall halt, resume, and single-step the AVR CPU core via the AVR On-Chip Debug (OCD) interface layered on top of UPDI, mapping directly to GDB continue, step, and stop operations. `updi_enter_debug()` shall arm OCD mode at session start by sending the `OCD ` 8-byte KEY followed by an `ASI_RESET_REQ` pulse, after which `updi_halt()`, `updi_run()`, `updi_step()`, `updi_ocd_poll_halted()`, and `updi_ocd_read_halt_status()` provide the primitives that the RSP layer composes into GDB stop/continue/step operations.
    *Trace:* [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-011"></a>**HLR-011: Non-Intrusive Background Memory Read.**
    The application shall read target memory regions via UPDI background reads while the CPU core is running, without issuing a halt. This mechanism shall be used for all system introspection operations that do not require a stopped core.
    *Trace:* [SDD Section 4.1](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-012"></a>**HLR-012: UPDI Console Bridge.**
    The application shall poll the avrOS software UART console channel via UPDI and forward any received bytes to host `stdout`, enabling interactive CLI sessions with the running firmware and automated test output capture without halting the CPU.
    *Trace:* [SDD Section 4.1](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-036"></a>**HLR-036: UPDI Cold-Start Contention Handshake.**
    The UPDI link initialisation sequence shall make a fresh power-on AVR-Dx target reachable on host wirings that combine TX and RX onto the single UPDI line via a passive resistor (notably Raspberry Pi PL011 and FTDI-based serial adapters), where the host's push-pull idle-high TX otherwise drives the target's UPDI collision detector and silences responses. `updi_open()` shall execute up to three cold-start attempts that mirror avrdude's `serialupdi` programmer (the reference implementation against which behaviour has been verified by `strace`): attempt 0 (fast path, succeeds against an already-prepped target) shall transmit one 0x00 wake byte at session baud followed by `tcdrain()` + `tcflush(TCIFLUSH)`, then `STCS ASI_CTRLB=CCDETDIS`, `STCS ASI_CTRLA=IBDLY`, and `LDCS ASI_STATUSA`; attempts 1 and 2 (slow path, required on cold power-on) shall instead generate a multi-millisecond line-low BREAK by switching the kernel baud to 300 with one stop bit (`CSTOPB` cleared), writing two 0x00 bytes each followed by `tcdrain()` (each byte ≈33 ms line-low = ≥60 ms total — enough to reset the target's UPDI clock and contention detector) and consuming their half-duplex echoes, then waiting ≈50 ms for any in-flight line bytes to settle, performing `tcflush(TCIFLUSH)`, restoring session baud (`tcsetattr(TCSADRAIN)` + `tcflush(TCIFLUSH)`), and repeating the STCS+STCS+LDCS sequence. After a successful `LDCS ASI_STATUSA` the link probe is followed by a 32-byte System Information Block read (`SYNCH` + `0xE6`) which both confirms the link end-to-end and wakes any target left in UPDI SLEEP from a prior session — without this step a sleeping target accepts link-layer probes but rejects every memory access. After three consecutive failed attempts `updi_open()` shall return -1.
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
    The application shall respond to GDB register read (`g`) and write (`G`, `P`) packets, returning or updating the current AVR CPU register state (32 general-purpose registers, PC, SP, and status register) obtained from or written to the target via the AVR-Dx OCD register file at UPDI base `0x0F80`. Reads against the active GDB thread (or against any thread when no FSM context is established) shall return live OCD values; reads against a non-active virtual FSM thread shall return the synthesized register frame from the FSM mapper.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-015"></a>**HLR-015: Memory Read and Write.**
    The application shall respond to GDB memory read (`m`) and write (`M`, `X`) packets, mapping each request to the corresponding UPDI memory operation on the target. Requests addressing the FLASH region shall be routed to NVM read or write sequences as appropriate.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-016"></a>**HLR-016: Breakpoints.**
    The application shall implement code breakpoints by programming the two AVR-Dx OCD hardware comparators (`BP0`, `BP1`). Both `Z0` (software breakpoint) and `Z1` (hardware breakpoint) requests from GDB shall be routed to the same two hardware comparators, since installing the AVR `BREAK` opcode at runtime would require exiting OCD mode, entering NVMPROG (which resets the CPU and destroys live register/SREG/SP state), patching the FLASH page, and re-entering OCD — a sequence whose state-preservation cost outweighs the benefit on parts with only 32 KiB of FLASH per session. The shadow of the two comparator slots shall live in the RSP session context so that detach/reattach cycles leave silicon in a known state. Attempting to install a third breakpoint shall return GDB error reply `E08`; a duplicate insert at an already-installed address shall return `OK` without re-programming the comparator.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-017"></a>**HLR-017: Single-Step Execution.**
    The application shall respond to the GDB single-step (`s`/`S`) packet by executing exactly one AVR instruction on the target CPU via the UPDI step primitive and reporting the resulting stop reason.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-018"></a>**HLR-018: Continue Execution.**
    The application shall respond to the GDB continue (`c`/`C`) packet by resuming target CPU execution via `updi_run()` and then polling for a stop condition. While polling, the server shall remain responsive to the GDB Ctrl-C (`0x03`) async-interrupt byte on the client socket and shall halt the target via `updi_halt()` when one is received. The stop-reason packet shall report SIGINT (`T02`) when the halt was caused by Ctrl-C and SIGTRAP (`T05`) for every other halt cause (hardware breakpoint, BREAK opcode, single-step, external break). To bound recovery time on a flaky UPDI link, the run-poll loop shall give up after a fixed number of consecutive `updi_ocd_poll_halted()` failures and return GDB error reply `E01` rather than spinning indefinitely.
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
    The application shall parse the AVR ELF binary provided at launch to validate its architecture, locate the symbol table section, and build an index of avrOS system table symbols (`FSM_TABLE`, `QUE_TABLE`, `EVNT_TABLE`, and `currStateMachine`).
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
    The application shall implement a `monitor avros events` command that iterates the avrOS `EVNT_TABLE` and, for each registered named event, reads the current 1-byte status flag from SRAM via UPDI background read and returns a human-readable `<name>: <status>` listing to the GDB console, without halting the CPU core.
    *Trace:* [SDD Section 8.2.2](SDD.md), [SDD Section 8.3.1](SDD.md).

*   <a id="HLR-030"></a>**HLR-030: Monitor Queues Command.**
    The application shall implement a `monitor avros queues` command that iterates the avrOS `QUE_TABLE` and reports each registered queue's `capacity` and `sizeOfElement` from the FLASH descriptor via UPDI background read, returning a human-readable table to the GDB console without halting the CPU core.
    *Trace:* [SDD Section 8.2.2](SDD.md), [SDD Section 8.3.1](SDD.md).

*   <a id="HLR-032"></a>**HLR-032: Introspection Reliability.**
    The `monitor avros events` command shall accurately reflect the current event status flags across at least 10,000 consecutive invocations of `monitor_dispatch()` with the `avros events` sub-command without producing a UPDI protocol desync, a corrupted response, or a server crash.
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

## 9. Diagnostics and Bring-up

Requirements in this section govern operator-facing diagnostic features intended for hardware bring-up and connection troubleshooting. They are activated by explicit CLI flags and never run on the GDB session hot path.

*   <a id="HLR-044"></a>**HLR-044: Device-Signature Diagnostic Mode.**
    The server shall provide a one-shot diagnostic mode, activated by the `--device` command-line flag, that opens the UPDI link, reads the target SIGROW (`DEVICEID0..2`, 16-byte `SERNUM`), the SYSCFG.REVID byte, and the UPDI ASI status registers (`ASI_SYS_STATUS`, `ASI_KEY_STATUS`, `ASI_STATUSB`), prints a verbose human-readable report to `stdout`, and exits with status 0 on success or status 1 on any failure. In this mode the `<elf-file>` operand shall be optional, the GDB TCP listener shall not be bound, and no event loop shall be entered. Every failure shall produce a diagnostic on `stderr` that names the failed UPDI step (BREAK, SYNCH, SIGROW read, ASI read) so an operator can distinguish wiring, power, and fuse problems without external instrumentation. `--device` shall be mutually exclusive with `--load`; specifying both shall trigger a usage error and exit status 1.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.1](SDD.md), [SDD Section 4.1](SDD.md).

## 10. Hardware Integration Testing

Requirements in this section govern the on-target hardware integration test harness (`tests/hw/hw_test.c`). These tests link against the project's UPDI driver and exercise live AVR-Dx silicon over a serial UPDI link. They are manual-only and are explicitly excluded from `make test` so that unit-test execution remains hardware-independent and CI-safe.

*   <a id="HLR-046"></a>**HLR-046: Non-FLASH NVM Programming.**
    The application shall program the target's EEPROM, USERROW, FUSES, and (subject to HLR-047) LOCK memories from ELF `PT_LOAD` segments whose virtual addresses fall in the corresponding UPDI unified-address windows of the runtime-selected device (see HLR-048). For the AVR-DA reference target the windows are EEPROM `0x814000`–`0x8143FF` (512 B), USERROW `0x810080`–`0x8100FF` (32 B), FUSES `0x820000`–`0x82001F` (32 B), and LOCK `0x820040`–`0x820043` (4 B). The AVR-DB family shares the AVR-DA layout. The AVR-DD family widens USERROW to 128 B (`0x810080`–`0x8100FF` window, 128-byte page) and shrinks EEPROM to 256 B (`0x814000`–`0x8140FF`). The AVR-DU and AVR-SD families relocate USERROW to `0x811200`–`0x8113FF` (512 B) and likewise expose 256 B of EEPROM at `0x814000`–`0x8140FF`. FUSES and LOCK occupy the same windows on all five families. Each NVM kind shall use the byte-granular NVMCTRL command `EEERWR` (`0x13`) — erase-and-write per byte — and shall poll `NVMSTATUS.EEBUSY` (bit 1) clear between bytes. Segments whose virtual addresses fall in the SIGROW window (`0x811080`–`0x8110FF` on AVR-DA/DB/DD, `0x811080`–`0x8113FF` on AVR-DU/SD) shall be silently skipped because SIGROW is factory-programmed and read-only. Segments outside every programmable window for the selected device shall be rejected with a diagnostic. After all non-FLASH writes complete, the application shall re-enter OCD mode (`updi_enter_debug`) so the subsequent RSP session has a halted, OCD-addressable CPU. The AVR-EA and AVR-EB families are out of scope for this requirement because they use the incompatible NVMCTRLv3 controller (different command opcodes — FLPERW `0x05`, EEPERW `0x15`).
    *Trace:* [SDD Section 3.2.2](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-048"></a>**HLR-048: Multi-Family Runtime Device Dispatch.**
    The application shall maintain a compile-time table of supported AVR Dx/Du/Sd device families (AVR-DA, AVR-DB, AVR-DD, AVR-DU, AVR-SD) where each entry records the family name, the base address and size of every programmable NVM window (EEPROM, USERROW, FUSES, LOCK), the SIGROW base address, and a boolean flag indicating whether the family has been hardware-validated on real silicon. On startup, after `updi_open()` and before any NVM operation, the application shall select an active device descriptor by one of two paths: (a) if the user supplied `--force-device=<family>` on the command line, the named entry shall be selected by case-insensitive match and a warning shall be emitted on `stderr` when the entry's `hw_tested` flag is false; (b) otherwise the application shall read the 3-byte SIGROW DEVICEID0..2 triplet at the AVR-DA/DB/DD canonical address (`0x1100`) and look it up against the table's autodetect signatures, failing with a diagnostic that names all five supported families and the `--force-device` option when DEVICEID0 is not `0x1E` or the signature is unrecognised. The AVR-DU and AVR-SD families place SIGROW at `0x1080` rather than `0x1100`, so they cannot be auto-detected from the AVR-DA probe address and shall always be selected via `--force-device`. Once selected, all subsequent NVM read/write call sites and the ELF-VMA window classifier in `load_segments()` shall consult the runtime-selected descriptor for window bases and sizes; no hard-coded address constant shall be used for the per-family fields. AVR-DA is the only family currently validated against live silicon; AVR-DB/DD/DU/SD entries are declared from the published datasheet memory maps and are marked untested-on-silicon. The diagnostic device mode (`--device`) shall tolerate unknown silicon — when no `--force-device` override is supplied it shall not invoke the autodetect probe so the report can still display raw SIGROW bytes for triage.
    *Trace:* [SDD Section 3.2.2](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-047"></a>**HLR-047: Lockbit Programming Safety Interlock.**
    Writing the LOCK byte (UPDI window `0x820040`–`0x820043`) shall be subject to two mandatory safety interlocks. First, the application shall refuse to program the LOCK window unless `--erase` was also supplied on the command line, because the silicon requires a chip-erase to clear `LOCKSTATUS` before lockbits can be re-programmed and partial writes brick the part. Second, the application shall refuse any 4-byte LOCK payload other than the unlock pattern `0x5CC5C55C` (little-endian) unless the operator additionally supplies `--allow-lock-updi`, because every other 4-byte value risks setting the `UPDIDIS` lockbit which permanently disables the UPDI debug interface. When `LOCKSTATUS` (`ASI_SYS_STATUS` bit 1) is asserted at the time of the LOCK write, the application shall abort with the new error code `UPDI_ERR_LOCKED` (-3). With both interlocks satisfied (`--erase` present, payload is the unlock pattern OR `--allow-lock-updi` present), the application shall write the LOCK byte(s) using the same EEPROM `EEERWR` sequence as the other non-FLASH windows.
    *Trace:* [SDD Section 3.2.2](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-045"></a>**HLR-045: On-Target Hardware Integration Test Harness.**
    The project shall provide a standalone, manual-only on-target hardware integration test harness, separate from the unit-test suite, that validates the UPDI driver, device-info report, SRAM and FLASH memory access, and the GDB RSP server against live AVR-Dx silicon. The harness shall be organised into four opt-in groups: Group A (read-only link and device-info probes), Group B (non-destructive SRAM round-trips with size and address-mode variants), Group C (destructive FLASH erase/write/verify, gated by an explicit confirmation variable), and Group D (RSP server spawn-and-probe over TCP, gated by an opt-in variable). The harness shall be configurable via environment variables and command-line flags (serial port, expected device ID, SRAM probe address, FLASH page address, RSP TCP port, verbosity), and shall accept an explicit `--addr-24bit` switch to validate 24-bit UPDI addressing on parts whose FLASH lies above 64 KiB. The Makefile shall expose dedicated targets `hw-test` (Groups A+B), `hw-test-nvm` (Groups A+B+C), `hw-test-rsp` (Groups A+B+D), and `hw-test-all` (all groups); none of these targets shall ever be invoked by `make test`.
    *Trace:* [SDD Section 2.2](SDD.md), [SDD Section 4.1](SDD.md), [SDD Section 4.2.1](SDD.md).

## 11. CI-Grade Loader and Link Diagnostics

Requirements in this section govern the Phase 9 features that turn the loader into a CI-trustable build/flash step and elevate the `--device` mode into a first-line link-health probe. They were tracked as GitHub issue #28 and implement the design narrative from the Phase 9 overview paragraph in the SDD.

*   <a id="HLR-049"></a>**HLR-049: Read-Back Verify After Load.**
    After every successful `--load` write, the application shall re-read each programmed NVM range through the existing UPDI read path and compare the read-back bytes against the ELF payload. Verify shall cover the FLASH, EEPROM, USERROW, and FUSES windows; segments classified as SIGROW (read-only) or LOCK (read-once / write-only-after-erase) shall be skipped. On mismatch the application shall print one diagnostic line per failing page to `stderr` carrying the window name, page-aligned VMA, expected and actual IEEE 802.3 CRC32 values; shall not call `updi_enter_debug()`; shall not call `rsp_listen()`; and shall exit with status `UPDI_EXIT_VERIFY_FAIL` (`2`, distinct from the existing exit code `1` reserved for I/O / configuration failures). Verify shall be enabled by default for `--load` and `--prog`; the operator may disable it with the new `--no-verify` flag, in which case the application shall behave exactly as before Phase 9.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.2.2](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-050"></a>**HLR-050: Program-and-Exit Mode.**
    The application shall provide a new operating mode, activated by the `--prog` command-line flag and mutually exclusive with both `--device` and `--load`, that programs the supplied ELF into NVM, verifies the write per HLR-049, and exits — no GDB TCP listener shall be bound, no `updi_enter_debug()` shall be issued, and the target shall be left running with the UPDI link closed cleanly. On start-up the mode shall emit a single `baud=<N>` line on `stdout` reporting the negotiated UART rate. During programming the mode shall render an in-place single-line progress indicator (`[####....] %% page N/M phase window`) when `stdout` is a TTY (detected by `isatty(STDOUT_FILENO)`), and shall degrade to one line per page (or per phase transition) when `stdout` is piped or redirected. On verify success the mode shall print `verify: OK` and exit with status 0; on verify failure it shall exit with status `UPDI_EXIT_VERIFY_FAIL` (`2`); on any I/O or NVM failure it shall exit with status 1. Intended use is `avr-updi-gdb --prog <serial-device> <elf-file>` from a CI script or a Makefile `flash:` target.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.2.2](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-051"></a>**HLR-051: Fuses Pretty-Printer in Device Mode.**
    The `--device` diagnostic mode shall, after printing the SIGROW / REVID / ASI status report defined by HLR-044, read the target's full FUSES window and the 4-byte LOCK window through the UPDI NVM read path and emit a human-readable decode to `stdout`. The decode shall consist of a header line (`Fuses (<family>, <N> bytes):`), one indented line per known fuse byte showing the offset, fuse name, and raw hex value, followed by indented sub-lines naming each documented bit-field (e.g. `WDTCFG.PERIOD = 8K`, `BODCFG.SLEEP = SAMPLED`, `OSCCFG.CLKSEL = OSCHF`, `SYSCFG0.CRCSRC`, `SYSCFG1.SUT`, `CODESIZE`, `BOOTSIZE`) using human-readable enum names rather than raw hex. Fuse bytes for which no bit-field decoder is registered shall be shown as `(reserved) raw=0x..`. The lock byte shall be shown on a final line `Lock: 0xXXXXXXXX (UNLOCKED|LOCKED)`, where `UNLOCKED` is the 4-byte little-endian value `0x5CC5C55C` and any other value is reported as `LOCKED`. The decode tables shall live alongside `g_device_table[]` in `src/updi.c` so they are selected by the same `updi_select_device()` runtime path. Phase 9 ships the AVR-DA / AVR-DB family table; other families fall back to raw hex with no per-bit decoding.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.2.1](SDD.md).

*   <a id="HLR-052"></a>**HLR-052: Auto-Baud Link-Quality Probe in Device Mode.**
    The `--device` diagnostic mode shall, before opening the UPDI session, walk a fixed baud-rate ladder (`230400, 200000, 150000, 115200, 57600, 38400, 19200`) and at each rung open the UART at the candidate rate, issue a small fixed UPDI transaction sequence (`LDCS ASI_STATUSA`, repeated for a small sample window — default 8 samples), and record the byte error rate. Each rung result shall be reported to `stdout` as one line `  baud=<N> errors=<K>/<total>`. The application shall select the highest-rate rung that observed zero errors over the sample window; if no rung observes zero errors the probe shall report failure and the application shall fall back to the user-supplied or default baud rate. The probe shall be read-only — no NVM, no fuses, no system-reset paths, no `updi_enter_debug()` — and shall close every interim UART file descriptor it opens. The operator may disable the probe with the new `--no-autobaud` flag, in which case `--device` shall behave exactly as before Phase 9 and use `cfg.baud_rate` directly. The probe is invoked from `--device` mode only; `--load` and `--prog` continue to use the explicit `--baud` setting.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.2.1](SDD.md).
