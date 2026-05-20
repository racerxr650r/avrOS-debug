# Product Vision Document: avrOS UPDI GDB Server (avrOSdb)

**Version:** 0.1
**Date:** 2026-04-26
**Author(s):** <Your name(s)>

## 1. Purpose

This Product Vision Document (PVD) defines *why* `avrOSdb` exists, *who* it is for, *what* problem it solves, and the *measurable outcomes* that determine whether it is succeeding. It sits above the Software Design Document (SDD), High-Level Requirements (HLRs), Low-Level Requirements (LLRs), and Software Test Plan (STP), and is the document the rest of the specification stack must remain aligned with.

When in doubt about a feature, scope decision, or trade-off, this document is the reference.

## 2. Vision Statement

`avrOSdb` delivers a seamless, modern debugging experience for cooperative, single-stack embedded architectures by bridging the AVR UPDI interface directly to standard IDEs with native avrOS state-machine awareness without requiring JTAG or ICE controllers like Atmel-ICE. Instead it uses the Serial + 1k Resistor mod to enable debugging with just a USB to 5v serial adapter or directly connect to Raspberry Pi UART pins.

Embedded engineers developing for modern AVR microcontrollers (DA/DB families) on Linux and macOS can debug their finite state machines, queues, and memory pools visually in VS Code or Zed, no longer forced to manually parse raw SRAM dumps or fight hardware breakpoint limitations over a command-line interface.

## 3. Problem Statement

Debugging an avrOS application with it's finite state machine (FSM) manager on memory-constrained microcontrollers currently requires significant overhead, a different development environment from the native avrOS host environment, and manual translation of system state. Standard out-of-the-box GDB RTOS awareness assumes preemptive multitasking and multiple stack pointers, which flatly fails against the lean architecture of avrOS. 

* Developers are forced to manually dump and decode SRAM to determine which FSM is currently active and which are suspended.
* Understanding the state of system queues, events, and memory pools requires navigating raw memory addresses and cross-referencing them against FLASH-resident tables manually.
* The rigid hardware breakpoint limitations of the AVR UPDI interface restrict the ability to trace execution fluidly.
* Modern IDEs (VS Code, Zed) treat the target as a "dumb" remote core, providing no visual insight into the actual application architecture.
* Current solutions require external JTAG tools like Atmel-ICE or something comparable.

The cumulative cost of this status quo is delayed bug resolution, over-reliance on `printf` debugging (which alters timing), and significant friction when inspecting the true state of complex, event-driven embedded systems.

## 4. Target Users

| Persona | Needs from `avrOSdb` |
| ------- | ------------------------- |
| **Embedded Application Developer** | Needs to see active and suspended FSMs, pending events, and queue payloads directly in their IDE's UI. |
| **Toolchain Integrator** | Needs a reliable, standard `target extended-remote` GDB interface to script launches in VS Code (Cortex-Debug) and Zed (DAP). |

`avrOSdb` is **not** aimed at: Developers using FreeRTOS or preemptive multitasking OSs, or developers targeting legacy ISP/JTAG AVR architectures (e.g., ATMega328p).

## 5. Value Proposition

`avrOSdb` eliminates the friction of embedded FSM debugging by doing 4 things, in order:

1.  **UPDI Protocol Bridging** — Exposes modern AVR DA/DB memory and execution control safely to `avr-gdb` over a standard local port.
2.  **Harvard Architecture Translation** — Automatically parses ELF `.text` sections to locate FLASH-resident system tables, mapping them to their dynamic SRAM status bytes without manual address configuration.
3.  **FSM Virtual Thread Mapping** — Translates the avrOS state-machine manager into standard GDB threads, exposing the currently executing FSM and all suspended FSM function pointers natively to the IDE's Call Stack UI.
4.  **System Introspection** — Provides custom `monitor` commands to poll and decode the state of queues, event bitmasks, and memory pools non-intrusively using UPDI background reads.
5.  **USB serial adaptor connectivity** — Connect cheaply and easily wired directly to a Raspberry Pi and a 1K resistor. Optionally, you can connect directly to a PC using a TTL serial adaptor and the same 1K resistor.

The unifying design choice is **State Over Stacks**: presenting the system exactly as it is architected (cooperative FSMs) rather than attempting to shoehorn it into traditional preemptive RTOS paradigms.

## 6. Product Principles

These principles are the tie-breakers when requirements conflict.

1.  **FSM-First Visualization.** We never attempt to unwind dormant stack pointers. Execution context is strictly defined by the current function pointer of the avrOS state tables.
2.  **Lean Host Architecture.** The server runs natively on Linux and macOS. We never introduce heavy host dependencies (like Java, Electron, or Python virtualization) to run the GDB stub.
3.  **Editor-Agnostic Core.** The server communicates exclusively via standard GDB remote serial protocol (RSP). We never implement IDE-specific plugins directly inside the server; all VS Code and Zed support is handled via their respective standard adapters (e.g., Cortex-Debug, DAP).
4.  **Non-Intrusive Polling.** System introspection (queues, events) prioritizes UPDI asynchronous memory reads. We never arbitrarily halt the CPU core to evaluate a system state unless explicitly requested by a user breakpoint.
5.  **Fail-Safe Address Resolution.** If the server cannot dynamically locate the avrOS system tables in the ELF/FLASH, it gracefully degrades to a standard bare-metal GDB server rather than crashing.
6.  **Layered Architecture.** Source modules are organised into four layers — entry/event-loop, transport/hardware (UPDI), protocol (GDB RSP), and application (ELF/FSM/monitor) — with strictly downward call direction. New features land in the layer that owns the corresponding concern; we never reach across layers or invert dependencies, and lower-layer headers never include higher-layer headers. The single permitted upward path is the `RspHandlers` callback table that lets the protocol layer invoke application-layer code without depending on it at compile time.
7.  **Verifiable.** Every behaviour worth describing is captured as an HLR/LLR with at least one bound test. The Traceability Matrix (e.g., via TraceR workflows) is the contract.

## 7. Scope

### 7.1 In Scope

* Native compilation for Linux and macOS.
* Written in C using C99 specfication
* Full UPDI physical layer communication and timing handling.
* Implementation of the GDB Remote Serial Protocol (RSP).
* Automatic ELF parsing for avrOS FLASH table location.
* Virtual thread generation from avrOS FSM tables.
* Custom `monitor avros events`, `monitor avros queues`, and `monitor avros mempool` commands.
* Asynchronous memory polling to bypass hardware breakpoint limits where possible.
* Only requires TTL level UART connection with 1k resistor to the host development workstation. No Atmel-ICE or other external JTAG programmer required.
* Command line support to load an elf file into flash. Intended to support automated build and test
* UPDI to console emulation to support avrOS CLI
* UPDI to stdout to support testing

### 7.2 Out of Scope

* Windows support (for the initial release).
* Support for legacy debug protocols (JTAG, debugWIRE, PDI).
* Built-in SVD (System View Description) parsing for hardware registers (this is delegated to the IDE/Cortex-Debug).
* Thread-aware unwinding for preemptive operating systems.

### 7.3 Non-Goals

* Creating a standalone GUI debugger application.
* Supporting non-AVR microcontrollers (e.g., ARM Cortex, RISC-V).

## 8. Success Metrics

`avrOSdb` is succeeding when:

| Metric | Target |
| ------ | ------ |
| **Time to Attach** | The server initializes, locates tables, and attaches to the IDE in under 2.0 seconds. |
| **State Accuracy** | 100% of defined avrOS FSMs correctly appear as virtual threads in the VS Code/Zed call stack pane upon hitting `main()`. |
| **Introspection Reliability** | `monitor avros events` accurately reflects pending bits without halting the core, measured across 10,000 continuous polling cycles without protocol desync. |
| **Coverage** | Every requirement in HLRs.md and LLRs.md is bound to at least one test in STP.md, per Traceability.md. Coverage gaps are documented, not silent. |

## 9. Roadmap Themes

These are *themes* — not committed features — that frame future investment. Specific work items live in HLRs/LLRs as they are adopted.

* **avarice Feature Parity.** Bringing the GDB Remote Serial Protocol surface up to functional parity with the legacy `avarice` JTAG/dW stub for the AVR-Dx UPDI use case: `(gdb) load` over `vFlash*`, true software breakpoints, hardware data watchpoints, an expanded `monitor` verb set, and extended-remote lifecycle (`vRun`/`vAttach`/`vKill`). Wire-level drop-in compatibility with `avarice`-specific scripts is an explicit non-goal — individual `.gdbinit` files, IDE launch configurations, and CI invocations may need targeted edits to reach the same outcome.
* **Native DAP Translation.** Developing a direct Debug Adapter Protocol (DAP) interface to bypass `avr-gdb` entirely, optimizing integration for ultra-fast editors like Zed.
* **Live Profiling.** Utilizing non-intrusive UPDI reads to sample FSM execution times and queue depths over time, outputting a data stream suitable for visual performance profiling. 

Anything not listed here is not on the roadmap and would require an explicit vision update.

## 10. Relationship to the Rest of the Spec Stack

| Document | Question it answers |
| -------- | ------------------- |
| **PVD** (this document) | *Why* does this product exist, and how do we know it is succeeding? |
| [HLRs.md](HLRs.md) | *What* must the product do to deliver on the vision? |
| [LLRs.md](LLRs.md) | *How* does each function in the implementation contribute to an HLR? |
| [SDD.md](SDD.md) | *How is the implementation structured* to satisfy the LLRs? |
| [STP.md](STP.md) | *How do we verify* that each LLR (and therefore each HLR, and therefore the vision) is actually delivered? |
| [Traceability.md](Traceability.md) | *Where are the gaps*, end-to-end? |

A change to this PVD should propagate downward (some HLRs may be added, retired, or reworded). A change discovered during implementation that conflicts with this PVD is a signal to update this document — not to silently diverge.
