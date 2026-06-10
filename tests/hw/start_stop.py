#!/usr/bin/env python3
"""Hardware acceptance for the --start / --stop / --reset one-shot CPU run-state
modes (HLR-089, LLR-MAIN-27/28, LLR-UPDI-37).

Spawns the real ``avrOSdb`` binary against live AVR-Dx silicon and drives each
one-shot run-state mode in turn, asserting that it takes OCD control and exits
cleanly with its success banner, and that the UPDI link stays healthy across the
repeated attach/detach cycles.  This exercises the full on-silicon path —
``updi_open`` → ``updi_enter_debug`` → ``updi_run`` / ``updi_halt`` →
``updi_detach`` (the no-reset teardown that leaves ``--stop`` halted) — that the
host unit tests can only stub.

Manual-only (``make hw-test-start-stop``); never wired into ``make test``.
Needs only the serial device — no ELF, no TCP listener.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

GREEN, RED, DIM, RESET = "\033[32m", "\033[31m", "\033[2m", "\033[0m"


def run(binary, args, timeout=25):
    try:
        p = subprocess.run([binary, *args], capture_output=True, text=True,
                           timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "(timed out)"
    except OSError as e:
        return 127, f"(spawn failed: {e})"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", default=os.getenv("HW_PORT", "/dev/ttyAMA2"))
    ap.add_argument("--bin", default=os.getenv("AVROSDB_BIN", "build/avrOSdb"))
    args = ap.parse_args()

    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        tag = (GREEN + "PASS" + RESET) if ok else (RED + "FAIL" + RESET)
        line = f"start-stop: {name:<46} {tag}"
        if detail and not ok:
            line += "  " + DIM + detail.strip().replace("\n", " ")[:160] + RESET
        print(line)

    # SS0 — host-level contract: the three run-state modes are mutually
    # exclusive (parse_args rejects before touching hardware).
    rc, out = run(args.bin, ["--start", "--stop", args.serial])
    check("SS0  --start --stop rejected (mutually exclusive)",
          rc != 0 and "mutually exclusive" in out, out)

    # SS1 — --stop: take OCD control and halt the CPU on real silicon.
    rc, out = run(args.bin, ["--stop", args.serial])
    check("SS1  --stop halts the CPU, exits 0",
          rc == 0 and "stop: OK" in out, f"rc={rc} {out}")

    # SS2 — --start: take OCD control and run the CPU on real silicon.
    rc, out = run(args.bin, ["--start", args.serial])
    check("SS2  --start runs the CPU, exits 0",
          rc == 0 and "start: OK" in out, f"rc={rc} {out}")

    # SS3 — --reset sibling still works after the start/stop cycle.
    rc, out = run(args.bin, ["--reset", args.serial])
    check("SS3  --reset pulses reset, exits 0",
          rc == 0 and "reset: OK" in out, f"rc={rc} {out}")

    # SS4 — the UPDI link is still healthy after the attach/detach cycles:
    # a fresh --device probe re-opens the link and reads the SIGROW report.
    rc, out = run(args.bin, ["--device", args.serial])
    check("SS4  link healthy after cycle (--device exits 0)",
          rc == 0, f"rc={rc} {out}")

    # SS5 — a second --stop after the cycle still succeeds (repeatable).
    rc, out = run(args.bin, ["--stop", args.serial])
    check("SS5  --stop repeatable",
          rc == 0 and "stop: OK" in out, f"rc={rc} {out}")

    passed = sum(1 for r in results if r)
    print(f"start-stop: {passed} passed, {len(results) - passed} failed")
    return 0 if passed == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
