# avrOS-debug

A UPDI-based GDB server that brings native avrOS state-machine awareness to standard IDEs (VS Code, Zed) for modern AVR DA/DB microcontrollers — no JTAG or ICE hardware required.

## Overview

`avr-updi-gdb` bridges the AVR UPDI debug interface to `avr-gdb` over the standard GDB Remote Serial Protocol, using only a USB-to-TTL serial adapter (or Raspberry Pi UART) and a 1kΩ resistor. It automatically parses your ELF to locate avrOS system tables in FLASH, maps them to live SRAM state, and presents each cooperative FSM as a virtual GDB thread — so active and suspended state machines, pending events, queues, and memory pools are all visible directly in your IDE's debugging UI.

## Key Features

- **UPDI protocol bridging** — full physical-layer communication with AVR DA/DB targets over serial + 1kΩ resistor
- **Harvard architecture translation** — automatic ELF parsing to resolve FLASH-resident avrOS tables to their SRAM status bytes
- **FSM virtual threads** — cooperative state machines appear as native threads in the IDE Call Stack pane
- **System introspection** — `monitor avros events`, `monitor avros queues`, and `monitor avros mempool` commands for non-intrusive polling via UPDI background reads
- **Editor-agnostic** — works with any IDE that supports GDB RSP (VS Code via Cortex-Debug, Zed via DAP)

## Platform Support

- Linux and macOS (Windows is not supported in the initial release)
- Targets: AVR DA/DB family microcontrollers running avrOS
