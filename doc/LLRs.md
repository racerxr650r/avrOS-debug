# Low-Level Requirements

**Version:** 0.1
**Date:** 2026-05-16
**Author(s):** John Anderson

## 1. Introduction

Each LLR ID follows the pattern `LLR-XXX-NN`, where `XXX` is a module code and `NN` is a zero-padded two-digit sequence number. Module codes: `MAIN` (`src/main.c`), `UPDI` (`src/updi.c`), `RSP` (`src/gdb_rsp.c`), `ELF` (`src/elf_parser.c`), `FSM` (`src/fsm_mapper.c`), `MON` (`src/monitor.c`), `INST` (`Makefile`, `doc/avr-updi-gdb.1`, `doc/UserManual.md`). IDs are permanent contracts and may not be renumbered or reused once allocated.

## 2. src/main.c — Entry Point and Event Loop

Requirements for `main()`, `parse_args()`, and `event_loop()`. These functions own the application lifecycle from argument parsing through resource cleanup.

*   <a id="LLR-MAIN-01"></a>**LLR-MAIN-01** — `parse_args()` shall populate an `AppConfig` struct from `argc`/`argv`. If any unrecognised argument is encountered, it shall print a usage message to `stderr` and call `exit(1)`.
    *Trace:* HLR-001 (CLI Argument Parsing).

*   <a id="LLR-MAIN-02"></a>**LLR-MAIN-02** — When not supplied on the command line, `parse_args()` shall apply the following defaults to the `AppConfig` fields: `gdb_port` = 1234, `baud_rate` = 115200, `load_flash` = false. The `<serial-device>` and `<elf-file>` positional arguments are mandatory; absence of either shall trigger the usage error path.
    *Trace:* HLR-001 (CLI Argument Parsing).

*   <a id="LLR-MAIN-03"></a>**LLR-MAIN-03** — `main()` shall call `updi_open()` and validate the returned file descriptor before calling `rsp_listen()`. A failure from either shall cause `main()` to release any already-open resources and return exit code 1.
    *Trace:* HLR-002 (Serial Device Initialisation), HLR-003 (GDB Listener Startup).

*   <a id="LLR-MAIN-04"></a>**LLR-MAIN-04** — When the `--load` flag is set, `main()` shall iterate over all `PT_LOAD` ELF program headers and call `updi_nvm_write_flash()` for each segment whose `p_filesz > 0` and whose virtual address lies below the SRAM base (i.e. flash-resident segments only). Segments with `p_vaddr >= ctx.sram_base` shall be skipped. All qualifying segments shall be written before entering `event_loop()`. A flash write failure shall cause `main()` to print an error to `stderr` and return exit code 1.
    *Trace:* HLR-004 (ELF Flash Load Option).

*   <a id="LLR-MAIN-05"></a>**LLR-MAIN-05** — `event_loop()` shall multiplex `listen_fd`, `gdb_fd`, and `updi_fd` using a single POSIX `select()` call with no timeout. On each iteration it shall: accept a GDB client on `listen_fd` if `gdb_fd` is -1; forward console bytes from `updi_fd` to `stdout` via `updi_console_poll()`; dispatch RSP packets from `gdb_fd` via `rsp_recv_packet()` and `rsp_dispatch()`. No POSIX threads shall be created.
    *Trace:* HLR-003 (GDB Listener Startup), HLR-039 (Single-Threaded Event Loop Architecture).

*   <a id="LLR-MAIN-06"></a>**LLR-MAIN-06** — `main()` shall register a `SIGINT`/`SIGTERM` handler that sets the global `volatile sig_atomic_t g_quit` flag to 1. `event_loop()` shall test `g_quit` at the top of each loop iteration and return immediately when the flag is set, without closing any file descriptors itself.
    *Trace:* HLR-035 (Graceful Shutdown and Resource Release), HLR-039 (Single-Threaded Event Loop Architecture).

*   <a id="LLR-MAIN-07"></a>**LLR-MAIN-07** — On all exit paths, `main()` shall release resources in this exact order: (1) `rsp_close(cfg.gdb_fd)` if `gdb_fd >= 0`, (2) `rsp_close(cfg.listen_fd)`, (3) `elf_close(&ctx)`, (4) `updi_close(cfg.updi_fd)`. The process shall exit with code 0 on normal termination and code 1 on any fatal initialisation or flash-write failure.
    *Trace:* HLR-035 (Graceful Shutdown and Resource Release).

*   <a id="LLR-MAIN-08"></a>**LLR-MAIN-08** — `parse_args()` shall recognise the `--device` long option, set `cfg->device_info = true`, and accept the absence of the `<elf-file>` operand without error. Specifying both `--device` and `--load` on the same command line shall cause `parse_args()` to print `"error: --device is mutually exclusive with --load"` to `stderr`, print the usage synopsis, and call `exit(1)`.
    *Trace:* HLR-044 (Device-Signature Diagnostic Mode).

*   <a id="LLR-MAIN-09"></a>**LLR-MAIN-09** — When `cfg.device_info` is true, `main()` shall: (1) call `updi_open()`; (2) on success call `updi_read_device_info()`; (3) on success format and print to `stdout` a multi-line report containing `Serial device:`, `Baud rate:`, `Signature:` (3 hex bytes), `Family:` (looked up via a static `device_family[]` table indexed by signature \u2014 unknown signatures shall print `unknown device`), `Revision:`, `Serial:` (10 hex bytes), and `UPDI status: SYS_STATUS=0x.. KEY_STATUS=0x.. STATUSB=0x..`; (4) call `updi_close()` and return. The function shall **not** call `rsp_listen()`, `elf_find_avros_tables()`, `fsm_build_thread_list()`, or `event_loop()`. On any UPDI failure the function shall print a verbose diagnostic to `stderr` that names the failed step (e.g. `"updi-open"`, `"sigrow"`, `"sys-status"`) and return exit code 1.
    *Trace:* HLR-044 (Device-Signature Diagnostic Mode).

## 3. src/updi.c — UPDI Physical Layer

Requirements for all public functions in `src/updi.c`: link initialisation, memory access, execution control, NVM programming, and console bridging.

*   <a id="LLR-UPDI-01"></a>**LLR-UPDI-01** — `updi_open()` shall open the UART device and configure it for 8E2 framing (8 data bits, even parity, 2 stop bits) in raw half-duplex mode using `termios`. `cfmakeraw()`, `CS8`, `CSTOPB`, `PARENB` (with `PARODD` cleared), and explicit baud setting via `cfsetispeed()`/`cfsetospeed()` shall be applied. If `tcsetattr()` returns `EINVAL` when setting `PARENB` (Linux PTY slaves strip parity from virtual terminals), the function shall retry with `PARENB` cleared so the link still works against PTY-backed test harnesses. The function shall return the open file descriptor on success or -1 on failure.
    *Trace:* HLR-006 (UPDI Hardware Connection).

*   <a id="LLR-UPDI-02"></a>**LLR-UPDI-02** — `updi_open()` shall execute up to three cold-start attempts. Attempt 0 (fast path) shall write a single 0x00 wake byte at the configured session baud rate, followed by `tcdrain()` and `tcflush(fd, TCIFLUSH)` to discard the half-duplex echo. Attempts 1 and 2 (slow path) shall instead generate a multi-millisecond line-low pulse by temporarily switching the kernel baud rate to 300 baud with `CSTOPB` cleared (1 stop bit) via `tcsetattr(TCSADRAIN)`, then writing two 0x00 bytes each followed immediately by `tcdrain()` to guarantee the byte is actually clocked out of the UART FIFO before the next operation. Each byte at 300 baud holds the TX line low for ≈33 ms (≥60 ms total — sufficient to reset the target's UPDI clock and contention detector on a cold power-on with passive single-wire combiners), and the half-duplex echoes are read-and-discarded at 300 baud. After both BREAK bytes, `updi_open()` shall sleep ≈50 ms (`nanosleep()`) so that any echo bytes still in flight on the line bus reach the kernel RX queue at 300-baud framing, then `tcflush(TCIFLUSH)` discards them. Only then shall the session baud rate be restored (`tcsetattr(TCSADRAIN)` followed by a second `tcflush(TCIFLUSH)`). Without `tcdrain()` after each write, the subsequent baud switch flushes or re-clocks the queued BREAK byte, destroying the line-low pulse; without the 50 ms settle, late echo bytes arrive AFTER the baud restore, get framed at 115200 baud, and desynchronise every subsequent frame echo. This mirrors avrdude's `serialupdi` programmer exactly (verified by `strace`) and is necessary because no portable POSIX API generates a multi-ms BREAK on demand — `tcsendbreak()` is too short and not honoured by all kernel UART drivers (notably the RPi PL011).
    *Trace:* HLR-006 (UPDI Hardware Connection), HLR-036 (UPDI Cold-Start Contention Handshake).

*   <a id="LLR-UPDI-03"></a>**LLR-UPDI-03** — After the wake byte (attempt 0) or 300-baud double-break (attempts 1–2), `updi_open()` shall transmit `STCS ASI_CTRLB = ASI_CTRLB_CCDETDIS` (0x08) to disable the target's collision/contention detector, then `STCS ASI_CTRLA = ASI_CTRLA_IBDLY` (0x80) to enable inter-byte delay on the target's responses, then probe the link with `LDCS ASI_STATUSA` (opcode `0x80 | 0x00 = 0x80`). `ASI_STATUSA` carries UPDIREV in its upper nibble and is always readable while UPDI is enabled, including before any reset. If any STCS write or the LDCS probe fails, `updi_open()` shall advance to the next attempt. After 3 consecutive failed attempts the function shall return -1 and the caller shall treat the link as unavailable.
    *Trace:* HLR-036 (UPDI Cold-Start Contention Handshake).

*   <a id="LLR-UPDI-04"></a>**LLR-UPDI-04** — `updi_mem_read()` and `updi_mem_write()` shall transfer up to `UPDI_MAX_BLOCK` (256) bytes per UPDI burst using a three-frame sequence per datasheet §35.3.3.4: (1) `ST_PTR_WORD` (`0x69`) followed by the 16-bit target address in little-endian order and a single ACK readback; (2) `REPEAT` (`0xA0`) followed by `(block_len - 1)`; (3) `LD ptr++` (`0x24`) for reads or `ST ptr++` (`0x64`) for writes. If `len` exceeds `UPDI_MAX_BLOCK`, the function shall split the request into consecutive bursts automatically, with no size restriction imposed on the caller.
    *Trace:* HLR-007 (Target Memory Read), HLR-008 (Target Memory Write).

*   <a id="LLR-UPDI-05"></a>**LLR-UPDI-05** — `updi_mem_read()` shall use `select()` with a 100 ms timeout before each `read()` call on the UART file descriptor to enforce a per-byte inactivity deadline. If `select()` returns zero (timeout) before a byte is available, the function shall abandon the current transaction and return -1.
    *Trace:* HLR-007 (Target Memory Read), HLR-037 (UPDI Operation Timeout Bounds).

*   <a id="LLR-UPDI-06"></a>**LLR-UPDI-06** — `updi_mem_write()` shall write arbitrary byte ranges to the SRAM address space of the target using UPDI ST commands and return 0 on success or -1 on UART framing error, timeout, or UPDI NAK.
    *Trace:* HLR-008 (Target Memory Write).

*   <a id="LLR-UPDI-07"></a>**LLR-UPDI-07** — `updi_nvm_write_flash()` shall require `word_addr` to be aligned to a 512-byte FLASH page boundary and `len` to be a non-zero multiple of 512. It shall enter NVM programming mode by transmitting the `KEY` opcode (`0xE0`) with the 8-byte string `"NVMProg "`, asserting reset with `STCS ASI_RESET_REQ = 0x59`, releasing reset with `STCS ASI_RESET_REQ = 0x00`, then polling `ASI_SYS_STATUS` via LDCS for bit 3 (NVMPROG, mask `0x08`) up to 100 iterations (≈ 100 ms). For each page it shall write the page data via `updi_mem_write()`, write `0x03` (ERWP) to `NVMCTRL_CTRLA`, and poll `NVMCTRL_STATUS` bit 0 (BUSY) clear up to 20 iterations (≈ 20 ms). On exit it shall toggle `ASI_RESET_REQ` `0x59` then `0x00` to release the target. Timeout of either poll shall return -1; a `NVMCTRL_STATUS` WRERR (bit 2) shall return `UPDI_ERR_WP`.
    *Trace:* HLR-009 (FLASH Programming), HLR-037 (UPDI Operation Timeout Bounds).

*   <a id="LLR-UPDI-08"></a>**LLR-UPDI-08** — `updi_halt()` is a Phase 2 stub. It shall return -1 unconditionally with no UART traffic. The full halt sequence (OCD register manipulation, STOPPED-bit polling) is part of the AVR On-Chip Debug specification and is implemented by the Phase 3 OCD layer.
    *Trace:* HLR-010 (Execution Control).

*   <a id="LLR-UPDI-09"></a>**LLR-UPDI-09** — `updi_step()` is a Phase 2 stub. It shall return -1 unconditionally with no UART traffic. Single-step is implemented by the Phase 3 OCD layer.
    *Trace:* HLR-010 (Execution Control).

*   <a id="LLR-UPDI-10"></a>**LLR-UPDI-10** — `updi_run()` is a Phase 2 stub. It shall return -1 unconditionally with no UART traffic. CPU resume is implemented by the Phase 3 OCD layer.
    *Trace:* HLR-010 (Execution Control).

*   <a id="LLR-UPDI-11"></a>**LLR-UPDI-11** — `updi_mem_read()` shall be callable while the target CPU is in the RUNNING state (not halted). The function shall not call `updi_halt()` internally, permitting non-intrusive SRAM sampling for the monitor sub-commands and console polling without interrupting firmware execution.
    *Trace:* HLR-011 (Non-Intrusive Background Memory Read).

*   <a id="LLR-UPDI-12"></a>**LLR-UPDI-12** — `updi_console_poll()` shall perform a background `updi_mem_read()` of the avrOS software UART output buffer and return any pending bytes to the caller without halting the CPU. The function shall return the number of bytes read (0 if none pending) or -1 on UPDI error.
    *Trace:* HLR-012 (UPDI Console Bridge).

*   <a id="LLR-UPDI-13"></a>**LLR-UPDI-13** — `updi_read_device_info(fd, info)` shall populate the caller-supplied `UpdiDeviceInfo` struct with: 3 SIGROW DEVICEID bytes from physical address `0x1100`-`0x1102`, the REVID byte at `0x1103`, 10 SERNUM bytes at `0x1110`-`0x1119`, and the three ASI registers `ASI_SYS_STATUS`, `ASI_KEY_STATUS`, `ASI_STATUSB` read via `LDCS`. The function shall be non-destructive (no halt, no NVM activity). On any UPDI read failure it shall set `info->fail_op` to a short ASCII tag (`"sigrow"`, `"revid"`, `"sernum"`, `"sys-status"`, `"key-status"`, `"asi-statusb"`) and return -1; on success it shall set `info->fail_op = NULL` and return 0.
    *Trace:* HLR-044 (Device-Signature Diagnostic Mode).

*   <a id="LLR-UPDI-14"></a>**LLR-UPDI-14** — After each successful `LDCS ASI_STATUSA` link probe inside `updi_open()`, the function shall read the target's 32-byte System Information Block by transmitting `SYNCH` (`0x55`) followed by opcode `UPDI_OP_KEY_SIB` (`0xE6`) and reading exactly `UPDI_SIB_LEN` (32) bytes of response — looped over `read()` until the full block is collected. The SIB read serves two purposes: (1) it confirms the link end-to-end (target must be actively responding to return non-echo bytes), and (2) it wakes a target that was left in UPDI SLEEP from a prior debug session. Without this step a sleeping target will appear to accept every link-layer probe (STCS, LDCS) — those frames merely echo on the half-duplex line — yet reject every memory access, manifesting as `updi_mem_read()` returning -1 immediately after a successful `updi_open()`. avrdude's `serialupdi` issues this exact 32-byte SIB read on every connect; the response payload (e.g. `"    AVR P:2D:1-3M2 (A7.KV001.0)\0"`) is discarded by `updi_open()` since the caller currently only needs the wake side-effect. If the SIB read times out or returns fewer than 32 bytes the attempt shall be abandoned and the next cold-start attempt shall begin.
    *Trace:* HLR-006 (UPDI Hardware Connection), HLR-036 (UPDI Cold-Start Contention Handshake).

## 4. src/gdb_rsp.c — GDB Remote Serial Protocol Server

Requirements for RSP packet framing, dispatch, GDB packet handlers, session lifecycle, and socket configuration.

*   <a id="LLR-RSP-01"></a>**LLR-RSP-01** — `rsp_listen()` shall set `SO_REUSEADDR` on the listener socket before calling `bind()`. After `rsp_accept()` returns a connected client socket, the client socket shall have `TCP_NODELAY` set to disable Nagle's algorithm.
    *Trace:* HLR-003 (GDB Listener Startup), HLR-038 (GDB Session Response Latency).

*   <a id="LLR-RSP-02"></a>**LLR-RSP-02** — `rsp_recv_packet()` shall scan incoming bytes discarding pre-packet ACK/NAK characters until a `$` delimiter is received. It shall accumulate the payload with a running XOR checksum until `#` is received, then compare the computed checksum against the two ASCII-hex checksum bytes that follow. On match it shall write `+` to the socket and return the payload length; on mismatch it shall write `-` and return -1.
    *Trace:* HLR-013 (RSP Server Accessibility).

*   <a id="LLR-RSP-03"></a>**LLR-RSP-03** — The `on_read_regs` handler for the `g` packet shall delegate to `fsm_get_registers()` for the currently selected virtual thread (stored in `RspContext.g_thread_p`, defaulting to the active FSM thread when zero or negative) and return the resulting 78-character hex string. The FSM mapper is responsible for placing R0-R31 at hex positions 0-63, SREG at 64-65, SPL at 66-67, SPH at 68-69, and PC as 4-byte little-endian at positions 70-77; live SREG/SP bytes for the active thread are read via `updi_mem_read()` from within `fsm_get_registers()`.
    *Trace:* HLR-014 (Register Read and Write).

*   <a id="LLR-RSP-04"></a>**LLR-RSP-04** — The `on_write_regs` handler for the `G` packet and the single-register `P` handler shall write the supplied register values to the target's CPU register file via `updi_mem_write()` and return `OK` on success or an error reply on UPDI failure.
    *Trace:* HLR-014 (Register Read and Write).

*   <a id="LLR-RSP-05"></a>**LLR-RSP-05** — The `on_read_mem` handler for `m addr,len` shall call `updi_mem_read()` with the decoded address and length, and return the result as a hex string. Both FLASH and SRAM addresses shall be accepted; address-space routing is handled by `updi_mem_read()` based on the raw address value.
    *Trace:* HLR-015 (Memory Read and Write).

*   <a id="LLR-RSP-06"></a>**LLR-RSP-06** — The `on_write_mem` handler for `M addr,len:data` (hex-encoded) and `X addr,len:data` (binary-encoded) packets shall call `updi_mem_write()` for SRAM addresses and `updi_nvm_write_flash()` for FLASH addresses, returning `OK` on success or an error reply on failure.
    *Trace:* HLR-015 (Memory Read and Write).

*   <a id="LLR-RSP-07"></a>**LLR-RSP-07** — The `on_insert_bp` handler for `Z0 addr,kind` shall: (1) check the breakpoint table for an existing entry at the requested address; if not present, (2) read and save the 2-byte instruction word via `updi_mem_read()`, (3) write the AVR BREAK opcode (0x9598) via `updi_nvm_write_flash()`, and (4) return `OK`. A duplicate insert shall return `OK` without a second flash write.
    *Trace:* HLR-016 (Software Breakpoints).

*   <a id="LLR-RSP-08"></a>**LLR-RSP-08** — The `on_remove_bp` handler for `z0 addr,kind` shall look up the requested address in the breakpoint table, restore the saved 2-byte instruction word via `updi_nvm_write_flash()`, remove the entry from the table, and return `OK`. If the address is not in the table, the handler shall return an error reply.
    *Trace:* HLR-016 (Software Breakpoints).

*   <a id="LLR-RSP-09"></a>**LLR-RSP-09** — When `on_insert_bp` is called and all `RSP_MAX_BREAKPOINTS` (16) breakpoint table slots are occupied, it shall not write to FLASH and shall return the GDB error reply `E08`.
    *Trace:* HLR-016 (Software Breakpoints).

*   <a id="LLR-RSP-10"></a>**LLR-RSP-10** — The `on_step` handler for `s`/`vCont;s` shall call `updi_step()` and, on success, send a stop-reason packet of the form `T05thread:<active_thread_id>;` to the GDB client.
    *Trace:* HLR-017 (Single-Step Execution).

*   <a id="LLR-RSP-11"></a>**LLR-RSP-11** — The `on_continue` handler for `c`/`vCont;c` shall call `updi_run()` followed by `fsm_invalidate()`, then poll by calling `updi_halt()` until the CPU stops. On halt, it shall call `fsm_build_thread_list()` to rebuild the thread list and send a stop-reason packet to the GDB client.
    *Trace:* HLR-018 (Continue Execution).

*   <a id="LLR-RSP-12"></a>**LLR-RSP-12** — `rsp_dispatch()` shall handle the following packets inline without invoking any `RspHandlers` slot, because each response is a fixed string requiring no target interaction: `qSupported` (responds with `PacketSize=800;QStartNoAckMode+;multiprocess-;vContSupported+`), `qAttached` (responds `1`), `QStartNoAckMode` (calls `rsp_set_noack(true)` and responds `OK`), and `vCont?` (responds `vCont;c;s`).
    *Trace:* HLR-019 (RSP Capability Negotiation and Lifecycle).

*   <a id="LLR-RSP-13"></a>**LLR-RSP-13** — The `on_detach` handler for the `D` packet shall call `updi_run()` to resume the target, close the GDB client socket, reset `cfg->gdb_fd` to -1, and allow the event loop to re-enter the listening state. The server process shall not exit.
    *Trace:* HLR-019 (RSP Capability Negotiation and Lifecycle).

*   <a id="LLR-RSP-14"></a>**LLR-RSP-14** — The `on_detach` handler for the `k` (kill) packet shall set `g_quit = 1` to cause the event loop to exit, triggering the full resource teardown sequence defined by LLR-MAIN-07.
    *Trace:* HLR-019 (RSP Capability Negotiation and Lifecycle), HLR-035 (Graceful Shutdown and Resource Release).

*   <a id="LLR-RSP-15"></a>**LLR-RSP-15** — The `on_monitor` handler for `qRcmd` shall pass the raw ASCII-hex-encoded command body directly to `monitor_dispatch()` without pre-decoding it, and relay the return code to the GDB client as `OK` on success (return 0), an O-packet error message on UPDI failure (return -1), or an empty reply on unrecognised sub-command (return -2).
    *Trace:* HLR-029 (Monitor Events Command), HLR-030 (Monitor Queues Command).

*   <a id="LLR-RSP-16"></a>**LLR-RSP-16** — The `H` (set-thread) packet handler shall store the requested GDB thread ID in the session context and use it to select the virtual thread for subsequent `g`/`G`/`P` register operations. Thread ID -1 (all threads) and 0 (any thread) shall both be interpreted as selecting the active FSM thread.
    *Trace:* HLR-025 (Active Thread Identification).

## 5. src/elf_parser.c — ELF Binary Parser

Requirements for `elf_open()`, `elf_close()`, `elf_find_avros_tables()`, and `elf_flash_addr()`.

*   <a id="LLR-ELF-01"></a>**LLR-ELF-01** — `elf_open()` shall read the first 16 bytes of the ELF binary and verify: the 4-byte magic sequence (0x7F `'E'` `'L'` `'F'`), `EI_CLASS == ELFCLASS32`, and `e_machine == EM_AVR` (0x0053). Any validation failure shall cause `elf_open()` to log a warning and return -1 without any heap allocation.
    *Trace:* HLR-021 (ELF Binary Parsing).

*   <a id="LLR-ELF-02"></a>**LLR-ELF-02** — `elf_open()` shall locate the `.symtab` section (type `SHT_SYMTAB`) and its associated string table section (index given by `sh_link`), and load both into heap-allocated buffers stored in `ctx->symtab` and `ctx->strtab`. `ctx->sym_count` and `ctx->strtab_size` shall be set accordingly. On any `malloc` failure, `elf_open()` shall free all partial allocations and return -1.
    *Trace:* HLR-021 (ELF Binary Parsing), HLR-040 (Bounded Heap Allocation).

*   <a id="LLR-ELF-03"></a>**LLR-ELF-03** — `elf_find_avros_tables()` shall perform a single O(sym_count) linear scan of `ctx->symtab`, compare each symbol name against the 8 avrOS sentinel names defined in SDD §6.3.3, and populate the corresponding fields of `AvrOsSymbolIndex` for each match. The function shall return 0 (success, even if partial) or -1 only on an internal ELF read error.
    *Trace:* HLR-021 (ELF Binary Parsing).

*   <a id="LLR-ELF-04"></a>**LLR-ELF-04** — `elf_flash_addr()` shall convert an ELF virtual memory address to a physical FLASH word address using the formula: `physical_word_addr = (vma - ctx->flash_base) / 2`. All FLASH-resident avrOS symbol addresses stored in `AvrOsSymbolIndex` shall be computed via this function.
    *Trace:* HLR-022 (Harvard Architecture Address Mapping).

*   <a id="LLR-ELF-05"></a>**LLR-ELF-05** — When `elf_find_avros_tables()` completes the symbol scan and fewer than the expected avrOS symbols were found, it shall return 0 (not -1). Fields of `AvrOsSymbolIndex` for absent symbols shall retain their zero-initialised values, and the function shall log an informational message identifying which symbols were not found.
    *Trace:* HLR-023 (Fail-Safe Degradation on Missing Symbols).

*   <a id="LLR-ELF-06"></a>**LLR-ELF-06** — `elf_close()` shall free `ctx->symtab`, free `ctx->strtab`, and close `ctx->fd`. After returning, `ctx->symtab` and `ctx->strtab` shall be set to NULL and `ctx->fd` to -1. `elf_close()` shall be safe to call on a partially initialised `ElfContext` (e.g., after a failed `elf_open()`).
    *Trace:* HLR-040 (Bounded Heap Allocation).

*   <a id="LLR-ELF-07"></a>**LLR-ELF-07** — `src/elf_parser.c` shall obtain ELF32 type definitions via a platform-conditional include: on Linux (`#ifdef __linux__`) it shall use the system header `<elf.h>`; on all other platforms it shall include the bundled portability shim `src/elf.h`. The shim shall define at minimum: `Elf32_Half`, `Elf32_Word`, `Elf32_Off`, `Elf32_Addr`, `Elf32_Ehdr`, `Elf32_Phdr`, `Elf32_Shdr`, `Elf32_Sym`, the `ELFMAG`/`SELFMAG` magic constants, `EI_CLASS`, `ELFCLASS32`, `EM_AVR` (0x0053), `PT_LOAD`, `SHT_SYMTAB`, `SHT_STRTAB`, `SHN_UNDEF`, and the `ELF32_ST_BIND`/`ELF32_ST_TYPE` accessor macros.
    *Trace:* HLR-033 (Native Linux and macOS Build).

*   <a id="LLR-ELF-08"></a>**LLR-ELF-08** — `elf_open()` shall iterate over all ELF program headers and identify `PT_LOAD` segments. The first `PT_LOAD` segment encountered shall be treated as the FLASH load segment: its `p_vaddr` and `p_filesz` shall be stored in `ctx->flash_base` and `ctx->flash_size` respectively. The second `PT_LOAD` segment shall be treated as the SRAM load segment: its `p_vaddr` and `p_memsz` shall be stored in `ctx->sram_base` and `ctx->sram_size` respectively. If fewer than two `PT_LOAD` segments are present the absent values shall remain zero. These fields are the sole inputs to `elf_flash_addr()`.
    *Trace:* HLR-021 (ELF Binary Parsing), HLR-022 (Harvard Architecture Address Mapping).

## 6. src/fsm_mapper.c — FSM Virtual Thread Mapper

Requirements for `fsm_build_thread_list()`, `fsm_invalidate()`, `fsm_get_active_thread()`, and `fsm_get_registers()`.

*   <a id="LLR-FSM-01"></a>**LLR-FSM-01** — `fsm_build_thread_list()` shall read `idx->fsm_table_count` consecutive 9-byte `fsmStateMachineDescr_t` records from the FLASH address `idx->fsm_table_addr` via `updi_mem_read()`. Each descriptor carries a 2-byte `name` pointer (FLASH), a 2-byte `stateMachine` pointer (SRAM, or NULL for an initializer slot that shall be skipped), a 2-byte `handler` function pointer, a 1-byte `priority`, and a 2-byte `instance`. For each non-NULL `stateMachine` pointer, the function shall read 2 bytes from `stateMachine + 9` (the `currState` field of `fsmStateMachine_t`) to obtain the current state function pointer and store it in `thread->state_fn`. A UPDI read failure at any point shall cause the function to return -1.
    *Trace:* HLR-024 (FSM Thread Enumeration).

*   <a id="LLR-FSM-02"></a>**LLR-FSM-02** — `fsm_build_thread_list()` shall assign `thread->gdb_id = loop_index + 1` (1-based) to each FSM entry in the order the entries appear in the FLASH table. Thread IDs shall not be reassigned within a debug session, ensuring the same FSM always maps to the same GDB thread ID.
    *Trace:* HLR-024 (FSM Thread Enumeration).

*   <a id="LLR-FSM-03"></a>**LLR-FSM-03** — `fsm_build_thread_list()` shall read the 2-byte SRAM value at `idx->current_fsm_addr` (avrOS `currStateMachine`) and compare it against each retained FSM entry's `stateMachine` pointer. The matching entry shall have `thread->is_active = true` and its `gdb_id` stored in `ctx->active_id`. If no entry matches, `ctx->active_id` shall be set to 0.
    *Trace:* HLR-025 (Active Thread Identification).

*   <a id="LLR-FSM-04"></a>**LLR-FSM-04** — `fsm_get_registers()` shall set the PC field (GDB register index 35, 4-byte little-endian at hex positions 70–77 of the 78-character g-packet buffer) to `thread->state_fn`. For non-active threads, R0–R31 (indices 0–31) and SREG (index 32) shall be zero-filled, and SPL (index 33) and SPH (index 34) shall be set to zero. For the active thread, SREG (index 32, hex positions 64–65), SPL (index 33, hex positions 66–67), and SPH (index 34, hex positions 68–69) shall be read from the target SRAM via `updi_mem_read()`.
    *Trace:* HLR-026 (Virtual Thread Register Frame).

*   <a id="LLR-FSM-05"></a>**LLR-FSM-05** — `fsm_build_thread_list()` shall process at most `FSM_MAX_THREADS` (32) entries from the FSM registration table regardless of `idx->fsm_table_count`. If `idx->fsm_table_count` exceeds `FSM_MAX_THREADS`, the function shall log a diagnostic warning and return `FSM_MAX_THREADS` as the thread count without returning an error to the GDB client.
    *Trace:* HLR-027 (Complete FSM Thread Coverage).

*   <a id="LLR-FSM-06"></a>**LLR-FSM-06** — `fsm_get_registers()` shall not issue any UPDI memory read targeting a non-active thread's stack region. The suspended FSM register frame shall be constructed entirely from the cached `thread->state_fn` value; the SP field shall be zeroed for non-active threads.
    *Trace:* HLR-028 (Stack-Free Thread Model).

## 7. src/monitor.c — avrOS System Introspection

Requirements for `monitor_dispatch()` and its static sub-command helpers `cmd_events()` and `cmd_queues()`.

*   <a id="LLR-MON-01"></a>**LLR-MON-01** — `monitor_dispatch()` shall hex-decode the ASCII-hex-encoded `cmd` string (pairs of hex digit characters) into a plain-text command string before any prefix or sub-command matching is attempted. An odd-length or invalid hex-digit sequence shall be treated as an unrecognised command.
    *Trace:* HLR-029 (Monitor Events Command).

*   <a id="LLR-MON-02"></a>**LLR-MON-02** — `monitor_dispatch()` shall verify that the decoded command string begins with the prefix `"avros "` (case-sensitive, including the trailing space). If the prefix does not match, the function shall send a usage-hint O-packet to the GDB console and return -2.
    *Trace:* HLR-029 (Monitor Events Command), HLR-030 (Monitor Queues Command).

*   <a id="LLR-MON-03"></a>**LLR-MON-03** — `cmd_events()` shall call `updi_mem_read()` to read `idx->event_count` consecutive 4-byte `evntDescriptor_t` records from FLASH at `idx->event_table_addr`. For each descriptor it shall dereference the `name` pointer (FLASH) to read the event's NUL-terminated display name (up to 31 chars) and the `status` pointer (SRAM) to read the 1-byte status flag, then append a line of the form `"  <name>: <status>\n"` and send the complete buffer as RSP O-packets.
    *Trace:* HLR-029 (Monitor Events Command).

*   <a id="LLR-MON-04"></a>**LLR-MON-04** — `cmd_queues()` shall call `updi_mem_read()` to read `idx->queue_count` consecutive 10-byte `queDescriptor_t` records from FLASH at `idx->queue_table_addr`. For each descriptor it shall extract and format the `capacity` and `sizeOfElement` fields as a one-line entry and send the complete table as RSP O-packets.
    *Trace:* HLR-030 (Monitor Queues Command).

*   <a id="LLR-MON-06"></a>**LLR-MON-06** — All monitor output shall be assembled into a temporary buffer of at most 512 bytes, hex-encoded (each ASCII byte converted to 2 hex characters per RSP O-packet spec), and transmitted to the GDB client as one or more RSP O-packets via `rsp_send_packet()`.
    *Trace:* HLR-029 (Monitor Events Command), HLR-030 (Monitor Queues Command).

*   <a id="LLR-MON-07"></a>**LLR-MON-07** — `monitor_dispatch()` and all of its static helpers (`cmd_events()`, `cmd_queues()`) shall use `updi_mem_read()` exclusively for all target memory access. None of these functions shall call `updi_halt()`, ensuring that monitor commands never interrupt firmware execution.
    *Trace:* HLR-011 (Non-Intrusive Background Memory Read), HLR-032 (Introspection Reliability).

## 8. Makefile and Documentation

Requirements for the Makefile installation targets (`install`, `uninstall`, `check-tools`, `bundle`) and the documentation deliverables (`doc/UserManual.md`, `doc/avr-updi-gdb.1`). Bundle artefacts are written to `dist/`.

*   <a id="LLR-INST-01"></a>**LLR-INST-01** — The Makefile shall provide a `check-tools` target that tests for the presence of every required host tool: `gcc` (or `cc`), `make`, `avr-gcc`, and `avr-nm`. For each absent tool the target shall print a diagnostic message to `stderr` naming the missing tool and then exit with status 1. The target shall exit with status 0 only when all required tools are found.
    *Trace:* HLR-041 (Makefile Install and Uninstall Targets).

*   <a id="LLR-INST-02"></a>**LLR-INST-02** — The Makefile `install` target shall accept a `PREFIX` variable (default `/usr/local`) and install: the compiled `avr-updi-gdb` binary to `$(PREFIX)/bin/avr-updi-gdb` and the man page `doc/avr-updi-gdb.1` to `$(PREFIX)/share/man/man1/avr-updi-gdb.1`. The target shall create any missing intermediate directories using `install -d`. The binary shall be installed with mode 0755 and the man page with mode 0644.
    *Trace:* HLR-041 (Makefile Install and Uninstall Targets).

*   <a id="LLR-INST-03"></a>**LLR-INST-03** — The Makefile `uninstall` target shall remove `$(PREFIX)/bin/avr-updi-gdb` and `$(PREFIX)/share/man/man1/avr-updi-gdb.1` using `rm -f`. The target shall be idempotent: running it when the files are already absent shall exit with status 0 without error.
    *Trace:* HLR-041 (Makefile Install and Uninstall Targets).

*   <a id="LLR-INST-04"></a>**LLR-INST-04** — The project shall include a user manual at `doc/UserManual.md` documenting: (1) prerequisites (required host tools and their minimum versions), (2) build instructions (`make`, `make test`), (3) connection wiring for the UPDI adapter (1 kΩ resistor on UPDI pin, serial adapter TX/RX orientation), (4) launch invocation and all CLI options, and (5) at least two complete usage examples (one with `--load`, one without).
    *Trace:* HLR-042 (User Manual and Unix Man Page).

*   <a id="LLR-INST-05"></a>**LLR-INST-05** — The project shall include a Unix man page at `doc/avr-updi-gdb.1` in `groff`/`troff` format. The man page shall contain at minimum: NAME, SYNOPSIS, DESCRIPTION, OPTIONS (one entry per CLI flag), OPERANDS, EXIT STATUS, EXAMPLES, and SEE ALSO sections. The man page shall be parseable by `man -l doc/avr-updi-gdb.1` without error or warning on Linux.
    *Trace:* HLR-042 (User Manual and Unix Man Page).

*   <a id="LLR-INST-06"></a>**LLR-INST-06** — The Makefile `bundle` target shall produce a Debian binary package at `dist/avr-updi-gdb_$(VERSION)_amd64.deb` using `dpkg-deb --build`. The package staging tree shall install the binary to `usr/bin/avr-updi-gdb` (mode 0755) and the man page to `usr/share/man/man1/avr-updi-gdb.1` (mode 0644). The `DEBIAN/control` file shall declare at minimum: `Package: avr-updi-gdb`, `Version`, `Architecture: amd64`, `Maintainer`, and `Description` fields.
    *Trace:* HLR-043 (Distribution Package Bundle).

*   <a id="LLR-INST-07"></a>**LLR-INST-07** — The Makefile `bundle` target shall produce an RPM binary package at `dist/avr-updi-gdb-$(VERSION)-1.x86_64.rpm` using `rpmbuild`. A `.spec` file shall be generated in the build tree containing at minimum: `Name`, `Version`, `Release: 1`, `Summary`, `License`, `%description`, `%install`, and `%files` sections. The `%files` section shall list `/usr/bin/avr-updi-gdb` and `/usr/share/man/man1/avr-updi-gdb.1`.
    *Trace:* HLR-043 (Distribution Package Bundle).

*   <a id="LLR-INST-08"></a>**LLR-INST-08** — The Makefile `bundle` target shall produce a Homebrew formula at `dist/avr-updi-gdb.rb`. The formula shall be a valid Ruby file usable with `brew install --formula dist/avr-updi-gdb.rb`. It shall define at minimum: `desc`, `homepage`, `url`, `sha256`, `version`, `license`, and an `install` block that places the binary using `bin.install` and the man page using `man1.install`.
    *Trace:* HLR-043 (Distribution Package Bundle).
