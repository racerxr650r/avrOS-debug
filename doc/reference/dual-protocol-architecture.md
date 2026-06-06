# Dual-Protocol Architecture Review: RSP + DAP over one debug core

**Status:** Phase 15 working document (issue #50). This is a hand-authored
review/plan, not a generated spec artefact. The requirement/design decisions it
recommends are reconciled into `doc/Project.xml` (SDD + HLRs) via the **tracer**
skill as the phase proceeds.

## 1. Purpose

PVD §9 names **Native DAP Translation** as a roadmap theme: a Debug Adapter
Protocol server that lets DAP-native editors (Zed, VS Code) drive `avrOSdb`
*directly*, without an `avr-gdb` process bridging the gap. The end state is two
client-facing protocol front-ends — the existing GDB RSP server and a new DAP
server — sitting **in parallel** over a single, protocol-agnostic debug core.

This document reviews where today's architecture already supports that, where it
couples debug logic to RSP, and what seam Phase 15 should establish so the DAP
work in a later phase is additive rather than a rewrite.

## 2. The one structural difference DAP forces: source-level mapping

RSP and DAP differ in framing (RSP: `$payload#cc` ASCII over TCP; DAP:
Content-Length-framed JSON-RPC), but that is the easy part. The architecturally
significant difference is **who owns source-level knowledge**:

| Concern | RSP today | DAP requires |
| ------- | --------- | ------------ |
| `addr ↔ file:line` | `avr-gdb` reads DWARF line tables | **server** must map it |
| variable/type → bytes | `avr-gdb` reads DWARF | **server** must resolve it |
| stack unwinding | `avr-gdb` uses DWARF CFI + `m` reads | **server** must unwind |
| breakpoint by `file:line` | `avr-gdb` resolves to a PC, sends `Z0 <pc>` | **server** must resolve |

With RSP, `avrOSdb` is deliberately source-blind: it serves raw registers,
memory, and PC-addressed breakpoints, and GDB does all the DWARF work. A DAP
client sends `setBreakpoints {path, line}`, `stackTrace`, `scopes`,
`variables` — all of which assume the *server* understands the program's debug
info. **This is the reason Phase 15 integrates `libdw` into `elf_parser`**
(work item 2): the DWARF capability is the prerequisite that makes a native DAP
server possible at all. It is independent of, and lands before, the protocol
front-end itself.

## 3. Current layering (as built)

```
                 ┌────────────────────────── main.c ──────────────────────────┐
                 │ CLI parse · TCP listen/accept · event_loop · session teardown│
                 └───────────────┬──────────────────────────────┬──────────────┘
                                 │ RspHandlers (fn-ptr table)    │ owns RspContext
                 ┌───────────────▼───────────────┐               │
   GDB client ──▶│  gdb_rsp.c  (RSP front-end)    │               │
   (TCP/RSP)     │  parse/format $..#cc · vCont   │               │
                 │  dh_* handlers · HW-BP arbiter │               │
                 │  stop-cause classifier · g-pkt │               │
                 └───┬───────────┬───────────┬────┘               │
                     │           │           │                    │
            ┌────────▼──┐  ┌─────▼─────┐ ┌───▼──────┐      ┌───────▼────────┐
            │ updi.c    │  │ monitor.c │ │fsm_mapper│      │ elf_parser.c   │
            │ UPDI/OCD  │  │ verbs     │ │ introspect│     │ symbols/segs   │
            │ exec ctrl │  └───────────┘ └──────────┘      │ (+DWARF: P15)  │
            │ reg/mem   │                                  └────────────────┘
            │ NVM       │
            └───────────┘
```

**What is already protocol-agnostic and reusable as-is:**

- **`updi.c`** is the debug-core *mechanism* layer and is already clean. Its
  OCD verbs are policy-free and take explicit parameters (e.g.
  `updi_ocd_set_hw_bp(fd, idx, addr)` — the *caller* picks the slot;
  `updi_step_32bit(fd, bp_slot, …)`). Per the Layered Architecture rule, UPDI
  helpers never choose a slot or a protocol. A DAP front-end calls the exact
  same primitives:
  `updi_halt/run/step`, `updi_ocd_read_pc/write_pc`, `updi_ocd_read_gpr`,
  `updi_mem_read/write`, `updi_nvm_*`, `updi_ocd_read_halt_status`, …
- **`elf_parser.c`** symbol/segment/`device_name` API is protocol-neutral.
  DWARF (Phase 15) extends it the same way.
- **`fsm_mapper.c`** and **`monitor.c`** are already verb-oriented and consume
  UPDI + ELF, not RSP.

**What is debug-core *policy* but currently lives inside the RSP front-end
(`gdb_rsp.c`):** these are the pieces a DAP server would otherwise have to
duplicate.

1. **The HW-comparator arbiter** (Phase 13): comparator 0 = the one user slot,
   comparator 1 = reserved step slot; `E08` on a second user HW BP. Pure policy
   over `RspContext.hw_bp_addr[2]`.
2. **The software-breakpoint shadow + step-over logic** (`RspSwBp sw_bp[64]`,
   `bp_mode`, the `BREAK`-plant/restore/inject sequence, `pc_dirty`).
3. **The stop-cause classifier** — reads `OCD_STATUS0/1` and decides
   `SC_SWBREAK` / `SC_HWBREAK` / `SC_STEP` / `SC_NONE`. Today its result is
   stashed in `RspContext.last_stop_cause` purely to shape the RSP `T`-reply.
4. **The register model** — *which* registers, in what order, form the
   architectural state (r0–r31, SREG, SP, PC). RSP's `g`-packet hex layout is
   front-end-specific, but the underlying ordered fetch from the OCD window is
   core.
5. **The device memory map** (`DeviceMemoryLayout map`) and the
   advertised-region bounds check.

## 4. The seam to establish in Phase 15

Introduce a **`debug_core`** boundary that owns the session state and policy in
§3, leaving each protocol front-end responsible only for wire framing and
translation:

```
   GDB client ─(RSP)─▶ gdb_rsp.c  ─┐
                                   ├─▶ debug_core  ─▶ updi.c / elf_parser(+DWARF) / fsm / monitor
   DAP  client ─(DAP)─▶ dap_srv.c ─┘   (session state + policy)
                       (later phase)
```

`debug_core` would expose a small, protocol-neutral verb surface — names
illustrative, to be fixed in the spec:

- **Execution:** `dc_halt`, `dc_resume`, `dc_step`, `dc_poll_stop` →
  returns a structured **stop event** (`{cause, pc, signal}`) instead of a
  pre-formatted RSP `T`-string. RSP and DAP each render that event into their
  own wire reply.
- **State:** `dc_read_regs`/`dc_write_regs` over an ordered register struct;
  `dc_read_mem`/`dc_write_mem` with the advertised-region bounds check;
  `dc_read_pc`/`dc_write_pc`.
- **Breakpoints:** `dc_bp_insert`/`dc_bp_remove` taking a PC address + kind,
  with the arbiter and SW-BP shadow behind them (the `E08`/`pc_dirty`/inject
  logic moves here, out of `dh_insert_bp`).
- **Introspection:** the existing `fsm_*` and `monitor` verbs.
- **Source (new, DWARF-backed):** `dc_addr_to_line`, `dc_line_to_addr`,
  `dc_locals_at(pc)`, `dc_unwind(pc, sp)` — thin wrappers over the new
  `elf_parser` DWARF accessors. RSP ignores these (GDB does the work); DAP
  depends on them.

**Splitting `RspContext`.** Today `RspContext` mixes core state
(`hw_bp_addr`, `sw_bp`, `bp_mode`, `pc_dirty`, `map`, `last_stop_cause`) with
RSP-transport state (`gdb_fd_p`, `g_thread_p`, `c_thread_p`,
`disconnect_reason`, `flash_xact_*`). The core fields become a
`DebugCoreSession`; the RSP front-end keeps a thin `RspContext` that *holds a
pointer to* the shared session plus its own transport fields. DAP gets its own
equivalently-thin context over the same session.

**The upward channel is already a precedent.** `RspContext.disconnect_reason`
is documented as "the sole upward channel from the protocol layer to the
entry/event-loop layer (preserves SDD §2.2 layered architecture)." The
structured stop event generalises exactly this idea: the core reports *what
happened*; each front-end decides *how to say it*.

## 5. Spec reconciliation (tracer) — done in Phase 15

The spec originally forbade what this phase prepares for:

- **HLR-020 "Editor-Agnostic Protocol"** — "The server shall communicate
  exclusively over standard GDB RSP. No IDE-specific protocol extensions (DAP,
  …) shall be implemented inside the server …"
- **SDD "Editor-Agnostic Core" design goal** — "The server exposes only
  standard GDB RSP. IDE-specific integration (Cortex-Debug, Zed DAP) is handled
  entirely by the GDB client."

These reflected the original RSP-only decision. PVD §9 has since adopted
*Native DAP Translation* as a theme, so the spec was reconciled (a deliberate,
recorded change):

- **HLR-020** revised — the *RSP front-end* stays pure-RSP, but a *separate*
  parallel protocol front-end (a future DAP server over the shared core) is now
  explicitly permitted; it does not alter the RSP front-end's contract.
- The SDD design goal was reworded from "Editor-Agnostic Core" to
  **"Protocol-Agnostic Debug Core"** (thin RSP + future DAP front-ends).
- **HLR-073 "Protocol-Agnostic Debug Core"** was added, with **LLR-RSP-51**
  binding `gdb_rsp.c` to the single aggregating seam include `src/debug_core.h`.
- New HLRs/LLRs were also authored for the DWARF capability (**HLR-072**,
  LLR-ELF-11..13) and the elfutils adoption (**HLR-071**).

The DAP front-end itself remains a *future* phase; Phase 15 removed the
prohibition and landed the core seam (`src/debug_core.h`) + DWARF only.

## 6. Scope boundary for Phase 15

**In scope (this phase):**
- This review (done).
- `elf_parser` reimplemented as a thin `libelf`/`libdw` adapter + DWARF
  accessors, with the public `elf_*` / `ElfContext` API unchanged for
  consumers (elfutils required on all platforms; `src/elf.h` shim deleted).
- Naming the `debug_core` verb surface and making the **low-risk** structural
  moves (collect the core entry points behind one header; do not yet relocate
  battle-tested RSP logic in a way that could regress the verified path).
- The tracer reconciliation in §5.

**Deferred to the DAP-server phase:**
- The DAP TCP listener, JSON-RPC framing, and request handlers.
- Physically relocating the arbiter / SW-BP / stop-classifier bodies out of
  `gdb_rsp.c` into `debug_core.c` (a behaviour-sensitive refactor best done
  with the DAP consumer in hand and the full `hw-test` suite as the net).
- DWARF-backed unwinding/variable rendering wired to actual DAP requests.

## 7. Risks

- **macOS / elfutils (HLR-033).** `libdw`/`libelf` are GNU/Linux-centric.
  Decision: elfutils is a **required** dependency (no symbol-only fallback);
  macOS obtains it via `brew install elfutils`. `src/elf_parser.c` is a thin
  adapter over libelf/libdw and the bundled `src/elf.h` shim is deleted.
- **Regressing the verified RSP path.** Mitigation: keep the Phase 15 moves
  structural and additive; defer behaviour-sensitive relocation to the phase
  that has the DAP consumer and re-runs `hw-test-gdb` G1–G24 as the gate.
- **DWARF for AVR specifics.** AVR's Harvard split (code vs data address
  spaces) and `avr-gcc`'s DWARF quirks (`.debug_line` byte-vs-word addressing,
  `DW_AT_location` for the mapped-flash window) need care; validate accessors
  against a known `-g` fixture, not just synthetic DWARF.
```
