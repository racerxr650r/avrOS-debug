# Low-Level Requirements

**Version:** 0.1
**Date:** 2026-05-16
**Author(s):** John Anderson

## 1. Introduction

Each LLR ID follows the pattern `LLR-XXX-NN`, where `XXX` is a module code and `NN` is a zero-padded two-digit sequence number. Module codes: `MAIN` (`src/main.c`), `UPDI` (`src/updi.c`), `RSP` (`src/gdb_rsp.c`), `ELF` (`src/elf_parser.c`), `FSM` (`src/fsm_mapper.c`), `MON` (`src/monitor.c`). IDs are permanent contracts and may not be renumbered or reused once allocated.

## 2. src/main.c — Entry Point and Event Loop

Requirements for `main()`, `parse_args()`, and `event_loop()`. These functions own the application lifecycle from argument parsing through resource cleanup.

*   <a id="LLR-MAIN-01"></a>**LLR-MAIN-01** — `parse_args()` shall populate an `AppConfig` struct from `argc`/`argv`. If any unrecognised argument is encountered, it shall print a usage message to `stderr` and call `exit(1)`.
    *Trace:* HLR-001 (CLI Argument Parsing).

*   <a id="LLR-MAIN-02"></a>**LLR-MAIN-02** — When not supplied on the command line, `parse_args()` shall apply the following default values: `--port` = 1234, `--baud` = 115200, `--load` = false. The `<serial-device>` and `<elf-file>` positional arguments are mandatory; absence of either shall trigger the usage error path.
    *Trace:* HLR-001 (CLI Argument Parsing).

*   <a id="LLR-MAIN-03"></a>**LLR-MAIN-03** — `main()` shall call `updi_open()` and validate the returned file descriptor before calling `rsp_listen()`. A failure from either shall cause `main()` to release any already-open resources and return exit code 1.
    *Trace:* HLR-002 (Serial Device Initialisation), HLR-003 (GDB Listener Startup).

*   <a id="LLR-MAIN-04"></a>**LLR-MAIN-04** — When the `--load` flag is set, `main()` shall iterate over all `PT_LOAD` ELF segments and call `updi_nvm_write_flash()` for each segment before entering `event_loop()`. A flash write failure shall cause `main()` to print an error to `stderr` and return exit code 1.
    *Trace:* HLR-004 (ELF Flash Load Option).

*   <a id="LLR-MAIN-05"></a>**LLR-MAIN-05** — `event_loop()` shall multiplex `listen_fd`, `gdb_fd`, and `updi_fd` using a single POSIX `select()` call with no timeout. On each iteration it shall: accept a GDB client on `listen_fd` if `gdb_fd` is -1; forward console bytes from `updi_fd` to `stdout` via `updi_console_poll()`; dispatch RSP packets from `gdb_fd` via `rsp_recv_packet()` and `rsp_dispatch()`. No POSIX threads shall be created.
    *Trace:* HLR-003 (GDB Listener Startup), HLR-039 (Single-Threaded Event Loop Architecture).

*   <a id="LLR-MAIN-06"></a>**LLR-MAIN-06** — `main()` shall register a `SIGINT`/`SIGTERM` handler that sets the global `volatile sig_atomic_t g_quit` flag to 1. `event_loop()` shall test `g_quit` at the top of each loop iteration and return immediately when the flag is set, without closing any file descriptors itself.
    *Trace:* HLR-035 (Graceful Shutdown and Resource Release), HLR-039 (Single-Threaded Event Loop Architecture).

*   <a id="LLR-MAIN-07"></a>**LLR-MAIN-07** — On all exit paths, `main()` shall release resources in this exact order: (1) `rsp_close(cfg.gdb_fd)` if `gdb_fd >= 0`, (2) `rsp_close(cfg.listen_fd)`, (3) `elf_close(&ctx)`, (4) `updi_close(cfg.updi_fd)`. The process shall exit with code 0 on normal termination and code 1 on any fatal initialisation or flash-write failure.
    *Trace:* HLR-035 (Graceful Shutdown and Resource Release).

## 3. src/updi.c — UPDI Physical Layer

Requirements for all public functions in `src/updi.c`: link initialisation, memory access, execution control, NVM programming, and console bridging.

*   <a id="LLR-UPDI-01"></a>**LLR-UPDI-01** — `updi_open()` shall open the UART device and configure it for 8N2 framing (8 data bits, no parity, 2 stop bits) in raw half-duplex mode using `termios`. `cfmakeraw()`, `CSTOPB`, and explicit baud setting via `cfsetispeed()`/`cfsetospeed()` shall be applied. The function shall return the open file descriptor on success or -1 on failure.
    *Trace:* HLR-006 (UPDI Hardware Connection).

*   <a id="LLR-UPDI-02"></a>**LLR-UPDI-02** — `updi_open()` shall generate the UPDI BREAK condition by temporarily switching the UART baud rate to `UPDI_BREAK_BAUD` (300 baud), transmitting a 0x00 framing byte (which holds TX low for at least 24.6 µs at 300 baud, satisfying the UPDI BREAK timing requirement), then restoring the session baud rate before transmitting the SYNCH character (0x55).
    *Trace:* HLR-006 (UPDI Hardware Connection), HLR-036 (UPDI Link Initialisation Timing and Retry).

*   <a id="LLR-UPDI-03"></a>**LLR-UPDI-03** — If the target does not respond to the BREAK+SYNCH sequence with a UPDI ACK (0x40) within the expected window, `updi_open()` shall retry the entire BREAK+SYNCH sequence. After 3 consecutive failures, `updi_open()` shall return -1 and the caller shall treat the link as unavailable.
    *Trace:* HLR-036 (UPDI Link Initialisation Timing and Retry).

*   <a id="LLR-UPDI-04"></a>**LLR-UPDI-04** — `updi_mem_read()` shall transfer up to `UPDI_MAX_BLOCK` (256) bytes per UPDI burst using the REPEAT+LD auto-increment sequence. If `len` exceeds `UPDI_MAX_BLOCK`, the function shall split the request into consecutive block reads automatically, with no size restriction imposed on the caller.
    *Trace:* HLR-007 (Target Memory Read).

*   <a id="LLR-UPDI-05"></a>**LLR-UPDI-05** — `updi_mem_read()` shall apply a 100 ms per-byte read timeout to every `read()` call on the UART file descriptor. If no byte is received within 100 ms, the function shall abandon the current transaction and return -1.
    *Trace:* HLR-007 (Target Memory Read), HLR-037 (UPDI Operation Timeout Bounds).

*   <a id="LLR-UPDI-06"></a>**LLR-UPDI-06** — `updi_mem_write()` shall write arbitrary byte ranges to the SRAM address space of the target using UPDI ST commands and return 0 on success or -1 on UART framing error, timeout, or UPDI NAK.
    *Trace:* HLR-008 (Target Memory Write).

*   <a id="LLR-UPDI-07"></a>**LLR-UPDI-07** — `updi_nvm_write_flash()` shall require `word_addr` to be aligned to a 512-byte FLASH page boundary and `len` to be a non-zero multiple of 512. The function shall poll `NVMCTRL_STATUS` BUSY for up to 100 ms before issuing the NVM write command, and poll the BUSY bit for up to 20 ms after each page write. Timeout of either poll shall cause the function to return -1. FLASH write-protection detection shall return `UPDI_ERR_WP`.
    *Trace:* HLR-009 (FLASH Programming), HLR-037 (UPDI Operation Timeout Bounds).

*   <a id="LLR-UPDI-08"></a>**LLR-UPDI-08** — `updi_halt()` shall write 0x01 to `ASI_SYS_CTRL` to request a halt, then poll `ASI_SYS_STATUS` at 1 ms intervals for bit 3 (STOPPED). The function shall return 0 when STOPPED is observed within 50 ms, or -1 if the bit has not been set after 50 polling intervals.
    *Trace:* HLR-010 (Execution Control), HLR-037 (UPDI Operation Timeout Bounds).

*   <a id="LLR-UPDI-09"></a>**LLR-UPDI-09** — `updi_step()` shall issue the UPDI ASI single-instruction-step command, then verify that the STOPPED bit is set in `ASI_SYS_STATUS` before returning. It shall return 0 on success or -1 on timeout or UPDI error.
    *Trace:* HLR-010 (Execution Control).

*   <a id="LLR-UPDI-10"></a>**LLR-UPDI-10** — `updi_run()` shall clear the UPDI ASI halt request and verify that the STOPPED bit in `ASI_SYS_STATUS` is no longer set before returning. It shall return 0 on success or -1 on UPDI error.
    *Trace:* HLR-010 (Execution Control).

*   <a id="LLR-UPDI-11"></a>**LLR-UPDI-11** — `updi_mem_read()` shall be callable while the target CPU is in the RUNNING state (not halted). The function shall not call `updi_halt()` internally, permitting non-intrusive SRAM sampling for the monitor sub-commands and console polling without interrupting firmware execution.
    *Trace:* HLR-011 (Non-Intrusive Background Memory Read).

*   <a id="LLR-UPDI-12"></a>**LLR-UPDI-12** — `updi_console_poll()` shall perform a background `updi_mem_read()` of the avrOS software UART output buffer and return any pending bytes to the caller without halting the CPU. The function shall return the number of bytes read (0 if none pending) or -1 on UPDI error.
    *Trace:* HLR-012 (UPDI Console Bridge).

## 4. src/gdb_rsp.c — GDB Remote Serial Protocol Server

Requirements for RSP packet framing, dispatch, GDB packet handlers, session lifecycle, and socket configuration.

*   <a id="LLR-RSP-01"></a>**LLR-RSP-01** — `rsp_listen()` shall set `SO_REUSEADDR` on the listener socket before calling `bind()`. After `rsp_accept()` returns a connected client socket, the client socket shall have `TCP_NODELAY` set to disable Nagle's algorithm.
    *Trace:* HLR-003 (GDB Listener Startup), HLR-038 (GDB Session Response Latency).

*   <a id="LLR-RSP-02"></a>**LLR-RSP-02** — `rsp_recv_packet()` shall scan incoming bytes discarding pre-packet ACK/NAK characters until a `$` delimiter is received. It shall accumulate the payload with a running XOR checksum until `#` is received, then compare the computed checksum against the two ASCII-hex checksum bytes that follow. On match it shall write `+` to the socket and return the payload length; on mismatch it shall write `-` and return -1.
    *Trace:* HLR-013 (RSP Server Accessibility).

*   <a id="LLR-RSP-03"></a>**LLR-RSP-03** — The `on_read_regs` handler for the `g` packet shall read all 35 AVR CPU registers (R0–R31, SREG, SPL, SPH, and PC) from the target via `updi_mem_read()` and return them as a 70-character hex string in GDB g-packet register order.
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

*   <a id="LLR-RSP-12"></a>**LLR-RSP-12** — `rsp_dispatch()` shall handle `qSupported` inline, responding with a fixed string advertising the supported feature set. It shall handle `qAttached` inline, responding `1` (attached to an existing process). Neither packet requires target interaction or handler table dispatch.
    *Trace:* HLR-019 (RSP Capability Negotiation and Lifecycle).

*   <a id="LLR-RSP-13"></a>**LLR-RSP-13** — The `on_detach` handler for the `D` packet shall call `updi_run()` to resume the target, close the GDB client socket, reset `cfg->gdb_fd` to -1, and allow the event loop to re-enter the listening state. The server process shall not exit.
    *Trace:* HLR-019 (RSP Capability Negotiation and Lifecycle).

*   <a id="LLR-RSP-14"></a>**LLR-RSP-14** — The `on_detach` handler for the `k` (kill) packet shall set `g_quit = 1` to cause the event loop to exit, triggering the full resource teardown sequence defined by LLR-MAIN-07.
    *Trace:* HLR-019 (RSP Capability Negotiation and Lifecycle), HLR-035 (Graceful Shutdown and Resource Release).

*   <a id="LLR-RSP-15"></a>**LLR-RSP-15** — The `on_monitor` handler for `qRcmd` shall pass the raw ASCII-hex-encoded command body directly to `monitor_dispatch()` without pre-decoding it, and relay the return code to the GDB client as `OK` on success (return 0), an O-packet error message on UPDI failure (return -1), or an empty reply on unrecognised sub-command (return -2).
    *Trace:* HLR-029 (Monitor Events Command), HLR-030 (Monitor Queues Command), HLR-031 (Monitor Memory Pool Command).

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

## 6. src/fsm_mapper.c — FSM Virtual Thread Mapper

Requirements for `fsm_build_thread_list()`, `fsm_invalidate()`, `fsm_get_active_thread()`, and `fsm_get_registers()`.

*   <a id="LLR-FSM-01"></a>**LLR-FSM-01** — `fsm_build_thread_list()` shall read `idx->fsm_table_count` consecutive `avros_fsm_entry_t` records from the FLASH address `idx->fsm_table_addr` via `updi_mem_read()`. A UPDI read failure at any point shall cause the function to return -1.
    *Trace:* HLR-024 (FSM Thread Enumeration).

*   <a id="LLR-FSM-02"></a>**LLR-FSM-02** — `fsm_build_thread_list()` shall assign `thread->gdb_id = loop_index + 1` (1-based) to each FSM entry in the order the entries appear in the FLASH table. Thread IDs shall not be reassigned within a debug session, ensuring the same FSM always maps to the same GDB thread ID.
    *Trace:* HLR-024 (FSM Thread Enumeration).

*   <a id="LLR-FSM-03"></a>**LLR-FSM-03** — `fsm_build_thread_list()` shall read the 2-byte SRAM value at `idx->current_fsm_addr` and compare it against each FSM entry's `state_var_sram_addr`. The matching entry shall have `thread->is_active = true` and its `gdb_id` stored in `ctx->active_id`. If no entry matches, `ctx->active_id` shall be set to 0.
    *Trace:* HLR-025 (Active Thread Identification).

*   <a id="LLR-FSM-04"></a>**LLR-FSM-04** — `fsm_get_registers()` shall set the PC field (GDB register index 35, encoded as a 4-byte little-endian value at bytes 68–71 of the g-packet buffer) to `thread->state_fn`. For non-active threads, R0–R31 (indices 0–31) and SREG (index 32) shall be zero-filled. For the active thread, SPL (index 33) and SPH (index 34) shall be read from the target SRAM via `updi_mem_read()`.
    *Trace:* HLR-026 (Virtual Thread Register Frame).

*   <a id="LLR-FSM-05"></a>**LLR-FSM-05** — `fsm_build_thread_list()` shall process at most `FSM_MAX_THREADS` (32) entries from the FSM registration table regardless of `idx->fsm_table_count`. If `idx->fsm_table_count` exceeds `FSM_MAX_THREADS`, the function shall log a diagnostic warning and return `FSM_MAX_THREADS` as the thread count without returning an error to the GDB client.
    *Trace:* HLR-027 (Complete FSM Thread Coverage).

*   <a id="LLR-FSM-06"></a>**LLR-FSM-06** — `fsm_get_registers()` shall not issue any UPDI memory read targeting a non-active thread's stack region. The suspended FSM register frame shall be constructed entirely from the cached `thread->state_fn` value; the SP field shall be zeroed for non-active threads.
    *Trace:* HLR-028 (Stack-Free Thread Model).

## 7. src/monitor.c — avrOS System Introspection

Requirements for `monitor_dispatch()` and its static sub-command helpers `cmd_events()`, `cmd_queues()`, and `cmd_mempool()`.

*   <a id="LLR-MON-01"></a>**LLR-MON-01** — `monitor_dispatch()` shall hex-decode the ASCII-hex-encoded `cmd` string (pairs of hex digit characters) into a plain-text command string before any prefix or sub-command matching is attempted. An odd-length or invalid hex-digit sequence shall be treated as an unrecognised command.
    *Trace:* HLR-029 (Monitor Events Command).

*   <a id="LLR-MON-02"></a>**LLR-MON-02** — `monitor_dispatch()` shall verify that the decoded command string begins with the prefix `"avros "` (case-sensitive, including the trailing space). If the prefix does not match, the function shall send a usage-hint O-packet to the GDB console and return -2.
    *Trace:* HLR-029 (Monitor Events Command), HLR-030 (Monitor Queues Command), HLR-031 (Monitor Memory Pool Command).

*   <a id="LLR-MON-03"></a>**LLR-MON-03** — `cmd_events()` shall call `updi_mem_read()` to read 2 bytes from `idx->event_mask_addr`. For each of the 16 bits (bit 0 through bit 15), it shall append a line of the form `"  event<N>: SET\n"` or `"  event<N>: clear\n"` to the output buffer and send the complete buffer as RSP O-packets.
    *Trace:* HLR-029 (Monitor Events Command).

*   <a id="LLR-MON-04"></a>**LLR-MON-04** — `cmd_queues()` shall call `updi_mem_read()` to read `idx->queue_count` consecutive queue status structures from `idx->queue_table_addr`. For each structure, it shall extract and format the `head`, `tail`, and `count` fields as a one-line entry and send the complete table as RSP O-packets.
    *Trace:* HLR-030 (Monitor Queues Command).

*   <a id="LLR-MON-05"></a>**LLR-MON-05** — `cmd_mempool()` shall call `updi_mem_read()` to read `idx->mempool_count` consecutive pool status structures from `idx->mempool_table_addr`. For each structure, it shall extract and format the `free_count` and `capacity` fields with a percentage utilisation figure and send the complete table as RSP O-packets.
    *Trace:* HLR-031 (Monitor Memory Pool Command).

*   <a id="LLR-MON-06"></a>**LLR-MON-06** — All monitor output shall be assembled into a temporary buffer of at most 512 bytes, hex-encoded (each ASCII byte converted to 2 hex characters per RSP O-packet spec), and transmitted to the GDB client as one or more RSP O-packets via `rsp_send_packet()`.
    *Trace:* HLR-029 (Monitor Events Command), HLR-030 (Monitor Queues Command), HLR-031 (Monitor Memory Pool Command).

*   <a id="LLR-MON-07"></a>**LLR-MON-07** — `monitor_dispatch()` and all of its static helpers (`cmd_events()`, `cmd_queues()`, `cmd_mempool()`) shall use `updi_mem_read()` exclusively for all target memory access. None of these functions shall call `updi_halt()`, ensuring that monitor commands never interrupt firmware execution.
    *Trace:* HLR-011 (Non-Intrusive Background Memory Read), HLR-032 (Introspection Reliability).
