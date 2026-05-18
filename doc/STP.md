# Software Test Plan

**Version:** 0.1
**Date:** 2026-05-16
**Author(s):** John Anderson

## 1. Introduction

## 2. Test Strategy

## 3. Test Catalogue

Snapshot: **115 test(s)** across
**8 file(s)**.

### 3.1. [tests/test_main.c](../tests/test_main.c)

Role: **unit**. **14 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="parse_args_valid_positional_args_populate_config"></a>`parse_args_valid_positional_args_populate_config` | `LLR-MAIN-01` | Call `parse_args()` with a valid `<serial-device>` and `<elf-file>` and verify that the returned `AppConfig` carries the correct device path and ELF path. |
| 2 | <a id="parse_args_unrecognised_flag_exits_1"></a>`parse_args_unrecognised_flag_exits_1` | `LLR-MAIN-01` | Call `parse_args()` with an unrecognised flag and assert that the process exits with status 1 and that a usage message is present on `stderr`. |
| 3 | <a id="parse_args_default_port_baud_load_when_not_supplied"></a>`parse_args_default_port_baud_load_when_not_supplied` | `LLR-MAIN-02` | Call `parse_args()` with only the two mandatory positional arguments and verify that `cfg.gdb_port == 1234`, `cfg.baud_rate == 115200`, and `cfg.load_flash == false`. |
| 4 | <a id="parse_args_missing_serial_device_exits_1"></a>`parse_args_missing_serial_device_exits_1` | `LLR-MAIN-02` | Call `parse_args()` with no positional arguments and assert that the process exits with status 1 and emits a usage message. |
| 5 | <a id="parse_args_missing_elf_file_exits_1"></a>`parse_args_missing_elf_file_exits_1` | `LLR-MAIN-02` | Call `parse_args()` with only `<serial-device>` and no `<elf-file>` and assert that the process exits with status 1. |
| 6 | <a id="main_updi_open_failure_releases_resources_and_returns_1"></a>`main_updi_open_failure_releases_resources_and_returns_1` | `LLR-MAIN-03` | Inject a mock `updi_open()` that returns -1 and verify that `main()` does not call `rsp_listen()`, releases any already-opened resources, and returns exit code 1. |
| 7 | <a id="main_rsp_listen_failure_closes_updi_and_returns_1"></a>`main_rsp_listen_failure_closes_updi_and_returns_1` | `LLR-MAIN-03` | Inject a mock `rsp_listen()` that returns -1 and verify that `main()` calls `updi_close()` before returning exit code 1. |
| 8 | <a id="main_load_flag_writes_all_pt_load_segments_to_flash"></a>`main_load_flag_writes_all_pt_load_segments_to_flash` | `LLR-MAIN-04` | With `cfg.load = true` and a mock ELF context containing two `PT_LOAD` segments, verify that `main()` calls `updi_nvm_write_flash()` exactly twice, once per segment, before entering `event_loop()`. |
| 9 | <a id="main_load_nvm_write_failure_prints_error_and_returns_1"></a>`main_load_nvm_write_failure_prints_error_and_returns_1` | `LLR-MAIN-04` | With `cfg.load = true` and a mock `updi_nvm_write_flash()` that returns -1, verify that `main()` prints an error to `stderr` and returns exit code 1 without entering `event_loop()`. |
| 10 | <a id="event_loop_uses_single_select_no_pthread_create"></a>`event_loop_uses_single_select_no_pthread_create` | `LLR-MAIN-05` | Instrument `event_loop()` with mock file descriptors and verify that `select()` is called with all three fds (`listen_fd`, `gdb_fd`, `updi_fd`) in the read set, and that no calls to `pthread_create()` occur. |
| 11 | <a id="event_loop_accepts_gdb_client_when_gdb_fd_is_minus1"></a>`event_loop_accepts_gdb_client_when_gdb_fd_is_minus1` | `LLR-MAIN-05` | With `cfg.gdb_fd == -1` and `listen_fd` readable, verify that `event_loop()` calls `accept()` and updates `cfg.gdb_fd`. |
| 12 | <a id="sigint_handler_sets_g_quit_to_1"></a>`sigint_handler_sets_g_quit_to_1` | `LLR-MAIN-06` | Send SIGINT to the test process and verify that the signal handler installed by `main()` sets `g_quit` to 1 without performing any other operation. |
| 13 | <a id="event_loop_exits_immediately_when_g_quit_is_1"></a>`event_loop_exits_immediately_when_g_quit_is_1` | `LLR-MAIN-06` | Set `g_quit = 1` before calling `event_loop()` and verify that the function returns immediately without calling `select()`. |
| 14 | <a id="main_cleanup_closes_gdb_elf_updi_in_order"></a>`main_cleanup_closes_gdb_elf_updi_in_order` | `LLR-MAIN-07` | On a simulated clean shutdown, verify that `main()` calls `rsp_close(gdb_fd)`, `rsp_close(listen_fd)`, `elf_close()`, and `updi_close()` in that exact order, and returns exit code 0. |

### 3.2. [tests/test_updi.c](../tests/test_updi.c)

Role: **unit**. **22 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="updi_open_sets_8e2_raw_half_duplex_via_termios"></a>`updi_open_sets_8e2_raw_half_duplex_via_termios` | `LLR-UPDI-01` | Open a PTY pair, call `updi_open()` on the slave side, and inspect the resulting `termios` structure to confirm `CS8`, `CSTOPB` (2 stop bits), even-parity intent (`PARODD` clear), and raw mode (no `ICANON`/`ECHO`/`ISIG`/`OPOST`). PARENB itself is not asserted because Linux PTYs strip parity on slave reopen; parity is verified on real serial hardware in Phase 3 integration. |
| 2 | <a id="updi_open_returns_minus1_on_device_open_failure"></a>`updi_open_returns_minus1_on_device_open_failure` | `LLR-UPDI-01` | Call `updi_open()` with a path that does not exist and verify the return value is -1. |
| 3 | <a id="updi_open_asserts_wake_byte_then_stcs_ctrlb"></a>`updi_open_asserts_wake_byte_then_stcs_ctrlb` | `LLR-UPDI-02` | Verify that `updi_open()` transmits a single 0x00 wake byte first, then the STCS CTRLB frame (SYNCH `0x55`, opcode `0xC0|ASI_CTRLB = 0xC3`, value `ASI_CTRLB_CCDETDIS = 0x08`) — the first half of the avrdude-matching cold-start sequence (issue #19). |
| 4 | <a id="updi_open_issues_stcs_ctrla_and_ldcs_statusa"></a>`updi_open_issues_stcs_ctrla_and_ldcs_statusa` | `LLR-UPDI-03` | Verify that after STCS CTRLB the SUT writes STCS CTRLA (`0xC2`, value `ASI_CTRLA_IBDLY = 0x80`) and then probes the link with LDCS STATUSA (`0x80|ASI_STATUSA = 0x80`) — the second half of the cold-start sequence (issue #19). |
| 5 | <a id="updi_open_restores_session_baud_after_break"></a>`updi_open_restores_session_baud_after_break` | `LLR-UPDI-02`, `LLR-UPDI-14` | Verify that `updi_open()` leaves the configured session baud rate (B115200) in effect after a successful cold-start sequence — i.e. it does not corrupt the termios baud setting. The PTY responder also feeds the 32-byte SIB payload (`SYNCH`+`0xE6` → `"  AVR P:2D:1-3M2 (A7.KV001.0)\0"`), so a successful run additionally exercises the SIB-read SLEEP-wake step that `updi_open()` issues after the LDCS ASI_STATUSA link probe. |
| 6 | <a id="updi_open_retries_cold_start_3_times_on_no_ack"></a>`updi_open_retries_cold_start_3_times_on_no_ack` | `LLR-UPDI-03` | Simulate a target that never responds and verify that `updi_open()` runs all three cold-start attempts before giving up: attempt 0 emits 1× 0x00 wake byte then STCS+STCS+LDCS at session baud; attempts 1 and 2 each emit 2× 0x00 BREAK bytes (at 300 baud via `tcsetattr`) then STCS+STCS+LDCS at session baud. With no 0x00 byte present anywhere else in the frames, this yields 1 + 2 + 2 = 5 zero bytes on the wire. |
| 7 | <a id="updi_open_returns_minus1_after_3_consecutive_link_failures"></a>`updi_open_returns_minus1_after_3_consecutive_link_failures` | `LLR-UPDI-03` | After 3 failed cold-start attempts (wake + STCS CTRLB + STCS CTRLA + LDCS STATUSA) with no response, verify that `updi_open()` returns -1. |
| 8 | <a id="updi_mem_read_splits_request_larger_than_256_bytes"></a>`updi_mem_read_splits_request_larger_than_256_bytes` | `LLR-UPDI-04` | Request a read of 512 bytes and verify that `updi_mem_read()` issues two consecutive 256-byte three-frame bursts rather than a single oversized transfer. |
| 9 | <a id="updi_mem_read_uses_repeat_ld_auto_increment_sequence"></a>`updi_mem_read_uses_repeat_ld_auto_increment_sequence` | `LLR-UPDI-04` | Capture the bytes written to the UART and verify the three-frame burst: byte 0 = `ST_PTR_WORD` (`0x69`), bytes 1–2 = address little-endian, frame 2 = `REPEAT` (`0xA0`) + count, frame 3 = `LD ptr++` (`0x24`). |
| 10 | <a id="updi_mem_read_calls_select_with_100ms_timeout_before_read"></a>`updi_mem_read_calls_select_with_100ms_timeout_before_read` | `LLR-UPDI-05` | Using a `--wrap=select` shim that records the `timeval` argument, verify that `updi_mem_read()` passes a `timeval` of 0 seconds and 100,000 microseconds before every `read()` call. |
| 11 | <a id="updi_mem_read_returns_minus1_on_select_timeout"></a>`updi_mem_read_returns_minus1_on_select_timeout` | `LLR-UPDI-05` | Inject a wrapped `select()` that returns 0 (timeout) and verify that `updi_mem_read()` immediately returns -1 without calling `read()`. |
| 12 | <a id="updi_mem_write_returns_0_on_success"></a>`updi_mem_write_returns_0_on_success` | `LLR-UPDI-06` | Call `updi_mem_write()` targeting an SRAM address with a loopback PTY that echoes the expected ACK and verify the return value is 0. |
| 13 | <a id="updi_mem_write_returns_minus1_on_updi_nak"></a>`updi_mem_write_returns_minus1_on_updi_nak` | `LLR-UPDI-06` | Inject a responder that returns a non-ACK byte instead of `UPDI_ACK` (0x40) and verify that `updi_mem_write()` returns -1. |
| 14 | <a id="updi_nvm_write_flash_rejects_unaligned_address"></a>`updi_nvm_write_flash_rejects_unaligned_address` | `LLR-UPDI-07` | Call `updi_nvm_write_flash()` with a `word_addr` that is not a multiple of 512 and verify the function returns -1 without issuing any UART traffic. |
| 15 | <a id="updi_nvm_write_flash_rejects_non_multiple_of_512_length"></a>`updi_nvm_write_flash_rejects_non_multiple_of_512_length` | `LLR-UPDI-07` | Call `updi_nvm_write_flash()` with a `len` that is not a non-zero multiple of 512 and verify the function returns -1. |
| 16 | <a id="updi_nvm_write_flash_nvmprog_poll_timeout_returns_minus1"></a>`updi_nvm_write_flash_nvmprog_poll_timeout_returns_minus1` | `LLR-UPDI-07` | Inject a mock target that never sets the NVMPROG bit (`ASI_SYS_STATUS` bit 3) and verify that `updi_nvm_write_flash()` returns -1 after 100 polling iterations. |
| 17 | <a id="updi_nvm_write_flash_per_page_busy_timeout_returns_minus1"></a>`updi_nvm_write_flash_per_page_busy_timeout_returns_minus1` | `LLR-UPDI-07` | Inject a mock target that never clears `NVMCTRL_STATUS` BUSY (bit 0) after a page write and verify that `updi_nvm_write_flash()` returns -1 after 20 polling iterations. |
| 18 | <a id="updi_halt_returns_minus1_stub_until_ocd_layer"></a>`updi_halt_returns_minus1_stub_until_ocd_layer` | `LLR-UPDI-08` | Verify that `updi_halt()` returns -1 with no UART traffic in Phase 2 — it is a stub pending the Phase 3 OCD layer. |
| 19 | <a id="updi_step_returns_minus1_stub_until_ocd_layer"></a>`updi_step_returns_minus1_stub_until_ocd_layer` | `LLR-UPDI-09` | Verify that `updi_step()` returns -1 with no UART traffic in Phase 2 — it is a stub pending the Phase 3 OCD layer. |
| 20 | <a id="updi_run_returns_minus1_stub_until_ocd_layer"></a>`updi_run_returns_minus1_stub_until_ocd_layer` | `LLR-UPDI-10` | Verify that `updi_run()` returns -1 with no UART traffic in Phase 2 — it is a stub pending the Phase 3 OCD layer. |
| 21 | <a id="updi_console_poll_returns_pending_bytes_without_halting"></a>`updi_console_poll_returns_pending_bytes_without_halting` | `LLR-UPDI-11`, `LLR-UPDI-12` | Pre-stuff the master end of the PTY with 4 bytes and verify that `updi_console_poll()` returns 4 and copies the bytes to the output buffer without calling `updi_halt()`. |
| 22 | <a id="updi_console_poll_returns_0_when_output_buffer_empty"></a>`updi_console_poll_returns_0_when_output_buffer_empty` | `LLR-UPDI-12` | With no data pending on the master end, verify that `updi_console_poll()` returns 0 in a non-blocking manner. |

### 3.3. [tests/test_rsp.c](../tests/test_rsp.c)

Role: **unit**. **29 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="rsp_listen_sets_so_reuseaddr_before_bind"></a>`rsp_listen_sets_so_reuseaddr_before_bind` | `LLR-RSP-01` | Verify that `rsp_listen()` calls `setsockopt()` with `SO_REUSEADDR` enabled before calling `bind()`. |
| 2 | <a id="rsp_accept_sets_tcp_nodelay_on_client_socket"></a>`rsp_accept_sets_tcp_nodelay_on_client_socket` | `LLR-RSP-01` | After `rsp_accept()` returns a client socket, verify that `TCP_NODELAY` has been set on that socket via `getsockopt()`. |
| 3 | <a id="rsp_recv_packet_discards_leading_ack_nak_bytes"></a>`rsp_recv_packet_discards_leading_ack_nak_bytes` | `LLR-RSP-02` | Feed a byte stream that begins with `+` and `-` characters before the `$` delimiter and verify that `rsp_recv_packet()` discards them and correctly decodes the payload that follows. |
| 4 | <a id="rsp_recv_packet_sends_plus_on_valid_checksum"></a>`rsp_recv_packet_sends_plus_on_valid_checksum` | `LLR-RSP-02` | Feed a well-formed RSP packet with a correct XOR checksum and verify that `rsp_recv_packet()` writes `+` to the socket and returns the payload length. |
| 5 | <a id="rsp_recv_packet_sends_minus_and_returns_minus1_on_bad_checksum"></a>`rsp_recv_packet_sends_minus_and_returns_minus1_on_bad_checksum` | `LLR-RSP-02` | Feed an RSP packet with a deliberately corrupted checksum and verify that `rsp_recv_packet()` writes `-` to the socket and returns -1. |
| 6 | <a id="on_read_regs_g_returns_78_char_hex_string"></a>`on_read_regs_g_returns_78_char_hex_string` | `LLR-RSP-03` | Invoke the `g` packet handler via `rsp_dispatch()` and verify that the RSP reply payload is exactly 78 hex characters, representing all 36 GDB AVR register values. |
| 7 | <a id="on_read_regs_g_places_pc_little_endian_at_positions_70_77"></a>`on_read_regs_g_places_pc_little_endian_at_positions_70_77` | `LLR-RSP-03` | Inject a known PC value via the mock UPDI and verify that hex characters at positions 70–77 of the `g` reply encode the PC as 4-byte little-endian. |
| 8 | <a id="on_write_regs_G_writes_all_registers_via_updi_mem_write"></a>`on_write_regs_G_writes_all_registers_via_updi_mem_write` | `LLR-RSP-04` | Dispatch a `G` packet containing 78 hex characters and verify that the handler calls `updi_mem_write()` with the decoded register values and returns `OK`. |
| 9 | <a id="on_write_regs_P_writes_single_register_via_updi_mem_write"></a>`on_write_regs_P_writes_single_register_via_updi_mem_write` | `LLR-RSP-04` | Dispatch a `P` packet specifying register index 5 and a new value, and verify that the handler calls `updi_mem_write()` targeting only the R5 SRAM address and returns `OK`. |
| 10 | <a id="on_read_mem_m_calls_updi_mem_read_and_returns_hex"></a>`on_read_mem_m_calls_updi_mem_read_and_returns_hex` | `LLR-RSP-05` | Dispatch an `m addr,4` packet for a SRAM address and verify that the handler calls `updi_mem_read()` with the decoded address and length and returns an 8-character hex string. |
| 11 | <a id="on_write_mem_M_calls_updi_mem_write_for_sram_address"></a>`on_write_mem_M_calls_updi_mem_write_for_sram_address` | `LLR-RSP-06` | Dispatch an `M addr,4:data` packet targeting a SRAM address and verify that the handler calls `updi_mem_write()` and returns `OK`. |
| 12 | <a id="on_write_mem_X_calls_nvm_write_flash_for_flash_address"></a>`on_write_mem_X_calls_nvm_write_flash_for_flash_address` | `LLR-RSP-06` | Dispatch an `X addr,2:data` packet targeting a FLASH address and verify that the handler calls `updi_nvm_write_flash()` and returns `OK`. |
| 13 | <a id="on_insert_bp_writes_break_opcode_and_saves_original_word"></a>`on_insert_bp_writes_break_opcode_and_saves_original_word` | `LLR-RSP-07` | Dispatch a `Z0 addr,2` packet and verify that the handler reads and saves the original 2-byte instruction word, writes the AVR BREAK opcode (0x9598) via `updi_nvm_write_flash()`, and returns `OK`. |
| 14 | <a id="on_insert_bp_duplicate_returns_ok_without_reflash"></a>`on_insert_bp_duplicate_returns_ok_without_reflash` | `LLR-RSP-07` | Dispatch the same `Z0 addr,2` breakpoint insert twice and verify that `updi_nvm_write_flash()` is called only once, and that the second call returns `OK` immediately. |
| 15 | <a id="on_remove_bp_restores_saved_instruction_word"></a>`on_remove_bp_restores_saved_instruction_word` | `LLR-RSP-08` | After inserting a breakpoint with `Z0`, dispatch `z0` for the same address and verify that `updi_nvm_write_flash()` is called with the original saved instruction word. |
| 16 | <a id="on_remove_bp_unknown_address_returns_error_reply"></a>`on_remove_bp_unknown_address_returns_error_reply` | `LLR-RSP-08` | Dispatch `z0` for an address that has no entry in the breakpoint table and verify that the handler returns a GDB error reply. |
| 17 | <a id="on_insert_bp_returns_E08_when_table_full"></a>`on_insert_bp_returns_E08_when_table_full` | `LLR-RSP-09` | Fill all `RSP_MAX_BREAKPOINTS` (16) breakpoint table slots with distinct addresses, then dispatch one more `Z0` insert and verify the response is `E08`. |
| 18 | <a id="on_step_s_calls_updi_step_and_sends_T05_stop_reason"></a>`on_step_s_calls_updi_step_and_sends_T05_stop_reason` | `LLR-RSP-10` | Dispatch an `s` packet and verify that the handler calls `updi_step()` once and sends a stop-reason reply of the form `T05thread:<id>;`. |
| 19 | <a id="on_continue_calls_updi_run_then_fsm_invalidate"></a>`on_continue_calls_updi_run_then_fsm_invalidate` | `LLR-RSP-11` | Dispatch a `c` packet and verify that the handler calls `updi_run()` and then `fsm_invalidate()` in that order before polling for a halt. |
| 20 | <a id="on_continue_rebuilds_thread_list_after_halt_and_sends_stop"></a>`on_continue_rebuilds_thread_list_after_halt_and_sends_stop` | `LLR-RSP-11` | After the simulated halt triggered by `on_continue`, verify that `fsm_build_thread_list()` is called and the handler sends a stop-reason packet to the GDB client. |
| 21 | <a id="rsp_dispatch_qsupported_returns_feature_string_no_target_access"></a>`rsp_dispatch_qsupported_returns_feature_string_no_target_access` | `LLR-RSP-12` | Dispatch a `qSupported` packet and verify that a non-empty feature string is returned and that no UPDI calls are made. |
| 22 | <a id="rsp_dispatch_qattached_returns_1_no_target_access"></a>`rsp_dispatch_qattached_returns_1_no_target_access` | `LLR-RSP-12` | Dispatch a `qAttached` packet and verify the reply is `1` and that no UPDI calls are made. |
| 23 | <a id="on_detach_D_resumes_target_closes_socket_resets_gdb_fd"></a>`on_detach_D_resumes_target_closes_socket_resets_gdb_fd` | `LLR-RSP-13` | Dispatch a `D` packet and verify that the handler calls `updi_run()`, closes the GDB client socket, and sets `cfg->gdb_fd` to -1 without exiting the server process. |
| 24 | <a id="on_kill_k_sets_g_quit_to_1"></a>`on_kill_k_sets_g_quit_to_1` | `LLR-RSP-14` | Dispatch a `k` packet and verify that `g_quit` is set to 1, causing the event loop to exit on its next iteration. |
| 25 | <a id="on_monitor_qRcmd_passes_hex_body_to_monitor_dispatch"></a>`on_monitor_qRcmd_passes_hex_body_to_monitor_dispatch` | `LLR-RSP-15` | Dispatch a `qRcmd` packet with a hex-encoded command body and verify that `monitor_dispatch()` receives the same hex-encoded string without any pre-decoding. |
| 26 | <a id="on_monitor_returns_ok_when_monitor_dispatch_succeeds"></a>`on_monitor_returns_ok_when_monitor_dispatch_succeeds` | `LLR-RSP-15` | Inject a mock `monitor_dispatch()` that returns 0 and verify that the `qRcmd` handler sends `OK` to the GDB client. |
| 27 | <a id="on_monitor_sends_o_packet_error_on_updi_failure"></a>`on_monitor_sends_o_packet_error_on_updi_failure` | `LLR-RSP-15` | Inject a mock `monitor_dispatch()` that returns -1 and verify that the `qRcmd` handler sends an O-packet error message to the GDB console. |
| 28 | <a id="H_packet_stores_thread_id_for_register_operations"></a>`H_packet_stores_thread_id_for_register_operations` | `LLR-RSP-16` | Dispatch `Hg3` (set thread 3 for register operations) and then a `g` packet, and verify that `fsm_get_registers()` is called with the thread entry for GDB thread ID 3. |
| 29 | <a id="H_packet_minus1_and_0_both_map_to_active_fsm_thread"></a>`H_packet_minus1_and_0_both_map_to_active_fsm_thread` | `LLR-RSP-16` | Dispatch `Hg-1` and `Hg0` in turn, each followed by a `g` packet, and verify that both select the active FSM thread's register frame. |

### 3.4. [tests/test_elf.c](../tests/test_elf.c)

Role: **unit**. **14 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="elf_open_accepts_valid_avr_elf32_binary"></a>`elf_open_accepts_valid_avr_elf32_binary` | `LLR-ELF-01` | Call `elf_open()` with a well-formed AVR ELF32 fixture file and verify the return value is non-negative and `ctx.sym_count > 0`. |
| 2 | <a id="elf_open_returns_minus1_on_invalid_elf_magic"></a>`elf_open_returns_minus1_on_invalid_elf_magic` | `LLR-ELF-01` | Call `elf_open()` with a file whose first 4 bytes are not the ELF magic sequence and verify the return value is -1. |
| 3 | <a id="elf_open_returns_minus1_on_wrong_machine_type"></a>`elf_open_returns_minus1_on_wrong_machine_type` | `LLR-ELF-01` | Call `elf_open()` with a valid ELF32 file that has `e_machine != EM_AVR` and verify the return value is -1. |
| 4 | <a id="elf_open_loads_symtab_and_strtab_into_heap_buffers"></a>`elf_open_loads_symtab_and_strtab_into_heap_buffers` | `LLR-ELF-02` | After a successful `elf_open()`, verify that `ctx.symtab` and `ctx.strtab` are non-NULL heap pointers, and that `ctx.sym_count` and `ctx.strtab_size` reflect the actual section sizes. |
| 5 | <a id="elf_open_frees_partial_allocs_and_returns_minus1_on_malloc_failure"></a>`elf_open_frees_partial_allocs_and_returns_minus1_on_malloc_failure` | `LLR-ELF-02` | Inject a mock `malloc()` that fails on the second allocation, call `elf_open()`, and verify the return value is -1 and no heap memory remains allocated. |
| 6 | <a id="elf_find_avros_tables_performs_single_linear_scan"></a>`elf_find_avros_tables_performs_single_linear_scan` | `LLR-ELF-03` | Instrument `elf_find_avros_tables()` with a mock symbol table and verify that each symbol entry is examined at most once regardless of how many avrOS sentinels are present. |
| 7 | <a id="elf_find_avros_tables_populates_all_7_avros_sentinel_fields"></a>`elf_find_avros_tables_populates_all_7_avros_sentinel_fields` | `LLR-ELF-03` | Call `elf_find_avros_tables()` with a fixture ELF that contains all 7 avrOS sentinel symbols (`__start_FSM_TABLE`, `__stop_FSM_TABLE`, `__start_QUE_TABLE`, `__stop_QUE_TABLE`, `__start_EVNT_TABLE`, `__stop_EVNT_TABLE`, `currStateMachine`) and verify every populated field of `AvrOsSymbolIndex` is non-zero. |
| 8 | <a id="elf_flash_addr_applies_vma_minus_base_over_2_formula"></a>`elf_flash_addr_applies_vma_minus_base_over_2_formula` | `LLR-ELF-04` | Call `elf_flash_addr()` with known `vma` and `ctx.flash_base` values and verify the return equals `(vma - flash_base) / 2`. |
| 9 | <a id="elf_flash_addr_all_avros_symbol_addresses_use_word_formula"></a>`elf_flash_addr_all_avros_symbol_addresses_use_word_formula` | `LLR-ELF-04` | After `elf_find_avros_tables()`, verify that every FLASH-resident address stored in `AvrOsSymbolIndex` was computed via `elf_flash_addr()`, not as a raw VMA. |
| 10 | <a id="elf_find_avros_tables_returns_0_on_partial_symbol_match"></a>`elf_find_avros_tables_returns_0_on_partial_symbol_match` | `LLR-ELF-05` | Call `elf_find_avros_tables()` with a fixture ELF containing only 4 of the 8 avrOS sentinels and verify the return value is 0 (not -1). |
| 11 | <a id="elf_find_avros_tables_zero_initialises_absent_symbol_fields"></a>`elf_find_avros_tables_zero_initialises_absent_symbol_fields` | `LLR-ELF-05` | After `elf_find_avros_tables()` on a partial-symbol ELF, verify that each field in `AvrOsSymbolIndex` that corresponds to a missing symbol is zero. |
| 12 | <a id="elf_close_frees_symtab_strtab_and_closes_fd"></a>`elf_close_frees_symtab_strtab_and_closes_fd` | `LLR-ELF-06` | After a successful `elf_open()`, call `elf_close()` and verify that the file descriptor is closed and no memory is leaked (checked via Valgrind or AddressSanitizer). |
| 13 | <a id="elf_close_safe_on_partially_initialised_context"></a>`elf_close_safe_on_partially_initialised_context` | `LLR-ELF-06` | Construct an `ElfContext` with `symtab = NULL`, `strtab` allocated, and `fd = -1`, call `elf_close()`, and verify no crash or double-free occurs. |
| 14 | <a id="elf_open_sets_flash_base_and_sram_base_from_pt_load_segments"></a>`elf_open_sets_flash_base_and_sram_base_from_pt_load_segments` | `LLR-ELF-08` | After a successful `elf_open()` on the full fixture, verify that `ctx.flash_base` equals the VMA of the first `PT_LOAD` segment (0x00000000 for the fixture) and `ctx.sram_base` equals the VMA of the second `PT_LOAD` segment (0x00804000 for the AVR128DA28 fixture). |

### 3.5. [tests/test_fsm.c](../tests/test_fsm.c)

Role: **unit**. **11 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="fsm_build_thread_list_reads_fsm_table_from_flash_via_updi"></a>`fsm_build_thread_list_reads_fsm_table_from_flash_via_updi` | `LLR-FSM-01` | Verify that `fsm_build_thread_list()` calls `updi_mem_read()` targeting `idx->fsm_table_addr` and reads `idx->fsm_table_count` consecutive 9-byte `fsmStateMachineDescr_t` records. |
| 2 | <a id="fsm_build_thread_list_returns_minus1_on_updi_failure"></a>`fsm_build_thread_list_returns_minus1_on_updi_failure` | `LLR-FSM-01` | Inject a mock `updi_mem_read()` that returns -1 and verify that `fsm_build_thread_list()` propagates the error by returning -1. |
| 3 | <a id="fsm_build_thread_list_assigns_1_based_thread_ids_in_order"></a>`fsm_build_thread_list_assigns_1_based_thread_ids_in_order` | `LLR-FSM-02` | Build a thread list from 3 FSM entries and verify that the resulting `gdb_id` values are 1, 2, and 3, in the order the entries appear in the FLASH table. |
| 4 | <a id="fsm_build_thread_list_thread_ids_stable_across_calls"></a>`fsm_build_thread_list_thread_ids_stable_across_calls` | `LLR-FSM-02` | Call `fsm_build_thread_list()` twice with the same FSM table content and verify that each FSM entry maps to the same `gdb_id` on both calls. |
| 5 | <a id="fsm_build_thread_list_sets_active_thread_from_current_fsm_ptr"></a>`fsm_build_thread_list_sets_active_thread_from_current_fsm_ptr` | `LLR-FSM-03` | Inject a mock `currStateMachine` SRAM value matching the second FSM entry's `stateMachine` pointer and verify that only that entry has `is_active == true` and `ctx->active_id == 2`. |
| 6 | <a id="fsm_build_thread_list_sets_active_id_0_when_no_entry_matches"></a>`fsm_build_thread_list_sets_active_id_0_when_no_entry_matches` | `LLR-FSM-03` | Inject a `current_fsm` SRAM value that does not match any registered FSM and verify that `ctx->active_id == 0`. |
| 7 | <a id="fsm_get_registers_places_state_fn_as_pc_at_hex_positions_70_77"></a>`fsm_get_registers_places_state_fn_as_pc_at_hex_positions_70_77` | `LLR-FSM-04` | Call `fsm_get_registers()` for a thread whose `state_fn = 0xABCD` and verify that the 78-character g-packet buffer contains the little-endian encoding of 0xABCD at hex character positions 70–77. |
| 8 | <a id="fsm_get_registers_non_active_r0_r31_sreg_spl_sph_all_zero"></a>`fsm_get_registers_non_active_r0_r31_sreg_spl_sph_all_zero` | `LLR-FSM-04` | Call `fsm_get_registers()` for a non-active thread and verify that hex positions 0–69 of the 78-character buffer are all `'0'`. |
| 9 | <a id="fsm_get_registers_active_thread_reads_live_sreg_spl_sph"></a>`fsm_get_registers_active_thread_reads_live_sreg_spl_sph` | `LLR-FSM-04` | Call `fsm_get_registers()` for the active thread with a mock that returns known SREG, SPL, and SPH values, and verify they appear at hex positions 64–65, 66–67, and 68–69 respectively. |
| 10 | <a id="fsm_build_thread_list_caps_at_32_entries_and_logs_warning"></a>`fsm_build_thread_list_caps_at_32_entries_and_logs_warning` | `LLR-FSM-05` | Inject an FSM table with 33 entries and verify that `fsm_build_thread_list()` returns `FSM_MAX_THREADS` (32) and emits a diagnostic warning without returning an error. |
| 11 | <a id="fsm_get_registers_non_active_no_updi_read_of_stack"></a>`fsm_get_registers_non_active_no_updi_read_of_stack` | `LLR-FSM-06` | Call `fsm_get_registers()` for a non-active thread and verify that `updi_mem_read()` is never called with any stack-region address. |

### 3.6. [tests/test_monitor.c](../tests/test_monitor.c)

Role: **unit**. **10 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="monitor_dispatch_hex_decodes_cmd_before_prefix_matching"></a>`monitor_dispatch_hex_decodes_cmd_before_prefix_matching` | `LLR-MON-01` | Pass `monitor_dispatch()` a hex-encoded `"avros events"` command and verify the function routes to `cmd_events()`, confirming that decoding occurs before prefix matching. |
| 2 | <a id="monitor_dispatch_treats_invalid_hex_sequence_as_unrecognised"></a>`monitor_dispatch_treats_invalid_hex_sequence_as_unrecognised` | `LLR-MON-01` | Pass `monitor_dispatch()` an odd-length hex string and verify it returns -2 without crashing or producing partial output. |
| 3 | <a id="monitor_dispatch_rejects_cmd_without_avros_space_prefix"></a>`monitor_dispatch_rejects_cmd_without_avros_space_prefix` | `LLR-MON-02` | Pass a hex-encoded command that decodes to `"other events"` and verify `monitor_dispatch()` returns -2. |
| 4 | <a id="monitor_dispatch_sends_usage_hint_o_packet_on_bad_prefix"></a>`monitor_dispatch_sends_usage_hint_o_packet_on_bad_prefix` | `LLR-MON-02` | Pass a command with an unrecognised prefix and verify that `monitor_dispatch()` sends at least one RSP O-packet containing a usage-hint string before returning -2. |
| 5 | <a id="cmd_events_reads_event_count_descriptors_from_evnt_table"></a>`cmd_events_reads_event_count_descriptors_from_evnt_table` | `LLR-MON-03` | Dispatch `"avros events"` and verify that `updi_mem_read()` is called with address `idx->event_table_addr` and a length equal to `idx->event_count` × 4 bytes (`evntDescriptor_t` size). |
| 6 | <a id="cmd_events_reports_name_and_status_value_per_descriptor"></a>`cmd_events_reports_name_and_status_value_per_descriptor` | `LLR-MON-03` | Inject two `evntDescriptor_t` records pointing at FLASH names `"RX"`, `"TX"` and 1-byte SRAM status values `0x01`, `0x00` and verify the decoded O-packet text contains both names and both status bytes formatted as `0xXX`. |
| 7 | <a id="cmd_events_output_is_hex_encoded_o_packet"></a>`cmd_events_output_is_hex_encoded_o_packet` | `LLR-MON-06` | Dispatch `"avros events"` and verify the single captured `rsp_send_packet()` payload begins with `'O'` and every subsequent character is an uppercase hex digit (`[0-9A-F]`), with an overall odd length. |
| 8 | <a id="cmd_queues_reads_queue_count_descriptors_from_que_table"></a>`cmd_queues_reads_queue_count_descriptors_from_que_table` | `LLR-MON-04` | Dispatch `"avros queues"` and verify that `updi_mem_read()` is called with `idx->queue_table_addr` and a length equal to `idx->queue_count` × 10 bytes (`queDescriptor_t` size). |
| 9 | <a id="cmd_queues_formats_capacity_and_sizeofelement_for_each_entry"></a>`cmd_queues_formats_capacity_and_sizeofelement_for_each_entry` | `LLR-MON-04` | Inject two `queDescriptor_t` records with `(capacity, sizeOfElement)` of `(8, 1)` and `(16, 4)` respectively and verify the decoded O-packet text contains `"capacity=8 sizeOfElement=1"` and `"capacity=16 sizeOfElement=4"`. |
| 10 | <a id="monitor_dispatch_and_helpers_never_call_updi_halt"></a>`monitor_dispatch_and_helpers_never_call_updi_halt` | `LLR-MON-07` | Execute both sub-commands (`events`, `queues`) through `monitor_dispatch()` and verify that `updi_halt()` is never called by inspecting the mock call log. |

### 3.7. [tests/test_integration.c](../tests/test_integration.c)

Role: **integration**. **4 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="integration_server_ready_within_2_seconds_of_client_connect"></a>`integration_server_ready_within_2_seconds_of_client_connect` | — | Launch `avr-updi-gdb` against a loopback UPDI stub and a fixture ELF, connect a GDB client, and measure the elapsed time from TCP accept to the first valid RSP response. Assert the elapsed time is less than 2.0 seconds. |
| 2 | <a id="integration_server_emits_only_standard_rsp_no_ide_extensions"></a>`integration_server_emits_only_standard_rsp_no_ide_extensions` | — | Capture all TCP traffic from a full debug session (connect, register read, memory read, continue, breakpoint, detach) and verify that every server-originated packet conforms to the GDB RSP specification with no DAP, Cortex-Debug, or other IDE-specific packet types present. |
| 3 | <a id="build_compiles_clean_on_linux_with_c99_and_posix"></a>`build_compiles_clean_on_linux_with_c99_and_posix` | `LLR-ELF-07` | Execute `make` with `CC=gcc CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L"` and assert that the build exits with status 0 and produces no compiler warnings or errors on a Linux x86-64 host. |
| 4 | <a id="runtime_links_only_libc_no_heavyweight_deps"></a>`runtime_links_only_libc_no_heavyweight_deps` | — | Run `ldd avr-updi-gdb` on the linked binary and verify that the only shared-library dependency is `libc.so`. Assert that no Java runtime, Python interpreter, Electron libraries, or other non-POSIX dependencies appear. |

### 3.8. [tests/test_install.c](../tests/test_install.c)

Role: **integration**. **8 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="check_tools_exits_nonzero_when_required_tool_is_absent"></a>`check_tools_exits_nonzero_when_required_tool_is_absent` | `LLR-INST-01` | Temporarily shadow a required tool (e.g. `avr-gcc`) with a dummy wrapper that is not executable, then run `make check-tools` and verify it exits with a non-zero status and prints a diagnostic to stderr naming the missing tool. |
| 2 | <a id="make_install_places_binary_at_prefix_bin"></a>`make_install_places_binary_at_prefix_bin` | `LLR-INST-02` | Run `make install PREFIX=/tmp/aod_test_$$` (using a temporary directory as the prefix) and verify that the file `$(PREFIX)/bin/avr-updi-gdb` exists and is executable after the target completes. |
| 3 | <a id="make_install_places_man_page_at_prefix_man1"></a>`make_install_places_man_page_at_prefix_man1` | `LLR-INST-02`, `LLR-INST-05` | After `make install PREFIX=...`, verify that the file `$(PREFIX)/share/man/man1/avr-updi-gdb.1` exists and that `man -l` parses it without error. |
| 4 | <a id="make_uninstall_removes_all_installed_files"></a>`make_uninstall_removes_all_installed_files` | `LLR-INST-03` | After `make install PREFIX=...`, run `make uninstall PREFIX=...` and verify that both `$(PREFIX)/bin/avr-updi-gdb` and `$(PREFIX)/share/man/man1/avr-updi-gdb.1` no longer exist. |
| 5 | <a id="user_manual_exists_and_contains_required_sections"></a>`user_manual_exists_and_contains_required_sections` | `LLR-INST-04` | Read `doc/UserManual.md` and verify the file exists and contains all required section headings: Prerequisites, Build, Connection Wiring, Usage (or Options), and at least one Example. |
| 6 | <a id="make_bundle_produces_deb_package"></a>`make_bundle_produces_deb_package` | `LLR-INST-06` | Run `make bundle VERSION=0.1.0` and verify that `dist/avr-updi-gdb_0.1.0_amd64.deb` exists, that `dpkg-deb --info` exits 0, and that `dpkg-deb --contents` lists both `./usr/bin/avr-updi-gdb` and `./usr/share/man/man1/avr-updi-gdb.1`. |
| 7 | <a id="make_bundle_produces_rpm_package"></a>`make_bundle_produces_rpm_package` | `LLR-INST-07` | Run `make bundle VERSION=0.1.0` and verify that `dist/avr-updi-gdb-0.1.0-1.x86_64.rpm` exists and that `rpm -qp --list` exits 0 and lists both `/usr/bin/avr-updi-gdb` and `/usr/share/man/man1/avr-updi-gdb.1`. |
| 8 | <a id="make_bundle_produces_homebrew_formula"></a>`make_bundle_produces_homebrew_formula` | `LLR-INST-08` | Run `make bundle VERSION=0.1.0` and verify that `dist/avr-updi-gdb.rb` exists, is valid Ruby syntax (parseable by `ruby -c`), and contains the required fields: `desc`, `url`, `sha256`, `version`, and an `install` block. |

### 3.9. [tests/test_device.c](../tests/test_device.c)

Role: **unit**. **6 test(s).**

| # | Test | Verifies | Purpose |
| - | ---- | -------- | ------- |
| 1 | <a id="parse_args_accepts_device_flag_without_elf_operand"></a>`parse_args_accepts_device_flag_without_elf_operand` | `LLR-MAIN-08` | Invoke `parse_args()` with `argv = {"avr-updi-gdb", "--device", "/dev/ttyUSB0"}` and assert `cfg.device_info == true`, `cfg.serial_device == "/dev/ttyUSB0"`, `cfg.elf_path == NULL`, and `parse_args()` returns without exiting. |
| 2 | <a id="parse_args_rejects_device_combined_with_load"></a>`parse_args_rejects_device_combined_with_load` | `LLR-MAIN-08` | Fork a subprocess that calls `parse_args()` with `--device --load /dev/ttyUSB0 fw.elf`; assert the child exits with status 1 and that captured stderr contains `"--device is mutually exclusive with --load"`. |
| 3 | <a id="updi_read_device_info_returns_sigrow_and_asi_bytes"></a>`updi_read_device_info_returns_sigrow_and_asi_bytes` | `LLR-UPDI-13` | Open a PTY pair; in a helper thread script the slave end to echo half-duplex bytes and respond to a SIGROW read at `0x1100`-`0x1119` with canned signature `0x1E 0x97 0x0A`, REVID `0xA6`, and a 10-byte serial number, and to LDCS reads of SYS_STATUS / KEY_STATUS / STATUSB. Call `updi_read_device_info()` and assert all struct fields match the canned values and that the function returns 0 with `info.fail_op == NULL`. |
| 4 | <a id="updi_read_device_info_reports_failed_step_on_nak"></a>`updi_read_device_info_reports_failed_step_on_nak` | `LLR-UPDI-13` | Drive the PTY harness to return an unexpected byte in place of the SIGROW response. Assert `updi_read_device_info()` returns -1, `info.fail_op` equals the string `"sigrow"`, and `info.fail_errno` is negative. |
| 5 | <a id="run_device_mode_prints_report_to_stdout"></a>`run_device_mode_prints_report_to_stdout` | `LLR-MAIN-09` | Capture `stdout` from `run_device_mode()` driven by the PTY harness with signature `0x1E 0x97 0x0A`. Assert the captured output contains the literal lines `Serial device:`, `Baud rate:`, `Signature:`, `Family:`, `Revision:`, `Serial:`, and `UPDI status:` (case-sensitive prefix match). |
| 6 | <a id="device_mode_does_not_call_rsp_listen"></a>`device_mode_does_not_call_rsp_listen` | `LLR-MAIN-09` | Link the test binary with a stub `rsp_listen()` that flags a global. Invoke `run_device_mode()` to completion and assert the flag remains clear, proving the diagnostic path bypasses GDB listener startup. |

## 4. LLR Coverage Matrix

Every LLR in [LLRs.md](LLRs.md) and the test(s) that verify it.
LLRs marked **(no direct test)** are exercised transitively or
verified by code review — see
[Traceability.md](Traceability.md) for the per-LLR justification.

| LLR | Function | HLR(s) | Verifying Test(s) |
| --- | -------- | ------ | ----------------- |
| `LLR-MAIN-01` | `main` | `HLR-001` | `parse_args_valid_positional_args_populate_config`, `parse_args_unrecognised_flag_exits_1` |
| `LLR-MAIN-02` | `main` | `HLR-001` | `parse_args_default_port_baud_load_when_not_supplied`, `parse_args_missing_serial_device_exits_1`, `parse_args_missing_elf_file_exits_1` |
| `LLR-MAIN-03` | `main` | `HLR-002`, `HLR-003` | `main_updi_open_failure_releases_resources_and_returns_1`, `main_rsp_listen_failure_closes_updi_and_returns_1` |
| `LLR-MAIN-04` | `main` | `HLR-004` | `main_load_flag_writes_all_pt_load_segments_to_flash`, `main_load_nvm_write_failure_prints_error_and_returns_1` |
| `LLR-MAIN-05` | `main` | `HLR-003`, `HLR-039` | `event_loop_uses_single_select_no_pthread_create`, `event_loop_accepts_gdb_client_when_gdb_fd_is_minus1` |
| `LLR-MAIN-06` | `main` | `HLR-035`, `HLR-039` | `sigint_handler_sets_g_quit_to_1`, `event_loop_exits_immediately_when_g_quit_is_1` |
| `LLR-MAIN-07` | `main` | `HLR-035` | `main_cleanup_closes_gdb_elf_updi_in_order` |
| `LLR-MAIN-08` | `main` | `HLR-044` | `parse_args_accepts_device_flag_without_elf_operand`, `parse_args_rejects_device_combined_with_load` |
| `LLR-MAIN-09` | `main` | `HLR-044` | `run_device_mode_prints_report_to_stdout`, `device_mode_does_not_call_rsp_listen` |
| `LLR-UPDI-01` | `updi` | `HLR-006` | `updi_open_sets_8e2_raw_half_duplex_via_termios`, `updi_open_returns_minus1_on_device_open_failure` |
| `LLR-UPDI-02` | `updi` | `HLR-006`, `HLR-036` | `updi_open_asserts_wake_byte_then_stcs_ctrlb`, `updi_open_restores_session_baud_after_break` |
| `LLR-UPDI-03` | `updi` | `HLR-036` | `updi_open_issues_stcs_ctrla_and_ldcs_statusa`, `updi_open_retries_cold_start_3_times_on_no_ack`, `updi_open_returns_minus1_after_3_consecutive_link_failures` |
| `LLR-UPDI-04` | `updi` | `HLR-007`, `HLR-008` | `updi_mem_read_splits_request_larger_than_256_bytes`, `updi_mem_read_uses_repeat_ld_auto_increment_sequence` |
| `LLR-UPDI-05` | `updi` | `HLR-007`, `HLR-037` | `updi_mem_read_calls_select_with_100ms_timeout_before_read`, `updi_mem_read_returns_minus1_on_select_timeout` |
| `LLR-UPDI-06` | `updi` | `HLR-008` | `updi_mem_write_returns_0_on_success`, `updi_mem_write_returns_minus1_on_updi_nak` |
| `LLR-UPDI-07` | `updi` | `HLR-009`, `HLR-037` | `updi_nvm_write_flash_rejects_unaligned_address`, `updi_nvm_write_flash_rejects_non_multiple_of_512_length`, `updi_nvm_write_flash_nvmprog_poll_timeout_returns_minus1`, `updi_nvm_write_flash_per_page_busy_timeout_returns_minus1` |
| `LLR-UPDI-08` | `updi` | `HLR-010` | `updi_halt_returns_minus1_stub_until_ocd_layer` |
| `LLR-UPDI-09` | `updi` | `HLR-010` | `updi_step_returns_minus1_stub_until_ocd_layer` |
| `LLR-UPDI-10` | `updi` | `HLR-010` | `updi_run_returns_minus1_stub_until_ocd_layer` |
| `LLR-UPDI-11` | `updi` | `HLR-011` | `updi_console_poll_returns_pending_bytes_without_halting` |
| `LLR-UPDI-12` | `updi` | `HLR-012` | `updi_console_poll_returns_pending_bytes_without_halting`, `updi_console_poll_returns_0_when_output_buffer_empty` |
| `LLR-UPDI-13` | `updi` | `HLR-044` | `updi_read_device_info_returns_sigrow_and_asi_bytes`, `updi_read_device_info_reports_failed_step_on_nak` |
| `LLR-UPDI-14` | `updi` | `HLR-006`, `HLR-036` | `updi_open_restores_session_baud_after_break` |
| `LLR-RSP-01` | `rsp` | `HLR-003`, `HLR-038` | `rsp_listen_sets_so_reuseaddr_before_bind`, `rsp_accept_sets_tcp_nodelay_on_client_socket` |
| `LLR-RSP-02` | `rsp` | `HLR-013` | `rsp_recv_packet_discards_leading_ack_nak_bytes`, `rsp_recv_packet_sends_plus_on_valid_checksum`, `rsp_recv_packet_sends_minus_and_returns_minus1_on_bad_checksum` |
| `LLR-RSP-03` | `rsp` | `HLR-014` | `on_read_regs_g_returns_78_char_hex_string`, `on_read_regs_g_places_pc_little_endian_at_positions_70_77` |
| `LLR-RSP-04` | `rsp` | `HLR-014` | `on_write_regs_G_writes_all_registers_via_updi_mem_write`, `on_write_regs_P_writes_single_register_via_updi_mem_write` |
| `LLR-RSP-05` | `rsp` | `HLR-015` | `on_read_mem_m_calls_updi_mem_read_and_returns_hex` |
| `LLR-RSP-06` | `rsp` | `HLR-015` | `on_write_mem_M_calls_updi_mem_write_for_sram_address`, `on_write_mem_X_calls_nvm_write_flash_for_flash_address` |
| `LLR-RSP-07` | `rsp` | `HLR-016` | `on_insert_bp_writes_break_opcode_and_saves_original_word`, `on_insert_bp_duplicate_returns_ok_without_reflash` |
| `LLR-RSP-08` | `rsp` | `HLR-016` | `on_remove_bp_restores_saved_instruction_word`, `on_remove_bp_unknown_address_returns_error_reply` |
| `LLR-RSP-09` | `rsp` | `HLR-016` | `on_insert_bp_returns_E08_when_table_full` |
| `LLR-RSP-10` | `rsp` | `HLR-017` | `on_step_s_calls_updi_step_and_sends_T05_stop_reason` |
| `LLR-RSP-11` | `rsp` | `HLR-018` | `on_continue_calls_updi_run_then_fsm_invalidate`, `on_continue_rebuilds_thread_list_after_halt_and_sends_stop` |
| `LLR-RSP-12` | `rsp` | `HLR-019` | `rsp_dispatch_qsupported_returns_feature_string_no_target_access`, `rsp_dispatch_qattached_returns_1_no_target_access` |
| `LLR-RSP-13` | `rsp` | `HLR-019` | `on_detach_D_resumes_target_closes_socket_resets_gdb_fd` |
| `LLR-RSP-14` | `rsp` | `HLR-019`, `HLR-035` | `on_kill_k_sets_g_quit_to_1` |
| `LLR-RSP-15` | `rsp` | `HLR-029`, `HLR-030` | `on_monitor_qRcmd_passes_hex_body_to_monitor_dispatch`, `on_monitor_returns_ok_when_monitor_dispatch_succeeds`, `on_monitor_sends_o_packet_error_on_updi_failure` |
| `LLR-RSP-16` | `rsp` | `HLR-025` | `H_packet_stores_thread_id_for_register_operations`, `H_packet_minus1_and_0_both_map_to_active_fsm_thread` |
| `LLR-ELF-01` | `elf` | `HLR-021` | `elf_open_accepts_valid_avr_elf32_binary`, `elf_open_returns_minus1_on_invalid_elf_magic`, `elf_open_returns_minus1_on_wrong_machine_type` |
| `LLR-ELF-02` | `elf` | `HLR-021`, `HLR-040` | `elf_open_loads_symtab_and_strtab_into_heap_buffers`, `elf_open_frees_partial_allocs_and_returns_minus1_on_malloc_failure` |
| `LLR-ELF-03` | `elf` | `HLR-021` | `elf_find_avros_tables_performs_single_linear_scan`, `elf_find_avros_tables_populates_all_7_avros_sentinel_fields` |
| `LLR-ELF-04` | `elf` | `HLR-022` | `elf_flash_addr_applies_vma_minus_base_over_2_formula`, `elf_flash_addr_all_avros_symbol_addresses_use_word_formula` |
| `LLR-ELF-05` | `elf` | `HLR-023` | `elf_find_avros_tables_returns_0_on_partial_symbol_match`, `elf_find_avros_tables_zero_initialises_absent_symbol_fields` |
| `LLR-ELF-06` | `elf` | `HLR-040` | `elf_close_frees_symtab_strtab_and_closes_fd`, `elf_close_safe_on_partially_initialised_context` |
| `LLR-ELF-07` | `elf` | `HLR-033` | `build_compiles_clean_on_linux_with_c99_and_posix` |
| `LLR-ELF-08` | `elf` | `HLR-021`, `HLR-022` | `elf_open_sets_flash_base_and_sram_base_from_pt_load_segments` |
| `LLR-FSM-01` | `fsm` | `HLR-024` | `fsm_build_thread_list_reads_fsm_table_from_flash_via_updi`, `fsm_build_thread_list_returns_minus1_on_updi_failure` |
| `LLR-FSM-02` | `fsm` | `HLR-024` | `fsm_build_thread_list_assigns_1_based_thread_ids_in_order`, `fsm_build_thread_list_thread_ids_stable_across_calls` |
| `LLR-FSM-03` | `fsm` | `HLR-025` | `fsm_build_thread_list_sets_active_thread_from_current_fsm_ptr`, `fsm_build_thread_list_sets_active_id_0_when_no_entry_matches` |
| `LLR-FSM-04` | `fsm` | `HLR-026` | `fsm_get_registers_places_state_fn_as_pc_at_hex_positions_70_77`, `fsm_get_registers_non_active_r0_r31_sreg_spl_sph_all_zero`, `fsm_get_registers_active_thread_reads_live_sreg_spl_sph` |
| `LLR-FSM-05` | `fsm` | `HLR-027` | `fsm_build_thread_list_caps_at_32_entries_and_logs_warning` |
| `LLR-FSM-06` | `fsm` | `HLR-028` | `fsm_get_registers_non_active_no_updi_read_of_stack` |
| `LLR-MON-01` | `monitor` | `HLR-029` | `monitor_dispatch_hex_decodes_cmd_before_prefix_matching`, `monitor_dispatch_treats_invalid_hex_sequence_as_unrecognised` |
| `LLR-MON-02` | `monitor` | `HLR-029`, `HLR-030` | `monitor_dispatch_rejects_cmd_without_avros_space_prefix`, `monitor_dispatch_sends_usage_hint_o_packet_on_bad_prefix` |
| `LLR-MON-03` | `monitor` | `HLR-029` | `cmd_events_reads_event_count_descriptors_from_evnt_table`, `cmd_events_reports_name_and_status_value_per_descriptor` |
| `LLR-MON-04` | `monitor` | `HLR-030` | `cmd_queues_reads_queue_count_descriptors_from_que_table`, `cmd_queues_formats_capacity_and_sizeofelement_for_each_entry` |
| `LLR-MON-06` | `monitor` | `HLR-029`, `HLR-030` | `cmd_events_output_is_hex_encoded_o_packet` |
| `LLR-MON-07` | `monitor` | `HLR-011`, `HLR-032` | `monitor_dispatch_and_helpers_never_call_updi_halt` |
| `LLR-INST-01` | `inst` | `HLR-041` | `check_tools_exits_nonzero_when_required_tool_is_absent` |
| `LLR-INST-02` | `inst` | `HLR-041` | `make_install_places_binary_at_prefix_bin`, `make_install_places_man_page_at_prefix_man1` |
| `LLR-INST-03` | `inst` | `HLR-041` | `make_uninstall_removes_all_installed_files` |
| `LLR-INST-04` | `inst` | `HLR-042` | `user_manual_exists_and_contains_required_sections` |
| `LLR-INST-05` | `inst` | `HLR-042` | `make_install_places_man_page_at_prefix_man1` |
| `LLR-INST-06` | `inst` | `HLR-043` | `make_bundle_produces_deb_package` |
| `LLR-INST-07` | `inst` | `HLR-043` | `make_bundle_produces_rpm_package` |
| `LLR-INST-08` | `inst` | `HLR-043` | `make_bundle_produces_homebrew_formula` |
