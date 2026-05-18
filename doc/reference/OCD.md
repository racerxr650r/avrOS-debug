Here is the synthesized `OCD.md` document based on the design patterns and reverse-engineered register mappings utilized by open-source UPDI debuggers (such as Bloom, PyAvrOCD, and AVRDUDE’s UPDI backend).

Because Microchip does not officially publish the target memory-mapped addresses for the On-Chip Debugger (OCD) peripheral, this document captures the established community consensus on how these control registers are accessed via standard UPDI instructions to achieve POSIX GDB server functionality.

---

# AVR UPDI On-Chip Debug (OCD) Implementation Guide

This document outlines the internal mechanics of the AVR Unified Program and Debug Interface (UPDI) OCD peripheral. It details the memory-mapped registers and operational sequences required to implement Start, Stop, Single-Step, and Breakpoint functionality in custom debugging tools.

## 1. Architectural Overview

Unlike legacy JTAG or debugWIRE, modern AVR microcontrollers (tinyAVR 0/1/2, megaAVR 0, AVR-Dx, AVR-Ex) map the OCD peripheral directly into the unified data memory space. Debugging over UPDI consists of two separate access domains:

1. **The Application Status Interface (ASI):** A set of out-of-band link-layer registers used to configure the debugger state, check lock status, and monitor sleep/halt states.
2. **The Memory-Mapped OCD Registers:** A peripheral block (typically located high in the I/O space) accessed via standard UPDI `LDS` (Load) and `STS` (Store) instructions.

---

## 2. OCD Initialization and Activation

Before the debugger can intercept execution, the OCD peripheral must be unlocked.

### The OCD Key Handshake

To authorize the debug session, the host must transmit a 64-bit cryptographic key using the UPDI `KEY` instruction before the target leaves the reset state.

* **Key String:** `"OCD     "` (ASCII hex: `0x4F 0x43 0x44 0x20 0x20 0x20 0x20 0x20`)

### Activating the Debugger

Once the key is accepted, the host must toggle the OCD activation bit in the ASI space.

* **Register:** `ASI_SYS_CTRLA` (UPDI internal control register)
* **Action:** Set the `OCD_EN` bit.
* **Verification:** Poll `ASI_SYS_STATUS` to ensure the debugger is active.

---

## 3. Stop (Halt) and Start (Resume) Execution

Halting the CPU is not done via a memory-mapped register, but rather via the out-of-band ASI control registers to ensure the CPU can be interrupted regardless of the current program counter.

### Halting the Core (Stop)

* **Register:** `ASI_CPU_REQ`
* **Action:** Write the `HALT_REQ` bit (typically `0x02` or `0x01` depending on the family variant).
* **Verification:** The host must poll `ASI_SYS_STATUS` until the `HALTED` bit is set.

### Resuming the Core (Start)

* **Register:** `ASI_CPU_REQ`
* **Action:** Clear the `HALT_REQ` bit.
* **Verification:** Poll `ASI_SYS_STATUS` until the `HALTED` bit clears, indicating the core has resumed standard instruction fetching.

---

## 4. Single-Stepping Execution

Single-stepping instructs the AVR core to execute exactly one instruction before immediately throwing a hardware break and returning to the halted state.

* **Register:** `OCD_CTRLA` (Memory-Mapped OCD space)
* **Action:**
1. Ensure the core is currently halted.
2. Write to `OCD_CTRLA` setting the `STEP` bit.
3. Clear the `HALT_REQ` bit in the `ASI_CPU_REQ` register to release the CPU.


* **Behavior:** The CPU will execute one full opcode (whether it is a 1-word or 2-word instruction) and immediately halt. `ASI_SYS_STATUS` will once again raise the `HALTED` flag.

---

## 5. Breakpoint Functionality

The AVR OCD supports both Software Breakpoints (modifying flash) and Hardware Breakpoints (dedicated silicon comparators).

### Software Breakpoints (The `BREAK` Instruction)

If the application is running out of SRAM, or if the host debugger is willing to tolerate flash page reprogramming penalties (as seen in some GDB server implementations), software breakpoints are unlimited.

* **Opcode:** `0x9598` (`BREAK`)
* **Implementation:** The debugger overwrites the target 16-bit instruction with `0x9598`. When the CPU fetches this opcode, it immediately halts and signals the OCD peripheral.

### Hardware Breakpoints

Hardware breakpoints watch the Program Counter (PC) or the Data Address bus without requiring flash modification. The number of available comparators varies by silicon (usually 2 to 4).

* **Registers:** Memory-mapped OCD breakpoint arrays (e.g., `PSB0`, `PSB1` for Program Space Breaks; `PDSB` for Data Space Breaks).
* **Implementation Flow:**
1. Halt the CPU.
2. Write the 16-bit or 24-bit target address into the target Program Space Break register (`PSB0`).
3. Modify the **Break Control Register** (within the OCD memory block) to set the `EN_PSB0` (Enable PSB0) flag.
4. Resume the CPU.


* **Interrupt Resolution:** When the PC matches `PSB0`, the CPU halts. The debugger must read the **Break Status Register** to determine *which* hardware breakpoint triggered the halt before notifying GDB.

---

## 6. Accessing Core CPU Registers (Instruction Injection)

Standard UPDI `LDS`/`STS` instructions can read SRAM and Peripherals, but **they cannot directly read the internal CPU Working Registers (r0-r31) or the Program Counter (PC)** while the core is halted. Open-source tools solve this via "Instruction Injection."

1. **The Target Instruction:** The debugger formulates a standard AVR assembly instruction, such as `ST Z, r16` (Store r16 to the memory address at the Z pointer).
2. **The Execution Register:** The debugger writes this raw opcode directly into a hidden, memory-mapped OCD execution register.
3. **Execution:** The debugger toggles a specific bit in the `OCD_CTRLA` register, forcing the halted CPU to execute *only* the injected instruction offline, without advancing the Program Counter.
4. **Extraction:** The CPU dumps `r16` into an SRAM scratchpad address. The debugger then uses a standard UPDI `LDS` command to read that SRAM address and forward the register value to GDB.

-------------------------------------------------------------------------------------------------------

Here is the reference mapping of the addresses, offsets, and bit positions for the registers involved in AVR UPDI debugging.

Because UPDI utilizes a dual-domain architecture, these registers are split into two distinct spaces: the **UPDI System Space (ASI)**, accessed via link-layer `LDCS`/`STCS` instructions, and the **Target Memory Space (OCD)**, accessed via standard `LDS`/`STS` instructions.

### 1. Application Status Interface (ASI) Registers

These registers exist within the UPDI peripheral itself and control the out-of-band link layer. They are accessed using UPDI Control/Status (`CS`) indices rather than memory addresses.

| Register Name | UPDI CS Index | Bit Name | Bit Position | Hex Mask | Purpose |
| --- | --- | --- | --- | --- | --- |
| **ASI_CPU_REQ** | `0x08` | `HALT_REQ` | Bit 1 | `0x02` | Setting this bit requests the CPU to halt. Clearing it requests the CPU to resume. |
| **ASI_SYS_CTRLA** | `0x09` | `OCD_EN` | Bit 3 | `0x08` | Setting this bit enables the On-Chip Debugger peripheral (requires the OCD key to be sent first). |
| **ASI_SYS_STATUS** | `0x0B` | `OCD_EN` | Bit 4 | `0x10` | Read-only flag. When set, confirms the OCD peripheral has successfully initialized. |
| **ASI_SYS_STATUS** | `0x0B` | `HALTED` | Bit 5 | `0x20` | Read-only flag. When set, confirms the CPU has successfully halted execution. |

---

### 2. On-Chip Debug (OCD) Memory-Mapped Registers

These registers are mapped directly into the target microcontroller's unified memory map. The values below reflect the established mappings found in open-source toolchains (such as Bloom and Microchip's own open-source `pymcuprog` backend) for modern UPDI devices (tinyAVR 0/1/2, megaAVR 0, and AVR-Dx families).

**OCD Peripheral Base Address:** `0x0F80`

| Register Name | Memory Address | Offset | Bit Name / Notes |
| --- | --- | --- | --- |
| **OCD_CTRLA** | `0x0F80` | `0x00` | **`STEP` (Bit 0 / `0x01`)**: Setting this bit while the CPU is halted arms the single-step hardware. |
| **OCD_STATUS** | `0x0F81` | `0x01` | Debugger status flags (e.g., indicating if a hardware break or software break triggered the current halt). |
| **OCD_DBGCTRL** | `0x0F82` | `0x02` | Debug control configurations (e.g., keeping timers running or paused during a halt). |
| **OCD_INSTR** | `0x0F88` | `0x08` | **Instruction Execution Register (16-bit)**: The hidden execution register where raw AVR opcodes (like `LDS` or `STS`) are injected to read/write core CPU registers `r0` through `r31`. |
| **OCD_PSB0_L** | `0x0F90` | `0x10` | **Program Space Breakpoint 0 (Low Byte)**. |
| **OCD_PSB0_H** | `0x0F91` | `0x11` | **Program Space Breakpoint 0 (High Byte)**. (On 24-bit address families like AVR-Dx, an additional extended byte register exists). |

### Implementation Note for Instruction Injection

To read a CPU working register (e.g., `r16`) using the `OCD_INSTR` register at `0x0F88`:

1. The host constructs the 16-bit opcode for `STS SRAM_Address, r16`.
2. The host writes this 16-bit opcode to `0x0F88` using UPDI `STS`.
3. The CPU (while halted) natively executes the instruction, pushing the value of `r16` out into the unified memory space where the host can safely read it.


Because Microchip does not officially publish the On-Chip Debug (OCD) register map in their datasheets, the following information is an amalgamation of data reverse-engineered by the open-source community and extracted from the source code of tools like Bloom, PyMCUProg, and AVRDUDE.

The UPDI debug architecture is split into two distinct addressing domains: the out-of-band **Application Status Interface (ASI)** accessed via UPDI Control/Status (`CS`) indices, and the target-side **OCD Peripheral** accessed via standard memory space `LDS`/`STS` instructions.

---

### 1. Application Status Interface (ASI) Registers

These registers control the UPDI physical/link layer and the master state of the CPU. They are accessed using the UPDI `LDCS` (Load Control/Status) and `STCS` (Store Control/Status) instructions.

| CS Index | Register Name | Bit Pos | Bit Name | Mask | Description / Function |
| --- | --- | --- | --- | --- | --- |
| **`0x08`** | **`ASI_CPU_REQ`** | 1 | `HALT_REQ` | `0x02` | Request the CPU to halt execution. Clearing this resumes execution. |
|  |  | 4 | `RESET_REQ` | `0x10` | Force a system reset of the microcontroller core. |
|  |  | 5 | `NVMPROG_REQ` | `0x20` | Request control of the Non-Volatile Memory (Flash/EEPROM) controller. |
| **`0x09`** | **`ASI_CTRLA`** | 0 | `UPDICLK_SEL` | `0x01` | Select UPDI clock source (0 = internal oscillator, 1 = external clock). |
|  |  | 1 | `UPDIDIS` | `0x02` | Disable UPDI completely (Requires physical reset or HV pulse to recover). |
|  |  | 2 | `RESET_EN` | `0x04` | Allow UPDI to control the reset line. |
|  |  | 3 | `OCD_EN` | `0x08` | Enable the OCD peripheral. (Ignored if the `OCD` activation key has not been sent). |
| **`0x0A`** | **`ASI_SYS_CTRLA`** | 0 | `CLK_REQ` | `0x01` | Force the main system clock to stay running (useful during deep sleep debugging). |
| **`0x0B`** | **`ASI_SYS_STATUS`** | 0 | `RST_SYS` | `0x01` | [Read-Only] 1 = Target is currently held in reset. |
|  |  | 1 | `INSLEEP` | `0x02` | [Read-Only] 1 = CPU is currently in a sleep state. |
|  |  | 2 | `NVMPROG` | `0x04` | [Read-Only] 1 = NVM controller is active/busy. |
|  |  | 4 | `OCD_EN` | `0x10` | [Read-Only] 1 = OCD is fully initialized and active. |
|  |  | 5 | `HALTED` | `0x20` | [Read-Only] 1 = CPU is successfully halted and ready for instruction injection. |
|  |  | 6 | `LOCKSTATUS` | `0x40` | [Read-Only] 1 = Device is locked via security fuses. Debugging blocked. |

---

### 2. OCD Memory-Mapped Registers (Base Address: `0x0F80`)

These registers live directly in the microcontroller's unified memory map. Once the CPU is halted via the ASI registers, the host uses standard UPDI `LDS` and `STS` instructions targeting these memory addresses to configure breakpoints and execute code.

| Address | Register Name | Bit Pos | Bit Name | Mask | Description / Function |
| --- | --- | --- | --- | --- | --- |
| **`0x0F80`** | **`OCD_CTRLA`** | 0 | `STEP` | `0x01` | Arm the hardware single-step mechanism. When the CPU resumes, it will execute exactly one instruction and halt. |
|  |  | 1 | `RUN_STBY` | `0x02` | Keep OCD active during Standby sleep mode. |
|  |  | 2 | `RUN_TIME` | `0x04` | Keep internal timers running while the CPU is halted (prevents watchdog resets during debug pauses). |
| **`0x0F81`** | **`OCD_STATUS`** | 0 | `HWBREAK` | `0x01` | [Read-Only] Set if the CPU halted due to matching a hardware breakpoint. |
|  |  | 1 | `SWBREAK` | `0x02` | [Read-Only] Set if the CPU halted due to executing the `BREAK` instruction. |
|  |  | 2 | `EXTBREAK` | `0x04` | [Read-Only] Set if the CPU halted due to an external UPDI halt request. |
| **`0x0F88`** | **`OCD_INSTR_L`** | - | - | - | Low byte of the instruction injection register. |
| **`0x0F89`** | **`OCD_INSTR_H`** | - | - | - | High byte of the instruction injection register. Writing here triggers the CPU to execute the 16-bit word. |
| **`0x0F8A`** | **`OCD_DATARX`** | - | - | - | Unused in basic implementations; can be used for fast host-to-target data transfer. |
| **`0x0F8B`** | **`OCD_DATATX`** | - | - | - | Unused in basic implementations; can be used for fast target-to-host data transfer. |
| **`0x0F90`** | **`OCD_PSB0_L`** | - | - | - | Program Space Breakpoint 0 (Low Byte). |
| **`0x0F91`** | **`OCD_PSB0_H`** | - | - | - | Program Space Breakpoint 0 (High Byte). *(Some families utilize `0x0F92` for the 24-bit extension).* |
| **`0x0F94`** | **`OCD_PSB1_L`** | - | - | - | Program Space Breakpoint 1 (Low Byte). |
| **`0x0F95`** | **`OCD_PSB1_H`** | - | - | - | Program Space Breakpoint 1 (High Byte). |
| **`0x0F98`** | **`OCD_DSB_L`** | - | - | - | Data Space Breakpoint (Low Byte) - Used for Memory Watchpoints. |
| **`0x0F99`** | **`OCD_DSB_H`** | - | - | - | Data Space Breakpoint (High Byte). |

---

### 3. Key Operational Sequences

#### Activating the OCD Peripheral

To gain access to the `0x0F80` memory space, the UPDI link must transmit the OCD activation key immediately after a reset, before the CPU begins executing user code.

1. Issue UPDI `KEY` Instruction: `0xE4`
2. Send the 64-bit string: `0x4F 0x43 0x44 0x20 0x20 0x20 0x20 0x20` ("OCD     ")
3. Write `0x08` to `ASI_CTRLA` (`0x09`) to set the `OCD_EN` bit.
4. Poll `ASI_SYS_STATUS` (`0x0B`) until `OCD_EN` (Bit 4) reads as 1.

#### Extracting the Program Counter (PC)

There is no dedicated memory-mapped register that holds the current Program Counter. Instead, debuggers must force the halted CPU to push the PC to the SRAM stack via Instruction Injection:

1. Halt the CPU (Set `HALT_REQ` in `ASI_CPU_REQ`).
2. Inject a `RCALL 0x0000` instruction via the `OCD_INSTR` registers (`0x0F88` and `0x0F89`). The CPU executes this, pushing its current PC onto the stack and decrementing the Stack Pointer (SP).
3. Inject instructions to read the SPL (Stack Pointer Low) and SPH (Stack Pointer High) I/O registers.
4. Use standard UPDI `LDS` to read the SRAM addresses currently pointed to by the SP to retrieve the PC.
5. Inject an `POP` instruction or an `ADIW` to repair the Stack Pointer back to its original state.

---------------------------------------------------------------------------------

To build a functional GDB server for modern AVR microcontrollers, your software must act as a translator. It receives high-level, ASCII-based **GDB Remote Serial Protocol (RSP)** packets over a TCP socket and translates them into low-level, half-duplex **UPDI** byte sequences targeting specific ASI and OCD registers.

Below is the architectural mapping between standard GDB RSP commands and the underlying AVR UPDI/OCD memory-mapped registers.

---

### 1. Execution Control & State Polling

These commands control the starting, stopping, and status reporting of the target CPU. They primarily interact with the out-of-band Application Status Interface (ASI).

| GDB RSP Packet | RSP Description | UPDI Target Register(s) | Translation Action / Bit Manipulation |
| --- | --- | --- | --- |
| **`?`** | Target Halt Reason / Status | `ASI_SYS_STATUS` (`0x0B`)<br>

<br>`OCD_STATUS` (`0x0F81`) | Poll `ASI_SYS_STATUS`. If `HALTED` (Bit 5) is 1, read `OCD_STATUS`. If `HWBREAK` (Bit 0) or `SWBREAK` (Bit 1) is set, reply with `$S05#` (SIGTRAP). |
| **`\x03`** (Ctrl-C) <br>

<br> or **`vCtrlC`** | Interrupt / Halt CPU | `ASI_CPU_REQ` (`0x08`) | Write `0x02` to set the `HALT_REQ` bit. Wait for `ASI_SYS_STATUS` to confirm `HALTED`. Reply with `$S02#` (SIGINT). |
| **`c`** <br>

<br> or **`vCont;c`** | Continue Execution | `ASI_CPU_REQ` (`0x08`) | Write `0x00` to clear the `HALT_REQ` bit. The CPU will immediately resume execution. |
| **`s`** <br>

<br> or **`vCont;s`** | Single Step | `OCD_CTRLA` (`0x0F80`)<br>

<br>`ASI_CPU_REQ` (`0x08`) | 1. Write `0x01` (`STEP`) to `OCD_CTRLA`.<br>

<br>2. Write `0x00` to `ASI_CPU_REQ` to clear `HALT_REQ`.<br>

<br>3. The CPU will execute one opcode and halt again. |

---

### 2. Register Access (Instruction Injection)

GDB assumes it can read the CPU's internal registers (like `r0-r31`, `SP`, `PC`, and `SREG`) at any time. Because these are **not** directly memory-mapped while the CPU is running or halted, the GDB server must use the Instruction Injection mechanism.

| GDB RSP Packet | RSP Description | UPDI Target Register(s) | Translation Action / Bit Manipulation |
| --- | --- | --- | --- |
| **`g`** | Read all CPU registers | `OCD_INSTR_L/H` (`0x0F88`)<br>

<br>Standard SRAM | 1. Inject the `STS` (Store Direct) opcode into `OCD_INSTR` to force the CPU to push `r0` through `r31` to a safe SRAM scratchpad.<br>

<br>2. Use standard UPDI `LDS` (Load) commands to read those SRAM addresses back to the host.<br>

<br>3. Format the block as a hex string and reply to GDB. |
| **`p`** *`[n]`* | Read single register *n* | `OCD_INSTR_L/H` (`0x0F88`)<br>

<br>Standard SRAM | Same as `g`, but inject a single `STS` instruction specifically targeting register *n*, then fetch that single byte from SRAM. |
| **`G`** | Write all CPU registers | `OCD_INSTR_L/H` (`0x0F88`)<br>

<br>Standard SRAM | 1. Use UPDI `STS` to write the new register values from GDB into the SRAM scratchpad.<br>

<br>2. Inject `LDS` (Load Direct) opcodes into `OCD_INSTR` to force the CPU to pull the values from SRAM back into `r0-r31`. |
| **`P`** *`[n=v]`* | Write single register *n* | `OCD_INSTR_L/H` (`0x0F88`)<br>

<br>Standard SRAM | Same as `G`, but write a single value to SRAM and inject one `LDS` instruction for register *n*. |

---

### 3. Program Counter (PC) Extraction & Modification

Extracting the PC is the most complex mapping because it is a multi-step macro that the GDB server must execute whenever GDB asks for the target state. GDB typically expects the PC to be included in the general register dump (`g`), appended at the end.

| Internal GDB Server Goal | UPDI Target Register(s) | Translation Action / Sequence |
| --- | --- | --- |
| **Read PC** | `OCD_INSTR` (`0x0F88`)<br>

<br>SRAM (via UPDI) | 1. Inject `RCALL +0` into `OCD_INSTR` (Pushes PC to stack, decrements SP).<br>

<br>2. Inject instructions to read the Stack Pointer (SP) into SRAM.<br>

<br>3. Use UPDI to read the SP value, then read the SRAM address the SP points to (This is the PC).<br>

<br>4. Inject `POP` instructions to restore the SP to its original state. |
| **Write PC** | `OCD_INSTR` (`0x0F88`) | 1. Inject instructions to load the target PC address into the `Z` pointer registers (`r30/r31` or `r24/r25` depending on architecture).<br>

<br>2. Inject an `IJMP` (Indirect Jump) instruction to force the halted CPU's PC to snap to the new address. |

---

### 4. Memory & Breakpoint Management

These commands handle reading/writing Flash, SRAM, and setting breakpoints.

| GDB RSP Packet | RSP Description | UPDI Target Register(s) | Translation Action / Bit Manipulation |
| --- | --- | --- | --- |
| **`m`** *`[addr,length]`* | Read Memory | Target Memory Address | Translates directly to UPDI `LDS` (Load from Data Space) commands. If reading a large block, the server should optimize by utilizing the UPDI `REPEAT` instruction to stream the data. |
| **`M`** *`[addr,length]`* | Write Memory | Target Memory Address | Translates to UPDI `STS` (Store to Data Space). If the address falls in the Flash memory map, the GDB server must silently execute the NVM (Non-Volatile Memory) unlock and page-erase/page-write sequence before writing. |
| **`Z0`** *`[addr]`* | Insert Software Breakpoint | Flash Memory Address (via NVM Controller) | 1. Read the original 16-bit opcode at the target Flash address.<br>

<br>2. Save the original opcode in the GDB server's host memory.<br>

<br>3. Overwrite the address with the AVR `BREAK` opcode (`0x9598`). |
| **`z0`** *`[addr]`* | Remove Software Breakpoint | Flash Memory Address (via NVM Controller) | Overwrite the `0x9598` `BREAK` opcode with the original instruction saved by the GDB server in the previous step. |
| **`Z1`** *`[addr]`* | Insert Hardware Breakpoint | `OCD_PSB0_L/H` (`0x0F90`)<br>

<br>`OCD_PSB1_L/H` (`0x0F94`) | Write the target address into an available Program Space Breakpoint register pair. Enable the specific PSB in the hidden debug control bits (implementation varies slightly by chip family). |