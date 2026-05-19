# avrOS-debug

A UPDI-based GDB server that brings native avrOS state-machine awareness to standard IDEs (VS Code, Zed) for modern AVR DA/DB microcontrollers — no JTAG or ICE hardware required.

## Overview

`avr-updi-gdb` bridges the AVR UPDI debug interface to `avr-gdb` over the standard GDB Remote Serial Protocol, using only a USB-to-TTL serial adapter (or Raspberry Pi UART) and a 1kΩ resistor. It automatically parses your ELF to locate avrOS system tables in FLASH, maps them to live SRAM state, and presents each cooperative FSM as a virtual GDB thread — so active and suspended state machines, pending events, queues, and memory pools are all visible directly in your IDE's debugging UI.

## Key Features

- **UPDI protocol bridging** — full physical-layer communication with AVR DA/DB targets over serial + 1kΩ resistor
- **On-chip debug (OCD) run control** — run, halt, single-step, and register access are performed through the AVR-Dx OCD controller over UPDI; no flash patching of the `BREAK` opcode, so NVM state is preserved across debug sessions
- **Hardware breakpoints** — both `break` (Z0) and `hbreak` (Z1) requests from GDB are routed to the two on-silicon comparators; a third simultaneous breakpoint is reported back as `E08`
- **Asynchronous interrupt** — Ctrl-C in GDB halts a running target via OCD STOP and reports `SIGINT` (`T02`)
- **Clean detach** — `detach` releases both HW comparators in silicon and lets the CPU run free before closing the socket
- **NVM programming** — `--load` programs every `PT_LOAD` segment of the ELF to the correct AVR-Dx NVM kind: FLASH (`.text`/`.data`), EEPROM, USERROW, FUSES, and LOCK. SIGROW segments are skipped (read-only). `--erase` performs a chip-erase prior to load, and is required when programming LOCK; `--allow-lock-updi` is required to write any LOCK pattern other than the unlock value `0x5CC5C55C` (every other 4-byte pattern risks permanently disabling UPDI).
- **Link diagnostics** — `--device` performs a one-shot, non-destructive read of SIGROW signature and ASI status registers and exits without starting a listener
- **Harvard architecture translation** — automatic ELF parsing to resolve FLASH-resident avrOS tables to their SRAM status bytes
- **FSM virtual threads** — cooperative state machines appear as native threads in the IDE Call Stack pane
- **System introspection** — `monitor avros events`, `monitor avros queues`, and `monitor avros mempool` commands for non-intrusive polling via UPDI background reads
- **Editor-agnostic** — works with any IDE that supports GDB RSP (VS Code via Cortex-Debug, Zed via DAP)

## Platform Support

- Linux (macOS is not supported in the initial release)
- Targets: AVR-Dx family microcontrollers running avrOS

## Quick Start

```bash
make                                          # builds build/avr-updi-gdb
build/avr-updi-gdb /dev/ttyUSB0 firmware.elf  # start GDB server on :1234
avr-gdb firmware.elf -ex 'target remote :1234'
```

See the [User Manual](doc/UserManual.md) for wiring, full CLI reference,
debugging walkthroughs (HW breakpoints, Ctrl-C, detach), VS Code
integration, and troubleshooting.

## Documentation

- [User Manual](doc/UserManual.md) — installation, wiring, CLI, debugging examples
- [Software Design Document](doc/SDD.md) — architecture and module-level design
- [High-Level Requirements](doc/HLRs.md) / [Low-Level Requirements](doc/LLRs.md)
- [Software Test Plan](doc/STP.md) / [Traceability Matrix](doc/Traceability.md)
- [Product Vision](doc/PVD.md)
