# Phase 7 — Device-Signature Diagnostic Mode (`--device`)

Tracks the work defined in [doc/SDP.md §8 — Phase 7](../doc/SDP.md#phase-7--device-signature-diagnostic-mode).

**Depends on:** Phases 0–6 complete (binary `avr-updi-gdb` builds clean, full test suite green, install/bundle targets working).

## Summary

Add a `--device` command-line flag that opens the UPDI link, reads the target SIGROW (DEVICEID0..2, REVID, 10-byte SERNUM) and the UPDI ASI status registers, prints a verbose human-readable report to `stdout` (style similar to `avrdude -v`), and exits. No GDB listener is bound, no event loop is entered, and `<elf-file>` is optional in this mode. Every failure produces a `stderr` diagnostic that names the failed UPDI step so an operator can distinguish wiring, power, and fuse problems without external instrumentation.

## Motivation

During bring-up of new boards and adapters there is currently no first-line tool to verify the host↔target UPDI link without launching a full GDB session. `--device` provides a one-shot diagnostic with verbose failure reporting that is useful for debugging both the app and the hardware connection.

## Deliverables

### CLI / source
- [ ] `src/main.c` — `AppConfig.device_info` (bool, default false); `parse_args()` recognises `--device`, makes `<elf-file>` optional in that mode, and rejects `--device` combined with `--load` (usage error, exit 1).
- [ ] `src/main.c` — `static int run_device_mode(const AppConfig *cfg)` that opens UPDI, calls `updi_read_device_info()`, prints the formatted report, closes UPDI, returns exit code. Does **not** call `rsp_listen()`, `elf_*`, `fsm_*`, or `event_loop()`.
- [ ] `src/main.c` — static `device_family[]` lookup table covering AVR DA / DB / DD / EA signatures (minimum: AVR32/64/128 DA28/32/48/64 and DB28/32/48/64). Unknown signatures render as `unknown device`.
- [ ] `src/updi.h` — `UpdiDeviceInfo` struct + `int updi_read_device_info(int fd, UpdiDeviceInfo *info)`.
- [ ] `src/updi.c` — implementation: SIGROW read at `0x1100`–`0x1119`, three `LDCS` reads (`ASI_SYS_STATUS`, `ASI_KEY_STATUS`, `ASI_STATUSB`); non-destructive (no halt, no NVM activity); on failure sets `info->fail_op` to a short ASCII tag and returns -1.

### Documentation
- [ ] `doc/UserManual.md` — `--device` row in the options table, a dedicated section with an annotated example output, and a Troubleshooting mention that `--device` is the first-line diagnostic.
- [ ] `doc/avr-updi-gdb.1` — `--device` under SYNOPSIS and OPTIONS plus a third entry under EXAMPLES.

### Tests (`tests/test_device.c` — 6 tests)
- [ ] (a) `parse_args` accepts `--device` without `<elf-file>` (LLR-MAIN-08).
- [ ] (b) `parse_args` rejects `--device` combined with `--load` (LLR-MAIN-08).
- [ ] (c) `updi_read_device_info()` populates the struct from a PTY-mocked SIGROW byte stream (LLR-UPDI-13).
- [ ] (d) `updi_read_device_info()` records the failed step name and returns -1 on NAK (LLR-UPDI-13).
- [ ] (e) `run_device_mode()` prints `Serial device:`, `Baud rate:`, `Signature:`, `Family:`, `Revision:`, `Serial:`, `UPDI status:` to stdout (LLR-MAIN-09).
- [ ] (f) `run_device_mode()` does not call `rsp_listen()` (LLR-MAIN-09).
- [ ] Makefile — register `test_device` in `TEST_NAMES` / `TEST_SRCS_test_device`.

### TraceR spec (already merged in this branch)
- [x] `doc/Project.xml` — SDD additions (main.c + updi.c), HLR-044 in new §9, LLR-MAIN-08/09, LLR-UPDI-13, `tests/test_device.c` block.
- [x] `doc/SDP.md` — Phase 7 section + §10 effort row.

## Acceptance Criteria

- `make` builds clean under `-Wall -Wextra -Wpedantic`.
- `make test` runs `tests/test_device` reporting 6/6 passing; full suite green.
- `python3 tools/lint_project.py` → 0 errors, 0 warnings.
- Generated docs (SDD/HLRs/LLRs/STP/Traceability) re-render with no `UnicodeEncodeError` (run with `LC_ALL=C.UTF-8 PYTHONIOENCODING=utf-8`).
- `man -l doc/avr-updi-gdb.1` exits 0.
- On real hardware: `avr-updi-gdb --device /dev/ttyUSB0` prints a recognisable signature and family name and exits 0; with the adapter disconnected the same command exits 1 with a `stderr` diagnostic that explicitly names the failed UPDI step (`updi-open`, `break`, `synch`, `sigrow`, etc.).

## CLI synopsis after Phase 7

```
avr-updi-gdb [--port port] [--baud baud] [--load] <serial-device> <elf-file>
avr-updi-gdb --device [--baud baud] <serial-device> [elf-file]
```

## Example output (real `AVR128DA48`)

```
$ avr-updi-gdb --device /dev/ttyUSB0
Serial device:   /dev/ttyUSB0
Baud rate:       115200
UPDI link:       up (SYNCH ack in 412 µs)
Signature:       1E 97 0A
Family:          AVR128DA48
Revision:        A6
Serial:          00 1A 2B 3C 4D 5E 6F 70 81 92
UPDI status:     SYS_STATUS=0x82  KEY_STATUS=0x10  STATUSB=0x00
NVM state:       idle, no programming key active
```

## Verification commands

```sh
python3 tools/lint_project.py
export LC_ALL=C.UTF-8 PYTHONIOENCODING=utf-8
for d in SDD HLRs LLRs STP Traceability; do
  python3 tools/render_doc.py tools/templates/$d.md.j2 $d --out doc/$d.md
done
make clean && make && make test
man -l doc/avr-updi-gdb.1 >/dev/null
```

## Out of Scope

- Fuse reading / writing (`--device` is read-only and non-destructive).
- Changes to the OCD layer, RSP packet handling, or NVM programming code paths.
- Cross-platform packaging changes (Phase 6 deliverables remain unchanged).

## Effort

S per SDP §10 — one UPDI helper, one new CLI flag, a family-name lookup table, one new test file; touches only `main.c` and `updi.c`; no protocol changes.
