#!/usr/bin/env python3
"""Phase-10 GDB acceptance harness (Group G).

Drives a real ``avr-gdb`` session against a freshly-spawned ``avrOSdb``
server which in turn talks to a live AVR-Dx target over UPDI. Verifies
the Phase-10 *avarice feature-parity* acceptance criteria from the SDP:

    G1  attach + ``monitor version`` banner
    G2  ``(gdb) load`` reflashes the target and PC = 0 after
    G3  five simultaneous breakpoints accepted (SW BP shadow)
    G4  ``break main`` + ``continue`` → halts at main
    G6  ``monitor reset`` / ``halt`` / ``info`` verbs behave
    G7  ``detach`` + re-attach extended-remote lifecycle

(Data watchpoints — G5 in earlier drafts — are intentionally not
exercised here: AVR-Dx UPDI silicon does not expose data-watchpoint
hardware, so the server replies the empty packet to Z2/Z3/Z4 and GDB
falls back to software watchpoints. See HLR-056 and
doc/reference/guesswork.md.)

The harness shells out to ``avr-gdb -batch -nx -x <script>`` once with a
generated GDB command script that emits ``===Gn===`` marker lines around
each test's output, then post-parses the captured stdout into PASS/FAIL
verdicts.  The output format mirrors the C hw-test harness so the
existing operator can read both reports the same way.

Usage::

    python3 tests/hw/gdb_acceptance.py \\
        --port      /dev/ttyAMA2 \\
        --elf       build/fixtures/gdb_target.elf \\
        --rsp-port  1234

Exit code: 0 if every G-test passes (or is explicitly skipped); 1 on
any failure.  Designed to be invoked from the ``hw-test-gdb`` Makefile
target; never wired into ``make test``.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from typing import List, Optional, Tuple

# Marker emitted between sections in the GDB script so we can slice the
# transcript into per-test windows.
MARK = "===G{n}==="
MARK_DONE = "===DONE==="

# ──────────────────────────────────────────────────────────────────────
#  Output helpers (match the visual style of tests/hw/hw_test.c)
# ──────────────────────────────────────────────────────────────────────

GREEN = "\033[32m"
RED   = "\033[31m"
YELL  = "\033[33m"
RESET = "\033[0m"

PASS_TAG = f"{GREEN}PASS{RESET}"
FAIL_TAG = f"{RED}FAIL{RESET}"
SKIP_TAG = f"{YELL}SKIP{RESET}"

WIDTH = 47

@dataclass
class Result:
    tag: str            # G1..G7
    title: str
    status: str         # "PASS" | "FAIL" | "SKIP"
    detail: str = ""
    ms: float = 0.0

def emit(r: Result) -> None:
    if r.status == "PASS":
        stat = PASS_TAG
        tail = f"({r.ms:7.1f} ms)"
    elif r.status == "FAIL":
        stat = FAIL_TAG
        tail = f"— {r.detail}" if r.detail else ""
    else:
        stat = SKIP_TAG
        tail = f"— {r.detail}" if r.detail else ""
    title = r.title.ljust(WIDTH)
    print(f"hw-test: {r.tag:4s} {title} {stat}  {tail}", flush=True)

# ──────────────────────────────────────────────────────────────────────
#  Server lifecycle
# ──────────────────────────────────────────────────────────────────────

def wait_for_tcp(port: int, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False

def spawn_server(avros_bin: str, port: int, rsp_port: int, elf: str,
                 log_path: str) -> subprocess.Popen:
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        [avros_bin, "--port", str(rsp_port), port, elf],
        stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
        close_fds=True,
    )
    return proc

def kill_server(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2.0)

# ──────────────────────────────────────────────────────────────────────
#  GDB script generation
# ──────────────────────────────────────────────────────────────────────

# Five scattered FLASH addresses well above the fixture's .text (the
# fixture is ~40 bytes) but inside the 128-KiB AVR-Dx FLASH window.
# Each lies on its own page so the BP-install paging is independent.
BP_ADDRS_HEX = [0x1000, 0x2000, 0x3000, 0x4000, 0x5000]

# Boilerplate prepended to every per-test script.
GDB_PROLOGUE_FMT = """\
set pagination off
set confirm off
set print pretty off
set print address off
set verbose off
set height 0
set width 0
set remotetimeout 10
target extended-remote :{rsp_port}
"""

# Commands executed for each Gn test, wrapped between connect (above)
# and a final `detach`/`quit`. Designed so each test is independently
# survivable — a hang in one does NOT impact the others.
PER_TEST_CMDS: dict[int, str] = {
    1: "monitor version\n",
    2: "load\ninfo registers pc\n",
    3: ("\n".join(f"break *0x{a:04X}" for a in BP_ADDRS_HEX)
        + "\ninfo breakpoints\ndelete breakpoints\n"),
    4: "break main\ncontinue\ninfo registers pc\ndelete breakpoints\n",
    6: ("monitor reset\nflushregs\ninfo registers pc\n"
        "monitor halt\nmonitor version\n"),
    7: ("detach\n"
        "target extended-remote :{rsp_port}\n"
        "info registers pc\n"),
}

# Per-test wall-clock cap (s) when running `avr-gdb -batch`. G2 has to
# erase+program a FLASH page over UPDI.
PER_TEST_TIMEOUT: dict[int, float] = {
    1: 10.0,
    2: 30.0,
    3: 20.0,
    4: 15.0,
    6: 15.0,
    7: 15.0,
}

def build_test_script(n: int, rsp_port: int) -> str:
    cmds = PER_TEST_CMDS[n].format(rsp_port=rsp_port)
    return (GDB_PROLOGUE_FMT.format(rsp_port=rsp_port)
            + cmds
            + "detach\nquit\n")

# ──────────────────────────────────────────────────────────────────────
#  Transcript parsing
# ──────────────────────────────────────────────────────────────────────

PC_RE = re.compile(r"pc\s+0x([0-9a-fA-F]+)")

def pc_value(text: str) -> Optional[int]:
    m = PC_RE.search(text)
    return int(m.group(1), 16) if m else None

# ──────────────────────────────────────────────────────────────────────
#  Per-test verdicts
# ──────────────────────────────────────────────────────────────────────

def verdict_G1(sect: str) -> Tuple[str, str]:
    # monitor version must produce output containing "avrOSdb" or a
    # version-ish token (semver / git short SHA).
    if re.search(r"avrOSdb|\bv?\d+\.\d+", sect, re.IGNORECASE):
        return "PASS", ""
    return "FAIL", "no version banner from `monitor version`"

def verdict_G2(sect: str) -> Tuple[str, str]:
    # Look for GDB's load summary and PC=0 read-back.
    if "Error" in sect or "failed" in sect.lower():
        return "FAIL", "GDB reported `load` error"
    if "Transfer rate" not in sect and "Loading section" not in sect:
        return "FAIL", "no `load` confirmation"
    pc = pc_value(sect)
    if pc is None:
        return "FAIL", "no PC reported after load"
    if pc != 0:
        return "FAIL", f"PC=0x{pc:X} after load (expected 0x0)"
    return "PASS", ""

def verdict_G3(sect: str) -> Tuple[str, str]:
    # info breakpoints must list at least 5 entries (one per address).
    # Each is rendered as "<num>  breakpoint  keep y  0x...".
    bps = re.findall(r"^\s*\d+\s+breakpoint", sect, re.MULTILINE)
    if len(bps) < 5:
        return "FAIL", f"only {len(bps)} BPs accepted (expected 5)"
    # Also confirm no install errors slipped in.
    if "Cannot insert breakpoint" in sect or "E22" in sect:
        return "FAIL", "BP installation error"
    return "PASS", ""

def verdict_G4(sect: str) -> Tuple[str, str]:
    # continue must produce a "Breakpoint N, ... main" hit line and the
    # subsequent `info reg pc` should be non-zero.
    if not re.search(r"Breakpoint \d+,.*\bmain\b", sect):
        return "FAIL", "no breakpoint hit at main"
    pc = pc_value(sect)
    if pc is None or pc == 0:
        return "FAIL", f"PC unexpected after hit: {pc}"
    return "PASS", ""

def verdict_G5(sect: str) -> Tuple[str, str]:
    # G5 removed: UPDI silicon has no data-watchpoint hardware (HLR-056).
    # Kept as a stub so any stale RUN_TEST() registration still imports.
    return "SKIP", "data watchpoints handled by GDB software fallback"

def verdict_G6(sect: str) -> Tuple[str, str]:
    # monitor reset → PC=0; monitor halt → "target halted"; monitor
    # version → recognisable banner. No protocol errors anywhere.
    if "Error in sourced command file" in sect:
        return "FAIL", "monitor verb failed"
    if "E22" in sect or "E11" in sect or "Invalid hex digit" in sect:
        return "FAIL", "monitor verb returned error code"
    pc = pc_value(sect)
    if pc is None:
        return "FAIL", "no PC after `monitor reset`"
    if pc != 0:
        return "FAIL", f"PC=0x{pc:X} after `monitor reset` (expected 0x0)"
    if "target halted" not in sect:
        return "FAIL", "`monitor halt` produced no `target halted` line"
    if not re.search(r"avrOSdb|\bv?\d+\.\d+", sect):
        return "FAIL", "`monitor version` produced no recognisable banner"
    return "PASS", ""

def verdict_G7(sect: str) -> Tuple[str, str]:
    # After detach + extended-remote :PORT, GDB must once again show a
    # plausible PC read-back.
    if "Connection refused" in sect or "remote failure" in sect.lower():
        return "FAIL", "could not re-attach"
    if pc_value(sect) is None:
        return "FAIL", "no PC after re-attach"
    return "PASS", ""

VERDICTS = {
    1: ("attach + monitor version", verdict_G1),
    2: ("`(gdb) load` reflashes target",  verdict_G2),
    3: ("five simultaneous breakpoints",  verdict_G3),
    4: ("break main + continue hit",      verdict_G4),
    6: ("monitor reset/halt/version verbs",  verdict_G6),
    7: ("detach + re-attach lifecycle",   verdict_G7),
}

# ──────────────────────────────────────────────────────────────────────
#  Main
# ──────────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(description="Group-G GDB acceptance harness")
    ap.add_argument("--port",       default=os.environ.get("HW_PORT", "/dev/ttyAMA2"),
                    help="UPDI serial port (default %(default)s)")
    ap.add_argument("--elf",        default=os.environ.get("HW_TEST_ELF",
                                            "build/fixtures/gdb_target.elf"),
                    help="ELF to load via `(gdb) load` (default %(default)s)")
    ap.add_argument("--rsp-port",   type=int,
                    default=int(os.environ.get("HW_RSP_PORT", "1234")),
                    help="TCP port for avrOSdb (default %(default)s)")
    ap.add_argument("--avros-bin",  default=os.environ.get("HW_AVROS_BIN",
                                            "build/avrOSdb"))
    ap.add_argument("--gdb-bin",    default=os.environ.get("HW_GDB_BIN",
                                            "avr-gdb"))
    ap.add_argument("--gdb-timeout", type=float, default=0.0,
                    help="Override the per-test avr-gdb timeout (s); "
                         "0 uses the built-in per-test caps")
    ap.add_argument("--verbose", "-v", action="count", default=0)
    ap.add_argument("--keep-transcript", action="store_true",
                    help="Don't delete the GDB transcript on success")
    args = ap.parse_args()

    if not os.path.isfile(args.avros_bin) or not os.access(args.avros_bin, os.X_OK):
        print(f"hw-test: gdb_acceptance: missing or non-executable {args.avros_bin}",
              file=sys.stderr)
        return 1
    if not os.path.isfile(args.elf):
        print(f"hw-test: gdb_acceptance: missing fixture ELF {args.elf}",
              file=sys.stderr)
        return 1

    print(
        "hw-test: ─── Group G (full-stack avr-gdb acceptance) ──────────────",
        flush=True,
    )

    # 1) Spawn avrOSdb.
    server_log = "/tmp/avrosdb_gdb_g.log"
    server = spawn_server(args.avros_bin, args.port, args.rsp_port,
                          args.elf, server_log)
    try:
        if not wait_for_tcp(args.rsp_port, timeout_s=8.0):
            print(f"hw-test: server did not bind 127.0.0.1:{args.rsp_port}",
                  file=sys.stderr)
            print(f"hw-test: server log: {server_log}", file=sys.stderr)
            return 1

        any_fail = False
        all_transcripts: List[str] = []
        for n in sorted(VERDICTS):
            title, fn = VERDICTS[n]
            script = build_test_script(n, args.rsp_port)
            with tempfile.NamedTemporaryFile("w", suffix=f"_G{n}.gdb",
                                             delete=False,
                                             encoding="utf-8") as fh:
                fh.write(script)
                script_path = fh.name
            t0 = time.monotonic()
            timeout_s = args.gdb_timeout if args.gdb_timeout else \
                        PER_TEST_TIMEOUT.get(n, 15.0)
            # Honour per-test cap even when the user gave a generous
            # global --gdb-timeout (e.g. for verbose debug).
            if args.gdb_timeout > PER_TEST_TIMEOUT.get(n, 15.0):
                timeout_s = args.gdb_timeout
            else:
                timeout_s = PER_TEST_TIMEOUT.get(n, 15.0)
            try:
                cp = subprocess.run(
                    [args.gdb_bin, "-batch", "-nx", "-x", script_path,
                     args.elf],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    stdin=subprocess.DEVNULL,
                    timeout=timeout_s, check=False,
                )
                transcript = cp.stdout.decode("utf-8", errors="replace")
                timed_out = False
            except subprocess.TimeoutExpired as exc:
                transcript = (exc.stdout or b"").decode("utf-8",
                                                        errors="replace")
                transcript += (f"\n*** avr-gdb -batch timed out after "
                               f"{timeout_s:.0f}s ***\n")
                timed_out = True
            dt_ms = (time.monotonic() - t0) * 1000.0
            all_transcripts.append(f"\n===== G{n} =====\n" + transcript)

            if args.verbose:
                print(f"hw-test: --- G{n} transcript ---", file=sys.stderr)
                print(transcript, file=sys.stderr)

            if timed_out:
                emit(Result(f"G{n}", title, "FAIL",
                            f"avr-gdb timed out after {timeout_s:.0f}s",
                            dt_ms))
                any_fail = True
            else:
                status, detail = fn(transcript)
                emit(Result(f"G{n}", title, status, detail, dt_ms))
                if status == "FAIL":
                    any_fail = True

            try:
                os.unlink(script_path)
            except OSError:
                pass

        print(
            "hw-test: ─────────────────────────────────────────────────────────",
            flush=True,
        )
        passed  = sum(1 for line in [] for _ in line)  # placeholder
        # Recompute from result tags emitted above by re-parsing not
        # needed — track inline.
        # (Counts derived from `any_fail` plus per-test outcomes.)
        # Simpler: print summary using a second pass over VERDICTS by
        # re-judging the transcripts we kept.
        per_results = []
        for idx, n in enumerate(sorted(VERDICTS)):
            tr = all_transcripts[idx]
            if "*** avr-gdb -batch timed out" in tr:
                per_results.append("SKIP" if n == 5 else "FAIL")
            else:
                per_results.append(VERDICTS[n][1](tr)[0])
        passed  = sum(1 for s in per_results if s == "PASS")
        skipped = sum(1 for s in per_results if s == "SKIP")
        failed  = sum(1 for s in per_results if s == "FAIL")
        print(f"hw-test: {passed} passed, {failed} failed, "
              f"{skipped} skipped", flush=True)

        if any_fail:
            keep = "/tmp/avrosdb_gdb_g_transcript.txt"
            with open(keep, "w", encoding="utf-8") as fh:
                fh.write("".join(all_transcripts))
            print(f"hw-test: transcript kept at {keep}", flush=True)
            print(f"hw-test: server log at  {server_log}", flush=True)

        return 1 if any_fail else 0

    finally:
        kill_server(server)

if __name__ == "__main__":
    sys.exit(main())
