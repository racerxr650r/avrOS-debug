---
name: application-debug
description: "Use when working with avrOSdb debugging. This handles VS Code extension or GDB debugging, such as RSP trace logs logic, debugging connection issues over RSP, UPDI, or TCP. Use when changing application debugging logic inside avrOSdb main.c or gdb_rsp.c or investigating lockups or target side state."
---

# avrOSdb Application Debugging - AI Working Rules

This project implements an RSP gdbserver over UPDI.

## Important States

1. `RspContext`: Holds global debug session state, including `last_stop_cause`, software track (`sw_bp`), and hardware track.
2. `SC_NONE`, `SC_HWBREAK`, `SC_SWBREAK`: These are stop cause states. Wait to emit `last_stop_cause` across `vCont` restarts — it must remain transient and not leak.
3. `fsm`: Real-Time OS thread tracking, parsing out FSM states into fake threads to VS Code. If a native-debug crash occurs, users might add `--no-fsm-threads`.

## Key Files

1. `src/gdb_rsp.c`: Formats and parses GDB packets, translates `vCont`, `continue`, `step`, and parses breakpoints (`Z0`, `Z1`).
2. `src/main.c`: Connects the TCP socket, orchestrates `event_loop`, and clears states when the socket tears down.
3. `tests/hw/gdb_acceptance.py`: Python harness testing End-to-End UPDI/RSP capabilities. Uses `HW_TEST_NVM_CONFIRM=YES` to allow testing.

## Instructions
- Ensure `last_stop_cause` is cleared on TCP disconnect.
- Do NOT trust GDB frontends to not loop over bad state (e.g. infinite unwind).
- Output logs from RSP packets to see the wire communication. Can pass `--log-rsp` to `avrOSdb` and see it print on stdout/stderr, or `AVROSDB_GDB_LOG_RSP=1` in `make hw-test-gdb`.

## References
- `doc/reference/guesswork.md`: Notes and reverse-engineered behaviours regarding UPDI and the AVR hardware debugging interface.
- `doc/reference/OCD.md`: Rules and protocols for managing the AVR On-Chip Debugger (OCD).
- [GDB Remote Serial Protocol (RSP) Docs](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html): The official specification for packets parsed and generated in `src/gdb_rsp.c`.
- `doc/Project.xml`: Single source of truth for the application's spec.
- `doc/SDD.md`: Software Design Document (regenerated from Project.xml).
- `doc/HLRs.md`: High-Level Requirements (regenerated from Project.xml).
- `doc/LLRs.md`: Low-Level Requirements (regenerated from Project.xml).
- `doc/STP.md`: Software Test Plan containing test definitions (regenerated).
- `tests/hw/` and `tests/`: Directories containing the active acceptance and unit tests.
