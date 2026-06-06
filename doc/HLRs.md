# High-Level Requirements

**Version:** 0.1
**Date:** 2026-05-16
**Author(s):** John Anderson

## 1. System Startup and Command-Line Interface

Requirements in this section govern how `avrOSdb` is invoked, how it initialises its resources, and how quickly it becomes ready for a GDB connection.

*   <a id="HLR-068"></a>**HLR-068: Detailed RSP Traffic Logging.**
    The application shall accept a `--log-rsp` command line flag that enables detailed logging of all GDB Remote Serial Protocol requests and responses to `stderr`. It shall be parsed via `--log-rsp` and set an internal logging flag. When this flag is enabled, every RSP packet received from or sent to the GDB client shall be printed to `stderr` prefixed with `RSP &lt; ` or `RSP &gt; ` respectively.
    *Trace:* [SDD Section 1.1](SDD.md), [SDD Section 3.2.2](SDD.md).

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
    The application shall respond to GDB register read (`g`) and write (`G`, `P`) packets, returning or updating the current AVR CPU register state (32 general-purpose registers, PC, SP, and status register) obtained from or written to the target via the AVR-Dx OCD register file at UPDI base `0x0F80`. All register reads shall return live OCD values: the live CPU is the sole GDB thread (avrOS FSM state is exposed via introspection, HLR-029 et seq., not as GDB threads).
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-015"></a>**HLR-015: Memory Read and Write.**
    The application shall respond to GDB memory read (`m`) and write (`M`, `X`) packets, mapping each request to the corresponding UPDI memory operation on the target. Requests addressing the FLASH region shall be routed to NVM read or write sequences as appropriate.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-016"></a>**HLR-016: Breakpoints.**
    The application shall implement code breakpoints by programming the two AVR-Dx OCD hardware comparators (`BP0`, `BP1`). Both `Z0` (software breakpoint) and `Z1` (hardware breakpoint) requests from GDB shall be routed to hardware comparator `BP0`. Comparator `BP1` is reserved internally as a workaround for the AVR-Dx hardware 32-bit stepping errata. Installing the AVR `BREAK` opcode at runtime would require exiting OCD mode, entering NVMPROG (which resets the CPU and destroys live register/SREG/SP state), patching the FLASH page, and re-entering OCD — a sequence whose state-preservation cost outweighs the benefit on parts with only 32 KiB of FLASH per session. The shadow of the comparator slots shall live in the RSP session context so that detach/reattach cycles leave silicon in a known state. Attempting to install a second breakpoint shall return GDB error reply `E08`; a duplicate insert at an already-installed address shall return `OK` without re-programming the comparator.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-017"></a>**HLR-017: Single-Step Execution.**
    The application shall respond to the GDB single-step (`s`/`S`) packet by executing exactly one AVR instruction on the target CPU and reporting the resulting stop reason. Due to the AVR-Dx hardware 32-bit stepping errata, if the instruction at the current PC is determined to be a 32-bit instruction (e.g. `CALL`, `JMP`, `LDS`, `STS`), the application shall bypass the native UPDI hardware stepper, calculate the target PC of the subsequent instruction, plant a temporary hardware breakpoint there, `RUN` the CPU, and await the halt. For 16-bit instructions, it shall use the native UPDI step primitive.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-018"></a>**HLR-018: Continue Execution.**
    The application shall respond to the GDB continue (`c`/`C`) packet by resuming target CPU execution via `updi_run()` and then polling for a stop condition. Before the free-run, if the instruction at the resume PC is a direct 32-bit `CALL` or `JMP`, the application shall emulate that change-of-flow over OCD (push the return address for `CALL`, set the OCD PC to the branch target) so the AVR-Dx OCD "first change-of-flow after RUN" errata (issue #40) does not run straight through the callee and silently skip every breakpoint reached inside it. While polling, the server shall remain responsive to the GDB Ctrl-C (`0x03`) async-interrupt byte on the client socket and shall halt the target via `updi_halt()` when one is received. The stop-reason packet shall report SIGINT (`T02`) when the halt was caused by Ctrl-C and SIGTRAP (`T05`) for every other halt cause (hardware breakpoint, BREAK opcode, single-step, external break). To bound recovery time on a flaky UPDI link, the run-poll loop shall give up after a fixed number of consecutive `updi_ocd_poll_halted()` failures and return GDB error reply `E01` rather than spinning indefinitely.
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

## 5. FSM State Snapshot (Introspection)

Requirements in this section govern how the server reads the avrOS cooperative scheduler state into an internal snapshot for developer introspection (surfaced via `monitor` commands, HLR-029 et seq.). avrOS FSMs are NOT presented as GDB virtual threads -- the live CPU is the sole GDB thread.

*   <a id="HLR-024"></a>**HLR-024: FSM Thread Enumeration.**
    Upon halting the target, the application shall read the avrOS FSM registration table from target SRAM via UPDI into an internal snapshot (per-FSM name, active flag, and current-state function pointer). The snapshot is exposed to the developer through introspection commands (HLR-029 et seq.) and is NOT presented as GDB virtual threads — the live CPU is the sole GDB thread.
    *Trace:* [SDD Section 7.1](SDD.md), [SDD Section 7.3.1](SDD.md).

*   <a id="HLR-025"></a>**HLR-025: Active Thread Identification.**
    The application shall identify the currently executing avrOS FSM from the runtime state read via UPDI and mark it as active in the introspection snapshot (HLR-024). All GDB register reads and memory operations target the single live-CPU thread; the active-FSM identification is surfaced via introspection, not as a GDB active-thread selection.
    *Trace:* [SDD Section 7.3.1](SDD.md).

## 6. System Introspection

Requirements in this section govern the custom `monitor avros` commands that expose avrOS runtime object state to the developer without halting the target CPU.

*   <a id="HLR-029"></a>**HLR-029: Monitor Events Command.**
    The application shall implement a `monitor avros events` command that iterates the avrOS `EVNT_TABLE` and, for each registered named event, reads the current 1-byte status flag from SRAM via UPDI background read and returns a human-readable `<name>: <status>` listing to the GDB console, without halting the CPU core.
    *Trace:* [SDD Section 8.2.2](SDD.md), [SDD Section 8.3.1](SDD.md).

*   <a id="HLR-030"></a>**HLR-030: Monitor Queues Command.**
    The application shall implement a `monitor avros queues` command that iterates the avrOS `QUE_TABLE` and reports each registered queue's `capacity` and `sizeOfElement` from the FLASH descriptor via UPDI background read, returning a human-readable table to the GDB console without halting the CPU core.
    *Trace:* [SDD Section 8.2.2](SDD.md), [SDD Section 8.3.1](SDD.md).

*   <a id="HLR-069"></a>**HLR-069: Monitor Tasks Command.**
    The application shall implement a `monitor avros tasks` command that lists each registered avrOS FSM with an active marker, its name, and its current state name, reading the FSM registration table and each FSM's `currStateName` via UPDI background reads (reusing `fsm_build_thread_list()`), without halting the CPU. avrOS FSMs surface as introspection here, not as GDB threads.
    *Trace:* [SDD Section 8.2.2](SDD.md), [SDD Section 8.3.1](SDD.md).

*   <a id="HLR-032"></a>**HLR-032: Introspection Reliability.**
    The `monitor avros events` command shall accurately reflect the current event status flags across at least 10,000 consecutive invocations of `monitor_dispatch()` with the `avros events` sub-command without producing a UPDI protocol desync, a corrupted response, or a server crash.
    *Trace:* [SDD Section 4.3.1](SDD.md), [SDD Section 8.1](SDD.md).

## 7. Platform and Build

Requirements in this section govern the host platforms the server must support and the constraints on its runtime dependencies.

*   <a id="HLR-033"></a>**HLR-033: Native Linux and macOS Build.**
    The application shall compile and execute natively on Linux (x86-64 and ARM) and macOS (x86-64 and Apple Silicon) using a C99-conforming toolchain plus the **elfutils** development libraries (libelf and libdw), which are a required dependency (`src/elf_parser.c` is a thin adapter over them). On Linux these come from `libelf-dev`/`libdw-dev` (or `elfutils-devel`); on macOS from `brew install elfutils`. The build shall not require any platform-specific preprocessor workarounds beyond what the C99 standard, POSIX.1-2008, and the elfutils headers provide; in particular, no bundled ELF type shim is used (elfutils supplies the system `<elf.h>`).
    *Trace:* [SDD Section 2.2](SDD.md).

*   <a id="HLR-034"></a>**HLR-034: Lean Host Runtime Dependencies.**
    The application runtime shall depend only on the host C standard library, POSIX serial and socket APIs, and the **elfutils** shared libraries (libelf and libdw, together with the compression backends libdw links to read compressed `.debug` sections: libz, libzstd, liblzma, libbz2). No Java runtime, Python interpreter, Electron-based framework, or other heavyweight dependency shall be required to launch or operate the server.
    *Trace:* [SDD Section 2.2](SDD.md).

*   <a id="HLR-039"></a>**HLR-039: Single-Threaded Event Loop Architecture.**
    The server shall multiplex all I/O using a single POSIX `select()`-based event loop with no POSIX threads. All module entry points shall be called synchronously from the event loop and shall return to it promptly. Long-blocking operations (NVM flash programming and UPDI link initialisation) shall be permitted only during server startup or in direct response to an explicit GDB command, and shall not block the event loop in any other context.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 2.2](SDD.md), [SDD Section 3.3.2](SDD.md).

*   <a id="HLR-040"></a>**HLR-040: Bounded Heap Allocation.**
    The server shall not allocate heap memory on any hot path (packet processing, UPDI reads, FSM enumeration, monitor commands). The application itself shall perform no `malloc` of the ELF symbol/string tables; ELF/DWARF parsing memory is owned internally by the elfutils handles (the libelf `Elf*` and libdw `Dwarf*` opened in `elf_open()`), is bounded by the size of the ELF binary, and is allocated at most once per GDB session. All such resources shall be released by `elf_close()` (via `dwarf_end()` / `elf_end()` and `close(fd)`) at session end or on error.
    *Trace:* [SDD Section 2.2](SDD.md), [SDD Section 9](SDD.md).

## 8. Installation and Documentation

Requirements in this section govern the Makefile installation targets and the user-facing documentation deliverables that accompany the compiled binary.

*   <a id="HLR-041"></a>**HLR-041: Makefile Install and Uninstall Targets.**
    The Makefile shall provide `install` and `uninstall` targets that accept a `PREFIX` variable (defaulting to `/usr/local`). The `install` target shall copy the compiled `avrOSdb` binary to `$(PREFIX)/bin/` and the man page to `$(PREFIX)/share/man/man1/`. The `uninstall` target shall remove exactly those files. A `check-tools` target shall verify that all required host tools (`gcc`, `make`, `avr-gcc`, `avr-nm`) are available before any build step, printing a diagnostic and exiting non-zero if any required tool is absent.
    *Trace:* [SDD Section 2.2](SDD.md).

*   <a id="HLR-042"></a>**HLR-042: User Manual and Unix Man Page.**
    The project shall provide a user manual (`doc/UserManual.md`) and a Unix man page (`doc/avrOSdb.1`). The man page shall be parseable by the standard `man` utility and shall document the command synopsis, all options, operands, exit codes, and at least one usage example. The `doc/UserManual.md` shall document prerequisites, build instructions, usage, CLI options, and connection wiring for the UPDI adapter.
    *Trace:* [SDD Section 2.2](SDD.md).

*   <a id="HLR-043"></a>**HLR-043: Distribution Package Bundle.**
    The Makefile shall provide a `bundle` target that produces native distribution packages for three target platforms: a Debian binary package (`dist/avrOSdb_$(VERSION)_amd64.deb`) for Debian/Ubuntu Linux, an RPM binary package (`dist/avrOSdb-$(VERSION)-1.x86_64.rpm`) for Red Hat/Fedora Linux, and a Homebrew formula (`dist/avrOSdb.rb`) for macOS. Each package shall include the `avrOSdb` binary and the man page. All output artefacts shall be written under the `dist/` directory. A `VERSION` variable (defaulting to the value extracted from `git describe`) shall parameterise every package version string.
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

*   <a id="HLR-070"></a>**HLR-070: Interactive Debug-Session Acceptance Suite.**
    The on-target Group-G GDB acceptance harness (`tests/hw/gdb_acceptance.py`, driven by `make hw-test-gdb`) shall include a suite of cases that emulate a complete *interactive* debug session against live AVR-Dx silicon — exercising the combinations a developer drives by hand in an IDE (VS Code Cortex-Debug / `cppdbg`) rather than the isolated protocol primitives covered by the earlier Group-G cases. The suite shall run against a dedicated deterministic bare-metal fixture (`tests/fixtures/gdb_debug_session.c`) built at `-O0` with an explicit four-deep call chain `main -> top -> mid -> leaf`, `noinline` functions, scalar / struct / array globals plus a constant marker, and per-frame parameters and locals whose first-hit values are fixed constants; the fixture's known first-hit values are `leaf(7, 2)` returning `49397`, `leaf(7, 3)` returning `49405`, `g_marker = 49374`, `g_cfg = { base = 100, gain = -7 }`, and `g_arr = { 10, 20, 30, 40 }`. The suite shall verify: (a) a multi-frame backtrace that reaches `main` in correct frame order, plus frame selection (`frame N`, `up`, `down`) with per-frame `info args`; (b) source-level `step` (into a call), `next` (over a call), and `finish` (out of a frame) reporting the correct returned value; (c) multiple simultaneous breakpoints hit in call order (`top` then `mid` then `leaf`); (d) a conditional breakpoint (`break leaf if b == 3`) that skips the non-matching `leaf(7, 2)` call and stops only at `leaf(7, 3)`; (e) global reads of a scalar, a struct, and an array, including specific struct fields and array elements; (f) per-frame `info locals` and `info args` reading back the exact constants the fixture assigns; and (g) one capstone case that drives a realistic session start to finish. The fixture shall be flashed via the server's `--load` path and selected through a new `--dbg-elf` / `HW_GDB_DBG_ELF` knob; breakpoints shall be set by symbol (never by line number) so the suite is robust to edits in the fixture. The suite shall remain manual-only and shall never be wired into `make test`.
    *Trace:* [SDD Section 5.3.1](SDD.md), [SDD Section 4.1](SDD.md).

*   <a id="HLR-071"></a>**HLR-071: ELF and DWARF Parsing via elfutils.**
    ELF and DWARF parsing shall be delegated to the **elfutils** libraries — libelf (accessed through the class-independent GElf API) for the ELF container and libdw for DWARF — rather than hand-decoded from raw byte layouts. `src/elf_parser.c` shall be a thin adapter that holds only the AVR-Dx/avrOS domain logic (avrOS system-table symbol discovery, the mapped-flash VMA→LMA translation, word-vs-byte flash addressing, and the Microchip deviceinfo note) on top of those libraries, so that ELF/DWARF format changes are absorbed by elfutils. The bundled ELF type shim (`src/elf.h`) is removed; ELF type definitions come from the system `<elf.h>` that elfutils provides. The module's public C API (`elf_open`, `elf_close`, `elf_find_avros_tables`, `elf_has_fsm_symbols`, `elf_flash_addr`, `elf_phys_flash_byte_addr`) and the `ElfContext` / `AvrOsSymbolIndex` contract consumed by `fsm_mapper` and `main` shall be preserved, with the libelf `Elf*` and libdw `Dwarf*` handles held as opaque `void*` so consumers need no elfutils headers.
    *Trace:* [SDD Section 6.1](SDD.md), [SDD Section 6.3.1](SDD.md).

*   <a id="HLR-072"></a>**HLR-072: DWARF Source-Level Lookup (DAP Groundwork).**
    The ELF module shall expose DWARF-backed source-level lookup so a future native Debug Adapter Protocol (DAP) front-end can map between machine addresses and source locations without delegating to `avr-gdb` (the GDB-RSP path does not use it). It shall provide: `elf_dwarf_available()` (returns 1, since elfutils is a required dependency); `elf_addr_to_line()` mapping a code byte address to its source file and 1-based line via libdw; and `elf_line_to_addr()` resolving a `file:line` (matched by basename) to the first code byte address. The accessors shall be safe to call when the ELF carries no debug info — returning -1 and leaving outputs untouched — and shall never abort the session. This requirement lands the source-mapping capability only; the DAP listener and request handlers are a later phase.
    *Trace:* [SDD Section 6.3.1](SDD.md).

## 11. CI-Grade Loader and Link Diagnostics

Requirements in this section govern the Phase 9 features that turn the loader into a CI-trustable build/flash step and elevate the `--device` mode into a first-line link-health probe. They were tracked as GitHub issue #28 and implement the design narrative from the Phase 9 overview paragraph in the SDD.

*   <a id="HLR-049"></a>**HLR-049: Read-Back Verify After Load.**
    After every successful `--load` write, the application shall re-read each programmed NVM range through the existing UPDI read path and compare the read-back bytes against the ELF payload. Verify shall cover the FLASH, EEPROM, USERROW, and FUSES windows; segments classified as SIGROW (read-only) or LOCK (read-once / write-only-after-erase) shall be skipped. On mismatch the application shall print one diagnostic line per failing page to `stderr` carrying the window name, page-aligned VMA, expected and actual IEEE 802.3 CRC32 values; shall not call `updi_enter_debug()`; shall not call `rsp_listen()`; and shall exit with status `UPDI_EXIT_VERIFY_FAIL` (`2`, distinct from the existing exit code `1` reserved for I/O / configuration failures). Verify shall be enabled by default for `--load` and `--prog`; the operator may disable it with the new `--no-verify` flag, in which case the application shall behave exactly as before Phase 9.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.2.2](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-050"></a>**HLR-050: Program-and-Exit Mode.**
    The application shall provide a new operating mode, activated by the `--prog` command-line flag and mutually exclusive with both `--device` and `--load`, that programs the supplied ELF into NVM, verifies the write per HLR-049, and exits — no GDB TCP listener shall be bound, no `updi_enter_debug()` shall be issued, and the target shall be left running with the UPDI link closed cleanly. On start-up the mode shall emit a single `baud=<N>` line on `stdout` reporting the negotiated UART rate. During programming the mode shall render an in-place single-line progress indicator (`[####....] %% page N/M phase window`) when `stdout` is a TTY (detected by `isatty(STDOUT_FILENO)`), and shall degrade to one line per page (or per phase transition) when `stdout` is piped or redirected. On verify success the mode shall print `verify: OK` and exit with status 0; on verify failure it shall exit with status `UPDI_EXIT_VERIFY_FAIL` (`2`); on any I/O or NVM failure it shall exit with status 1. Intended use is `avrOSdb --prog <serial-device> <elf-file>` from a CI script or a Makefile `flash:` target.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.2.2](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-051"></a>**HLR-051: Fuses Pretty-Printer in Device Mode.**
    The `--device` diagnostic mode shall, after printing the SIGROW / REVID / ASI status report defined by HLR-044, read the target's full FUSES window and the 4-byte LOCK window through the UPDI NVM read path and emit a human-readable decode to `stdout`. The decode shall consist of a header line (`Fuses (<family>, <N> bytes):`), one indented line per known fuse byte showing the offset, fuse name, and raw hex value, followed by indented sub-lines naming each documented bit-field (e.g. `WDTCFG.PERIOD = 8K`, `BODCFG.SLEEP = SAMPLED`, `OSCCFG.CLKSEL = OSCHF`, `SYSCFG0.CRCSRC`, `SYSCFG1.SUT`, `CODESIZE`, `BOOTSIZE`) using human-readable enum names rather than raw hex. Fuse bytes for which no bit-field decoder is registered shall be shown as `(reserved) raw=0x..`. The lock byte shall be shown on a final line `Lock: 0xXXXXXXXX (UNLOCKED|LOCKED)`, where `UNLOCKED` is the 4-byte little-endian value `0x5CC5C55C` and any other value is reported as `LOCKED`. The decode tables shall live alongside `g_device_table[]` in `src/updi.c` so they are selected by the same `updi_select_device()` runtime path. Phase 9 ships the AVR-DA / AVR-DB family table; other families fall back to raw hex with no per-bit decoding.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.2.1](SDD.md).

*   <a id="HLR-052"></a>**HLR-052: Auto-Baud Link-Quality Probe in Device Mode.**
    The `--device` diagnostic mode shall, before opening the UPDI session, walk a fixed baud-rate ladder (`230400, 200000, 150000, 115200, 57600, 38400, 19200`) and at each rung open the UART at the candidate rate, issue a small fixed UPDI transaction sequence (`LDCS ASI_STATUSA`, repeated for a small sample window — default 8 samples), and record the byte error rate. Each rung result shall be reported to `stdout` as one line `  baud=<N> errors=<K>/<total>`. The application shall select the highest-rate rung that observed zero errors over the sample window; if no rung observes zero errors the probe shall report failure and the application shall fall back to the user-supplied or default baud rate. The probe shall be read-only — no NVM, no fuses, no system-reset paths, no `updi_enter_debug()` — and shall close every interim UART file descriptor it opens. The operator may disable the probe with the new `--no-autobaud` flag, in which case `--device` shall behave exactly as before Phase 9 and use `cfg.baud_rate` directly. The probe is invoked from `--device` mode only; `--load` and `--prog` continue to use the explicit `--baud` setting.
    *Trace:* [SDD Section 2.1](SDD.md), [SDD Section 3.2.1](SDD.md).

## 12. GDB Protocol Completion (avarice Feature Parity)

Requirements in this section govern the Phase 10 features that bring `avrOSdb`'s GDB Remote Serial Protocol surface up to functional parity with the legacy `avarice` JTAG/dW stub: equivalent monitor verbs, equivalent `(gdb) load`, equivalent watchpoint and software-breakpoint experience, and equivalent extended-remote lifecycle. Wire-level drop-in compatibility with `avarice`-specific scripts is an explicit non-goal. Tracked as GitHub issue #31; implements the design narrative from the Phase 10 overview paragraph in the SDD.

*   <a id="HLR-053"></a>**HLR-053: Flash Programming via GDB load.**
    The application shall implement the GDB `vFlashErase:addr,length`, `vFlashWrite:addr:XX...`, and `vFlashDone` packets so that the standard `(gdb) load` command programs the AVR FLASH directly from the running GDB session without exiting to an external programmer. On `vFlashErase` the server shall reject any address range that falls outside the FLASH window of the runtime-selected device (HLR-048) with reply `E22`; ranges that fall in EEPROM, USERROW, FUSES, LOCK, or SIGROW shall be refused for the same reason because `gdb load` carries no per-window verb to disambiguate. Erase shall be performed page-aligned via `updi_nvm_erase_flash_page()`; `vFlashWrite` shall accumulate the binary payload into the corresponding page buffer and program it with `updi_nvm_write_flash()`; `vFlashDone` shall flush any pending page, call `updi_enter_debug()` to leave the CPU halted at reset, and reply `OK`. The session's hardware-breakpoint shadow (`RspContext.hw_bp_addr[]`) and SW-breakpoint shadow (HLR-054) shall be cleared on `vFlashDone` because their underlying instructions may have been overwritten by the load. While a `vFlash*` transaction is in progress the server shall not service `g`/`G`/`m`/`M`/`c`/`s` packets; if such a packet arrives between `vFlashErase` and `vFlashDone` the server shall reply `E22` and discard the in-progress page buffers.
    *Trace:* [SDD Section 5.3.1](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-054"></a>**HLR-054: True Software Breakpoints via Flash BREAK Opcode.**
    The application shall support an unbounded number of simultaneous software breakpoints by patching the AVR `BREAK` opcode (`0x9598`, little-endian) into the FLASH word at the target address on `Z0` and restoring the original 2-byte instruction on `z0`. A per-session shadow map shall record `(gdb_addr, flash_byte_addr, original_opcode_lo, original_opcode_hi, page_aligned_base)` for every active SW breakpoint so removal is exact. The patch sequence shall preserve live CPU state across the unavoidable OCD → NVMPROG → OCD transition: before NVMPROG entry the server shall snapshot R0–R31, SREG, SP, and PC via the OCD register file; after re-entering OCD it shall restore them with the existing `updi_ocd_write_*` primitives. SW breakpoints in the SIGROW, FUSES, USERROW, EEPROM, or LOCK windows shall be refused with `E22` because the AVR `BREAK` opcode is only meaningful when fetched as an instruction. The operator may opt out of true SW breakpoints with `monitor avros bp-mode hw-only` (HLR-055), in which case `Z0` shall continue to alias to the two HW comparators (the Phase 1–8 behaviour mandated by HLR-016); the mode shall persist across detach/reattach for the lifetime of the server process. On `vFlashDone` (HLR-053) and on `monitor reset` / `monitor chip-erase` (HLR-055) every SW breakpoint shadow entry shall be discarded because the underlying instruction is no longer present in FLASH.
    *Trace:* [SDD Section 5.3.1](SDD.md), [SDD Section 4.3.1](SDD.md).

*   <a id="HLR-055"></a>**HLR-055: avarice-Compatible Monitor Commands.**
    The application shall expose a top-level set of `monitor` verbs that mirror the `avarice` command surface, so that GDB scripts written for `avarice` work unchanged: `monitor reset` (issue `updi_reset()` and leave the CPU halted at the reset vector, then invalidate the FSM snapshot cache so the next introspection read reflects post-reset state), `monitor halt` (call `updi_halt()` and reply with the same `T05thread:<id>;` packet as a step boundary), `monitor go` (call `updi_run()` and reply `OK` — the subsequent stop-reason packet is emitted when the CPU next halts), `monitor erase` and `monitor chip-erase` (issue `updi_chip_erase()`, but only when the server was launched with the new `--allow-erase` flag — without that flag the verb shall reply with an O-packet diagnostic and `E22`), `monitor version` (one O-packet line carrying the server version, git short SHA, build date, and the active device family name from HLR-048), `monitor bp-mode <hw-only|sw>` (configure the breakpoint installation policy per HLR-054), and `monitor help` (one O-packet per recognised verb summarising syntax and side-effects, terminating with `OK`). The dispatcher in `src/monitor.c` shall continue to route the `avros ` prefix to the existing avrOS-specific handlers (`events`, `queues`); unknown verbs shall reply with the existing usage hint plus `E22`. None of the new verbs shall alter the FSM snapshot path other than via the explicit cache invalidation noted above.
    *Trace:* [SDD Section 8.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-056"></a>**HLR-056: Hardware Data Watchpoints.**
    The application shall reply with the empty RSP packet (`$#00`) to the GDB watchpoint requests `Z2`/`z2` (write), `Z3`/`z3` (read), and `Z4`/`z4` (access). The empty reply is the documented RSP signal for "feature unsupported" and causes GDB to fall back transparently to software watchpoints (single-step + memory poll), so the user-visible `watch <expr>` command continues to work. The empty-packet behaviour reflects a silicon constraint: the AVR-Dx OCD exposed over UPDI provides only the program-counter breakpoint comparators (`OCD_BP0A` / `OCD_BP1A`) and does NOT expose any data-address comparator hardware. The conclusion is confirmed by four independent sources: (1) the FF-bomb experiment in `doc/reference/guesswork.md`, which finds writable bits only at `BP0A`, `BP1A`, `CTRL0`, `CTRL1`, `STATUS0/1`, `INSN0/1`, `PC`, `SP`, `SREG`, and the register file; (2) the canonical open-source AVR debugger Bloom, whose UPDI driver declares `hardwareBreakpoints=1` and exposes no `setDataWatchpoint` method; (3) the sibling SerialUPDI debugger `feline-felicity/avr-absurd`, whose feature list documents "Two hardware breakpoints" and no watchpoints; and (4) Microchip's own `mraardvark/pyavrdebug` (CMSIS-DAP / Atmel-ICE stack), whose Z-packet handler explicitly replies the empty packet for Z2/Z3/Z4 — i.e. even the proprietary EDBG protocol over a debugger pod does not expose data watchpoints. The server shall not advertise any watchpoint-related capability in its `qSupported` reply.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-058"></a>**HLR-058: Extended-Remote Lifecycle Packets.**
    The application shall support the `target extended-remote` connection mode by implementing the `vRun;<arg>;<arg>...` packet (treat the first argument as a path to a new `.elf` file, swap it into the existing `AppConfig`, re-run ELF parsing, re-build the FSM context, and reply with a `T05` stop at the new reset vector), the `vAttach;<pid>` packet (reply `OK` followed by the standard stop-reply; `pid` is informational only on a single-target stub), and the `vKill;<pid>` packet (call `updi_run()` to release the target, close the GDB client fd, and return to the listen state without exiting the server process). Existing `D` (detach) and `k` (kill) handlers shall remain operational; `k` shall continue to mean process-exit so that scripts that hard-stop the server keep working. The `qSupported` feature string shall be extended with `vRun+;vAttach+;vKill+`. The server shall **not** advertise `multiprocess+`: it presents a single GDB thread (the live CPU), so multiprocess thread-id forms are unnecessary, and avrOS FSM state is exposed via introspection (`monitor avros tasks`, HLR-029 et seq.) rather than as GDB threads.
    *Trace:* [SDD Section 5.1](SDD.md), [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-059"></a>**HLR-059: Protocol Cleanup — qC, qOffsets, T, R.**
    The application shall stop replying with the empty packet for the standard small queries that `avarice` answers but Phase 1–8 leaves unimplemented, because some IDE configurations promote the resulting `unsupported feature` warnings to fatal errors. Specifically: `qC` shall return `QC<tid>` where `<tid>` is the currently selected `c`-thread (or `0` when no thread is selected); `qOffsets` shall return `Text=0;Data=0;Bss=0` because the AVR reset vector is fixed at address zero and no position-independent loading is performed; `T<tid>` (is-thread-alive) shall return `OK` for the single live-CPU thread (TID 1) and `E01` otherwise; and `R<XX>` (RCmd restart) shall behave as `monitor reset` followed by `c` and reply with the resulting stop packet. None of these handlers shall alter execution state beyond the explicit semantics above.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-064"></a>**HLR-064: vKill Listener Survival.**
    The `vKill[;<pid>]` packet shall close the GDB client connection but shall not terminate the avrOSdb server process. After replying `OK` and closing the client `fd`, `event_loop()` shall return to its `select()` loop and continue listening on the configured TCP port for a new GDB session. This makes avrOSdb honest about the GDB extended-remote contract: `vKill` ends the *debug session*, not the *debugger*, so a subsequent F5 / `target extended-remote :1234` from the same operator shall succeed without restarting the server. Concretely, `dh_vkill()` shall: (a) call `updi_halt()` on the target so the next session attaches to a stopped CPU; (b) release both AVR-Dx OCD hardware-breakpoint comparators via `rsp_hw_bp_clear_all()`; (c) drop the per-session software-breakpoint shadow via `rsp_sw_bp_clear_all()` (FLASH BREAK rewrite is not required — the shadow is purely host-side bookkeeping per LLR-RSP-30 et seq.); (d) reply `OK`; (e) record `ctx->disconnect_reason = "vKill"` for the lifecycle logger (HLR-065); (f) close the GDB socket via `*ctx->gdb_fd_p` and set the pointed-at fd to `-1`. The handler shall **not** set `*ctx->quit_p = 1`. The `--prog` mode (HLR-019/Phase 8) is unaffected: it never opens the GDB listener and never reaches `dh_vkill()`. The pre-Phase-11 legacy `k` packet path (`dh_detach` with `pkt[0] == 'k'`) is preserved and still sets `quit_p = 1` so existing operators relying on `kill` to shut the server down retain that behaviour.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-062"></a>**HLR-062: swbreak/hwbreak Stop-Cause Tags.**
    The application shall classify every halt and tag the resulting `T05` stop reply with the GDB Remote Serial Protocol stop-cause field that names the cause, so that `avr-gdb` (and front-ends layered on it, in particular VS Code `cppdbg`) can distinguish a software breakpoint hit from a hardware comparator hit from a single-step completion from a host-issued interrupt without a target-side guess. Concretely: (a) the `qSupported` reply shall advertise `swbreak+;hwbreak+` in addition to the existing capability set; (b) a new `enum RspStopCause` shall enumerate `SC_NONE`, `SC_SWBREAK`, `SC_HWBREAK`, `SC_STEP`, `SC_INTR`; (c) every default handler that emits a `T<sig>thread:<tid>;` stop reply (`dh_halt_reason`, the post-`vCont;c`/`c` halt path inside `dh_continue`, `dh_step`, and the indirect emitters `dh_vrun` and `dh_vattach` which delegate to `dh_halt_reason`) shall route through a single formatter that, after the `T<sig>` and before the `thread:` field, inserts `swbreak:;` when the cause is `SC_SWBREAK` and `hwbreak:;` when the cause is `SC_HWBREAK`; `SC_STEP`, `SC_INTR`, and `SC_NONE` shall produce no extra tag (the existing bare `T05`/`T02` semantics). Classification rules: read the live PC via `updi_ocd_read_pc()` after the halt has settled; if the PC matches any in-use `RspContext.sw_bp[i].addr` slot the cause is `SC_SWBREAK`; otherwise if the PC matches either `RspContext.hw_bp_addr[i]` slot the cause is `SC_HWBREAK`; otherwise the cause is `SC_INTR` when the local `got_ctrl_c` flag is set inside `dh_continue` (or when `OCD_STATUS1_EXTBRK` is set as observed by `signal_for_halt_status()`), `SC_STEP` when the halt arrived via `dh_step()`, and `SC_NONE` for the bare `?` probe. The classifier shall not adjust PC — GDB performs the BREAK-instruction PC rewind itself when it sees `swbreak:;`. The classifier shall preserve the layered architecture (SDD §2.2): it lives entirely in `src/gdb_rsp.c` and reads silicon only via the existing `updi_ocd_*` helpers. Backward compatibility: the tag is informational; older `avr-gdb` releases that do not advertise `swbreak+`/`hwbreak+` in their own `qSupported` ignore the extra field per the GDB RSP spec, so emission can remain unconditional.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-065"></a>**HLR-065: Lifecycle Logging.**
    The avrOSdb server shall emit one human-readable line per significant lifecycle transition to `stderr`, so that a developer running the server in a terminal can see at a glance when the GDB listener becomes ready, when a client attaches or detaches, and why the server is shutting down. The line set is:

    * `avrOSdb: listening on :<port>` — emitted exactly once, after `rsp_listen()` succeeds and immediately before the first `select()` call in `event_loop()`.
    * `avrOSdb: client connected from <ip>:<port>` — emitted after every successful `rsp_accept()`. The peer address is taken from `getpeername()` on the accepted socket; on `getpeername()` failure the line shall instead read `client connected from <unknown>`.
    * `avrOSdb: client disconnected (<reason>)` — emitted whenever the client `fd` transitions from open to closed. `<reason>` shall be one of:
      - `D` — the GDB `D` (detach) packet was processed and the protocol layer closed the socket;
      - `vKill` — the GDB `vKill` packet was processed; per HLR-064 this closes the client socket and returns to the `accept()` loop without exiting the server;
      - `EOF` — `rsp_recv_packet()` returned 0 (peer half-closed);
      - `read error: <errno-text>` — `rsp_recv_packet()` returned −1; `<errno-text>` is `strerror(errno)`.
    * `avrOSdb: shutting down (<reason>)` — emitted exactly once, immediately after `event_loop()` returns and before teardown begins. `<reason>` shall be one of `SIGINT`, `SIGTERM`, or `fatal: <text>` for any other path that sets `g_quit`.

    No new CLI flag shall gate these emissions; they are always-on and route to `stderr` only (never `stdout`, which is reserved for `--device` reports, the `--prog` progress bar, and UPDI console traffic). The lines shall be terminated by a single `\n` and shall not contain ANSI colour codes. Each transition shall produce exactly one line; duplicates from the same transition (e.g. an `EOF` followed by a fatal-error path on the same socket) are forbidden. The `D`/`vKill` reason classifier shall be carried out of `gdb_rsp.c` via a new `RspContext.disconnect_reason` field set by the corresponding handlers; `event_loop()` reads and clears that field after dispatching each packet so the protocol layer never invokes upper-layer code (preserving the layered-architecture constraint, SDD §2.2).
    *Trace:* [SDD Section 4.1](SDD.md).

*   <a id="HLR-066"></a>**HLR-066: vCont;r Range Step.**
    The `vCont` packet handler shall accept the `r<start>,<end>[:<thread>]` action and implement GDB's range-step semantics: keep single-stepping the target one instruction at a time so long as the live PC remains inside the half-open range `[start, end)`. When the PC leaves that range — or the user issues a Ctrl-C interrupt — the handler shall halt and emit a single stop reply identical in form to the `vCont;s` reply (a `T05`/`T02` packet with the `swbreak`/`hwbreak`/`step`/`intr` cause classification per HLR-062). `start` and `end` are GDB code addresses (Flash, no `0x800000` data flag); both are 32-bit hex without the `0x` prefix in the wire packet. The `vCont?` probe reply shall be extended from `vCont;c;s` to `vCont;c;s;r` so GDB's range-step optimisation activates. The handler shall reuse the existing OCD STATUS poll cadence (5 ms) for Ctrl-C detection; per-step PC reads use `updi_ocd_read_pc()`. As a safety net, an unbounded loop shall be capped at `RSP_RANGE_STEP_MAX = 100000` iterations to defend against a runaway target whose PC never exits the range; reaching the cap shall halt the target and emit a normal `T05` reply (the user will see no progress and can investigate). Range-step does not require breakpoint installation: it is a software loop driven entirely by the host. Backward compatibility: GDB clients that do not negotiate `vCont;r` continue to receive the prior `vCont;c;s` advertisement-equivalent behaviour because they never send `vCont;r` in the first place.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-063"></a>**HLR-063: qXfer:memory-map:read+ Advertisement.**
    The RSP server shall advertise `qXfer:memory-map:read+` in its `qSupported` reply and shall service `qXfer:memory-map:read::<offset>,<length>` requests by returning a GDB memory-map XML document describing the seven AVR-Dx memory regions visible to GDB: Flash, SRAM, EEPROM, FUSES, LOCK, SIGROW, and USERROW. The Flash region shall be expressed with `start="0x0"`, `length=<flash_size>`, and a `<property name="blocksize">` child set to the AVR-Dx Flash page size (`UPDI_FLASH_PAGE_SIZE` = 512 bytes). The SRAM region shall be expressed with `type="ram"`, `start=<sram_base>`, and `length=<sram_size>`, where `sram_base` is the GDB-side data-space address (already carrying the AVR `0x00800000` data-space flag, sourced verbatim from the second `PT_LOAD` segment of the supplied ELF). Flash and SRAM sizes shall be sourced from `RspContext.flash_size` / `.sram_base` / `.sram_size`, which `main.c` populates from the parsed ELF at session init. The five non-FLASH NVM regions are AVR-Dx device-class constants and shall be advertised at their GDB-visible ELF VMA bands (the same bands `load_segments()` in `src/main.c` already recognises for M-packet routing): EEPROM at `ELF_VMA_EEPROM = 0x810000` length `UPDI_EEPROM_SIZE = 0x200`, FUSES at `ELF_VMA_FUSES = 0x820000` length `UPDI_FUSES_SIZE = 0x10`, LOCK at `ELF_VMA_LOCK = 0x830000` length `UPDI_LOCK_SIZE = 0x4`, SIGROW at `ELF_VMA_SIGROW = 0x840000` length `UPDI_SIGROW_SIZE = 0x40`, USERROW at `ELF_VMA_USERROW = 0x850000` length `UPDI_USERROW_SIZE = 0x20`. EEPROM, FUSES, and LOCK shall be typed `flash` with `blocksize=0x1` (byte-granular host-side erase/write window — the silicon-side page programming is hidden by `updi.c`); USERROW shall be typed `flash` with `blocksize=UPDI_USERROW_SIZE` (whole-row write semantics on AVR-Dx); SIGROW shall be typed `rom` (read-only, no blocksize). The non-FLASH regions appear whenever the Flash or SRAM region is present. When the server is started without an ELF (or the ELF lacks both a Flash and a SRAM `PT_LOAD`), the handler shall reply `l` (end-of-transfer with no payload) and GDB shall fall back to its built-in defaults rather than receive a fabricated map. The reply shall honour the standard `qXfer` framing: a leading `m` for "more data follows" or `l` for "last chunk", followed by the raw XML bytes from `[offset, offset+length)` of the rendered document. Malformed `<offset>,<length>` syntax (missing comma, non-hex digits) shall produce `E00`. The XML shall be re-rendered per request into a stack buffer (no heap, no caching) so the handler is reentrant and stateless.
    *Trace:* [SDD Section 5.3.1](SDD.md).

*   <a id="HLR-067"></a>**HLR-067: Memory-Map Honesty for `m`-Reads.**
    The RSP server's `m<addr>,<len>` read handler shall enforce that every read request lies entirely within one of the memory regions it advertised via `qXfer:memory-map:read+` (HLR-063), and shall reply with the GDB error code `E14` (the conventional `EFAULT` mapping for "memory inaccessible") whenever any byte of the requested range falls outside every advertised region. This honesty contract exists because GDB's stack-unwind logic — used by `finish`, `step-out`, and frame-walking — reads the saved return address off the target's stack via DWARF CFI and then plants a temporary breakpoint at the address it reads; if the server fabricates filler bytes (e.g. `0xff`) for an out-of-range address, GDB will install a temp breakpoint at a fabricated location and hang waiting for it to fire. The check shall be performed against the same `RspContext.flash_size` / `.sram_base` / `.sram_size` fields HLR-063 uses to build its advertisement and shall include all seven advertised regions (Flash, SRAM, EEPROM, FUSES, LOCK, SIGROW, USERROW) using the AVR-Dx device-class constants and ELF VMA bands defined in HLR-063. Backward-compatibility: when both `flash_size` and `sram_size` are zero — the same condition under which HLR-063 publishes no map and GDB falls back to its built-in defaults — the bounds check shall be bypassed and the historical permissive behaviour of `m` shall be preserved (so the no-ELF startup mode and `tests/hw/gdb_acceptance.py` continue to work). Validation: the check shall be implemented in `dh_read_mem` only; symmetric enforcement on `M` / `X` writes is deliberately deferred to a future requirement because writes do not feed GDB's stack-unwind path. The arithmetic shall reject `len == 0` and shall detect 32-bit address-plus-length wrap-around so a malicious or malformed packet cannot bypass the check by overflow.
    *Trace:* [SDD Section 5.3.1](SDD.md).
