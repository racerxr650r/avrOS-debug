# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

**avrOS-debug** (`avrOSdb`) is a GDB Remote Serial Protocol (RSP) server that
bridges `avr-gdb` to modern **AVR-Dx** microcontrollers over the **UPDI**
single-wire debug interface — no JTAG/ICE hardware, just a USB-to-TTL serial
adapter and a 1kΩ resistor. It reads the target's ELF to surface **avrOS**
cooperative state machines as GDB virtual threads, and also works as a plain
bare-metal AVR-Dx debugger.

- Host OS: Linux. Target: AVR-Dx (AVR-DA/DB) running bare metal or avrOS.
- Single C99 host binary built from `src/`.

## Build & test

```bash
make            # build build/avrOSdb
make test       # build AVR ELF fixtures + all Unity suites, run them
make coverage   # clean build with gcov, run suites, report to build/coverage/
make clean      # remove build/, dist/, AI-tmp/
make check-tools# verify required host tools on PATH
make help       # full target list
```

- `ASAN=1` adds address/UB sanitizers; `V=1` for verbose; `VERSION=...` to override.
- CI (`.github/workflows/pr-tests.yml`) runs `make test-ci` and `make coverage`
  on PRs and posts sticky summary comments. **`make test` must stay
  hardware-independent** — never wire hardware tests into it.

### Hardware tests (manual only, need a real target)

`make hw-test` (safe, Groups A+B) and the destructive/opt-in variants
`hw-test-nvm`, `hw-test-rsp`, `hw-test-gdb`, `hw-test-all`. Destructive targets
require `HW_TEST_NVM_CONFIRM=YES`. Port defaults to `/dev/ttyAMA2` (override
with `HW_PORT=` or `PORT=`). See the `hw-test` block in the `Makefile` for all
`HW_*` knobs.

## Architecture (src/)

| File | Role |
| ---- | ---- |
| `main.c` | CLI parsing, TCP socket, `event_loop`, flash load, session teardown |
| `gdb_rsp.c` | RSP packet parse/format; `vCont`/continue/step; breakpoints (`Z0`/`Z1`); `RspContext` state |
| `updi.c` | UPDI physical/link layer, NVM programming, OCD (on-chip debugger) access |
| `fsm_mapper.c` | Maps avrOS FSM tasks → GDB virtual threads |
| `monitor.c` | `monitor` command dispatch |
| `elf_parser.c` | ELF symbol/segment parsing, VMA→LMA flash address translation, device-family checks |

Unit tests live in `tests/` (Unity framework, `--wrap` linker stubs per suite —
see the per-test config in the `Makefile`). AVR ELF fixtures are in
`tests/fixtures/`; hardware/acceptance harness in `tests/hw/`
(`gdb_acceptance.py`).

## Specification docs — managed by TraceR

**`doc/Project.xml` is the single source of truth.** These files are
**generated** and must never be hand-edited (changes are overwritten on render):

- `doc/SDD.md`, `doc/HLRs.md`, `doc/LLRs.md`, `doc/STP.md`, `doc/Traceability.md`

Hand-authored exceptions (edit directly, never generated):
- `doc/Project.xml` — the only spec file you edit directly for SDD/HLR/LLR/test content.
- `doc/PVD.md` — Product Vision, author-driven.
- `doc/SDP.md` — Software Development Plan. See **Development workflow** below.

To change any requirement, design section, test definition, or traceability
link, edit `Project.xml` then re-render (VS Code → *Project Spec: Render All
Documents*, or `tools/render_doc.py`). IDs (`HLR-NNN`, `LLR-XXX-NN`) are stable
contracts — never renumber or reuse; allocate the next free number. Wrap
special characters (`<`, `&`, backticks, markdown links) in `<![CDATA[...]]>`.

## Development workflow — phased delivery

Work is organized into **phases**, planned and documented in `doc/SDP.md`
(§8 Phased Delivery). The lifecycle of a phase:

1. **Plan & document** the phase in `doc/SDP.md` — append a `### Phase N — <title>`
   section matching the existing format: a `> **Status:**` blockquote, numbered
   implementation steps, an **Acceptance** criteria line, and any per-test
   `--wrap` / fixture notes. Cross-reference the HLR/LLR IDs it implements.
   (Requirement/design/test detail itself lives in `Project.xml` via the
   **tracer** skill; SDP.md is the plan and acceptance gate.)
2. **GitHub issue** — the phase becomes an issue (see `.github/ISSUE_PHASE_*.md`).
3. **Feature branch** — one short-lived branch per phase, named
   `<issue#>-<slug>` (e.g. `36-avrosdb-sram-excluded-from-memory-map...`,
   `1-phase-0-project-scaffolding`). Trunk-based; branch off and merge to the
   default branch (`develop`).
4. **Pull request** — merged once the phase's acceptance criteria are met. The
   review gate (SDP §5.2): `make` clean under `-Wall -Wextra -Wpedantic`,
   `make test` all pass, `python3 tools/lint_project.py` reports 0 errors/0
   warnings, no `<placeholder>` text remains. Then update the phase's
   `> **Status:**` blockquote in SDP.md to ✅ Complete with the merge commit.

Use the `gh` CLI for issues/PRs. Commit, branch, or push only when asked.

## Skills

Source of truth is `.github/skills/`; `.claude/skills/` symlinks to it so the
skills are directly invokable in Claude Code. Invoke the matching skill before
doing the work:

- **tracer** — any spec/requirements/design/test/traceability work, or any edit
  to `Project.xml`/`PVD.md`.
- **application-debug** — debugging `avrOSdb` itself: RSP/UPDI/TCP issues,
  changes to `main.c`/`gdb_rsp.c`, lockups, target-side state. Use `--log-rsp`
  (or `AVROSDB_GDB_LOG_RSP=1`) to dump wire traffic.
- **ai-tmp-cleanup** — where to put scratch files.

## Conventions

- Put temporary/scratch files (debug `.gdb` scripts, scratchpads, one-off logs)
  in **`AI-tmp/`** at the repo root. It is gitignored and wiped by `make clean`.
  Don't scatter temp files in `src/`, `tests/`, or the repo root.
- C99, `-Wall -Wextra -Wpedantic`; match surrounding style.
- Reference material: `doc/reference/OCD.md` (AVR OCD protocol) and
  `doc/reference/guesswork.md` (reverse-engineered UPDI/HW behaviour).
```
