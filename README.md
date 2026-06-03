# avrOS-debug

A GDB server for debugging your code on the modern AVR-Dx family of processors. It brings native avrOS state-machine awareness to your IDE (VS Code, Zed) or simply debugs your bare metal application without avrOS. No specialized JTAG or ICE programmer is needed, only a standard USB-to-TTL serial adapter (if debugging from a PC) and a single 1kΩ resistor.

## Overview

`avrOSdb` connects avr-gdb directly to the target hardware via the UPDI interface. It can read your project's ELF file to understand the `avrOS` tasks on the device and surfaces each cooperative state machine as a "virtual thread." This means your IDE's Call Stack now shows exactly which state machine is active, and you can inspect queues and events giving you unprecedented transparency into a avrOS application under debug. If you aren't using avrOS, it can act as a bare metal debugger for your AVR-Dx application as well.

## Key Features

- **No Dedicated Debugger Hardware** — Works via a simple serial adapter with a 1kΩ series resistor
- **Multithreaded FSM View** — Cooperative state machines automatically appear as standard OS threads in the GDB/IDE interface
- **Limitless Software Breakpoints** — Set up to 64 breakpoints directly in standard IDEs without worrying about hitting the AVR's 1-slot hardware comparator limit for normal stepping
- **Frictionless Flashing** — Reflash your device via `(gdb) load` straight from the IDE without restarting the server
- **Automatic Part Detection** — Automatically discovers your AVR-Dx memory map and blocks mismatched ELF flashing to protect your device
- **Non-destructive Control** — Debugging control leaves standard NVM layout preserved across sessions
- **VS Code Extension Ready** — Fully tested against the popular `Cortex-Debug` extension to connect seamlessly
- **Clean Detach** — Stopping the debugger correctly releases the hardware and lets the microcontroller return to running free

## Platform Support

- Host OS: Linux
- Target Devices: AVR-Dx family microcontrollers (e.g. AVR-DA, AVR-DB) running bare metal or with avrOS

## Quick Start

```bash
git clone https://github.com/racerxr650r/avrOS-debug
cd avrOS-debug
make all                             # Build avrOSdb
make install                         # Install avrOSdb
cd ${avrOS project directory}        # Go to your avrOS project          
avrOSdb /dev/ttyUSB0 build/main.elf  # start GDB server on localhost:1234
```

In VS Code (using the `Cortex-Debug` extension), set up your `launch.json`:
```json
// .vscode/launch.json
{
  "version": "0.2.0",
  "configurations": [
    {
      // Cortex-debug used as a *generic* GDB frontend
      "name": "Debug avrOS (external GDB)",
      "type": "cortex-debug",
      "request": "attach",
      "servertype": "external",
      "gdbTarget": "localhost:1234",
      "gdbPath": "/usr/bin/avr-gdb",
      "executable": "${workspaceFolder}/app/avrOS_example/build/main.elf",
      "cwd": "${workspaceFolder}/app/avrOS_example",
      "overrideAttachCommands": [
        "target extended-remote localhost:1234",
        "monitor reset",
        "tbreak main",
        "continue"
      ],
      "overrideResetCommands": [
        "monitor reset",
        "tbreak main",
        "continue"
      ],
      "showDevDebugOutput": "none",
      "preAttachCommands": [
        "set breakpoint auto-hw off",
        "set pagination off",
        "set print pretty on",
        "set remotetimeout 30",
        "set mem inaccessible-by-default off"
      ]
    }
  ]
}
```

## Documentation

For step-by-step wiring diagrams, complete CLI parameters, IDE integrations, and deeper technical specs, refer to our extended documentation:

- [User Manual](doc/UserManual.md) — Installation, wiring, and full CLI reference.
- [Software Design Document](doc/SDD.md)
- [Requirements](doc/HLRs.md) & [Traceability](doc/Traceability.md)
- [Software Test Plan](doc/STP.md)
- [Product Vision](doc/PVD.md)
