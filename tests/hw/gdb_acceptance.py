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
    G10 select thread 2, verify its synthetic PC, reset, then re-select
        thread 2 without losing the FSM thread model or reverting to the
        live CPU/reset-vector frame
    G11 stop at ``main.c:139`` in the avrOS example and ``step`` into
        ``fsmDispatch()`` rather than running on to the later
        ``gpioClearOutput()`` call

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

THIS_DIR = os.path.dirname(__file__)
DEFAULT_G10_ELF = os.path.join(THIS_DIR, "fixtures", "avrOS_example_main.elf")

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
        tail = f"- {r.detail}" if r.detail else ""
    else:
        stat = SKIP_TAG
        tail = f"- {r.detail}" if r.detail else ""
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
                 log_path: str, extra_args: Optional[List[str]] = None) -> subprocess.Popen:
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        [avros_bin, "--port", str(rsp_port)] + (extra_args or []) + [port, elf],
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
    5: ("break do_32bit_inst\ncontinue\n"
        "stepi\nstepi\nstepi\n"
        "info registers pc\ndelete breakpoints\n"),
    6: ("monitor reset\nflushregs\ninfo registers pc\n"
        "monitor halt\nmonitor version\n"),
    7: ("detach\n"
        "target extended-remote :{rsp_port}\n"
        "info registers pc\n"),
    8: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
        "monitor reset\n"
        "hbreak blink\n"
        "break blink2\n"
        "break blink3\n"
        "continue\n"
        "continue\n"
        "continue\n"
        "continue\n"
        "continue\n"
        "delete breakpoints\n"),
    9: "info threads\n",
        10: ("info threads\n"
            "thread 2\n"
            "info registers pc\n"
            "monitor reset\n"
            "info threads\n"
            "thread 2\n"
            "info registers pc\n"),
            11: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break main.c:139\n"
         "continue\n"
         "step\n"
         "frame\n"
         "info line *$pc\n"),
    # G12: validate the emulated CALL pushed a correct return address.
    # After stepping into fsmDispatch, `finish` runs until the matching
    # RET pops back to the caller.  Must land in main near line 141
    # (the statement immediately after fsmDispatch()).  Issue #40.
    12: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break main.c:139\n"
         "continue\n"
         "step\n"
         "print/x $sp\n"
         "x/8bx 0x807ff8\n"
         "tbreak main.c:141\n"
         "continue\n"
         "print/x $pc\n"
         "print/x $sp\n"
         "frame\n"
         "info line *$pc\n"),
    # G13: cortex-debug attach-sequence replay.  Mirrors the
    # `overrideAttachCommands` array used by the live avrOS repo's
    # .vscode/launch.json so we catch regressions that would break the
    # generic GDB frontend.  Issue #40.
    13: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "info registers pc\n"
         "frame\n"),
}

# Per-test wall-clock cap (s) when running `avr-gdb -batch`. G2 has to
# erase+program a FLASH page over UPDI.
PER_TEST_TIMEOUT: dict[int, float] = {
    1: 10.0,
    2: 30.0,
    3: 20.0,
    4: 15.0,
    5: 15.0,
    6: 15.0,
    7: 15.0,
    8: 15.0,
    10: 15.0,
    11: 20.0,
    12: 25.0,
    13: 20.0,
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

def pc_values(text: str) -> List[int]:
    return [int(v, 16) for v in PC_RE.findall(text)]

def pc_matches_state(pc: int, state: int) -> bool:
    byte_pc = pc << 1
    return pc == state or byte_pc == state or (byte_pc | 1) == state

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
    if not re.search(r"Breakpoint \d+,.*\bdo_32bit_inst\b", sect):
        return "FAIL", "no breakpoint hit at do_32bit_inst"
    pc = pc_value(sect)
    if pc is None or pc == 0:
        return "FAIL", f"PC unexpected after hit: {pc}"
    return "PASS", ""

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

def verdict_G9(sect: str) -> Tuple[str, str]:
    if "FSM" in sect or "state=" in sect:
        return "FAIL", "Found FSM thread labels despite --no-introspect"
    if "Id   Target Id" in sect and re.search(r"1\s+Thread \d+\.1", sect):
        return "PASS", ""
    return "FAIL", "No threads found"

def verdict_G10(sect: str) -> Tuple[str, str]:
    if "Error in sourced command file" in sect:
        return "FAIL", "thread selection or reset command failed"
    if ("Unknown thread 2" in sect or "No thread 2" in sect or
            "Cannot find thread 2" in sect or "No registers." in sect):
        return "FAIL", "thread 2 was lost across reset"
    # Expect thread 2 to appear before and after reset.
    hits = re.findall(r"\b2\s+Thread \d+\.2\b", sect)
    if len(hits) < 2:
        return "FAIL", "thread 2 did not survive the reset sequence"
    states = [int(v, 16) for v in re.findall(r"\b2\s+Thread \d+\.2\b.*state=0x([0-9a-fA-F]+)", sect)]
    if len(states) < 2:
        return "FAIL", "thread 2 state labels missing before or after reset"
    pcs = pc_values(sect)
    if len(pcs) < 2:
        return "FAIL", "no register reads while thread 2 selected"
    if not pc_matches_state(pcs[0], states[0]):
        return "FAIL", f"pre-reset thread 2 PC 0x{pcs[0]:x} not consistent with state 0x{states[0]:x}"
    if not pc_matches_state(pcs[1], states[1]):
        return "FAIL", f"post-reset thread 2 PC 0x{pcs[1]:x} not consistent with state 0x{states[1]:x}"
    return "PASS", ""

def verdict_G11(sect: str) -> Tuple[str, str]:
    if "Error in sourced command file" in sect:
        return "FAIL", "GDB step command failed"
    if "remote failure" in sect.lower():
        return "FAIL", "remote step failed"
    if re.search(r"^#0\s+fsmDispatch\b", sect, re.MULTILINE):
        return "PASS", ""
    if re.search(r"^#0\s+gpioClearOutput\b", sect, re.MULTILINE):
        return "FAIL", "step skipped fsmDispatch and landed in gpioClearOutput"
    return "FAIL", "step did not land in fsmDispatch"

def verdict_G12(sect: str) -> Tuple[str, str]:
    # Issue #40: validates that the emulated CALL pushed a return
    # address the silicon RET pops back into the right place.  After
    # `step` into fsmDispatch we set a temp breakpoint at the line
    # immediately following the CALL (main.c:141) and `continue`.
    # When fsmDispatch executes its RET, silicon pops the bytes we
    # pushed and jumps to 0x3f4 — if our push was wrong the CPU would
    # return to a bogus PC and never reach main.c:141.
    if "Error in sourced command file" in sect:
        return "FAIL", "GDB command failed before RET site"
    if "remote failure" in sect.lower():
        return "FAIL", "remote step/continue failed"
    # The frame banner printed after the final tbreak is the source
    # of truth — `#0  main () at main.c:<line>` — not the earlier
    # tbreak transcript which still mentions main.c:133.
    m = re.search(r"^#0\s+main\b.*?\bmain\.c:(\d+)",
                  sect, re.MULTILINE | re.DOTALL)
    if m is None:
        return "FAIL", "did not return to main (stack push wrong?)"
    ln = int(m.group(1))
    if not (139 <= ln <= 145):
        return "FAIL", f"landed at main.c:{ln} (expected 140..144)"
    return "PASS", ""

def verdict_G13(sect: str) -> Tuple[str, str]:
    # Issue #40: replays the cortex-debug launch.json
    # `overrideAttachCommands` sequence.  Must reach main, have a
    # plausible PC, and report no protocol errors.
    if "Error in sourced command file" in sect:
        return "FAIL", "cortex-debug attach sequence failed"
    if "remote failure" in sect.lower() or "Connection refused" in sect:
        return "FAIL", "remote failure during attach sequence"
    if not re.search(r"[Bb]reakpoint \d+,.*\bmain\b", sect):
        return "FAIL", "tbreak main never hit"
    if not re.search(r"^#0\s+main\b", sect, re.MULTILINE):
        return "FAIL", "frame did not report main"
    pc = pc_value(sect)
    if pc is None or pc == 0:
        return "FAIL", f"PC unexpected after attach sequence: {pc}"
    return "PASS", ""

def verdict_G8(sect: str) -> Tuple[str, str]:
    # Continue must produce breakpoint hit lines for blink, blink2, blink3
    hits_blink = len(re.findall(r"Breakpoint \d+,.*\bblink\b", sect))
    hits_blink2 = len(re.findall(r"Breakpoint \d+,.*\bblink2\b", sect))
    hits_blink3 = len(re.findall(r"Breakpoint \d+,.*\bblink3\b", sect))
    
    if hits_blink < 1:
        return "FAIL", f"expected at least 1 hit at blink, got {hits_blink}"
    if hits_blink2 < 1:
        return "FAIL", f"expected at least 1 hit at blink2, got {hits_blink2}"
    if hits_blink3 < 1:
        return "FAIL", f"expected at least 1 hit at blink3, got {hits_blink3}"
    return "PASS", ""

VERDICTS = {
    1: ("attach + monitor version", verdict_G1),
    2: ("`(gdb) load` reflashes target",  verdict_G2),
    3: ("five simultaneous breakpoints",  verdict_G3),
    4: ("break main + continue hit",      verdict_G4),
    5: ("stepi over 32-bit instructions", verdict_G5),
    6: ("monitor reset/halt/version verbs",  verdict_G6),
    7: ("detach + re-attach lifecycle",   verdict_G7),
    8: ("multiple breakpoints correctly hit during execution", verdict_G8),
    9: ("--no-introspect switch yields single thread", verdict_G9),
    10: ("reset preserves selected FSM register view", verdict_G10),
    11: ("step enters fsmDispatch from main", verdict_G11),
    12: ("finish from fsmDispatch returns to main (CALL stack push)",
         verdict_G12),
    13: ("cortex-debug attach sequence reaches main", verdict_G13),
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
    ap.add_argument("--g10-elf",    default=os.environ.get("HW_GDB_G10_ELF",
                                            DEFAULT_G10_ELF),
                    help="ELF used only for G10 FSM-thread reset coverage "
                         "(default %(default)s)")
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
    if not os.path.isfile(args.g10_elf):
        print(f"hw-test: gdb_acceptance: missing G10 ELF {args.g10_elf}",
              file=sys.stderr)
        return 1

    print(
        "hw-test: --- Group G (full-stack avr-gdb acceptance) ---",
        flush=True,
    )

    # 1) Spawn avrOSdb.
    current_extra_args = ["--log-rsp"]
    current_elf = args.elf
    server_log = "/tmp/avrosdb_gdb_g.log"
    server = spawn_server(args.avros_bin, args.port, args.rsp_port,
                          current_elf, server_log,
                          extra_args=current_extra_args)
    try:
        if not wait_for_tcp(args.rsp_port, timeout_s=8.0):
            print(f"hw-test: server did not bind 127.0.0.1:{args.rsp_port}",
                  file=sys.stderr)
            print(f"hw-test: server log: {server_log}", file=sys.stderr)
            return 1

        any_fail = False
        all_transcripts: List[str] = []
        for n in sorted(VERDICTS):
            if n == 9:
                desired_extra_args = ["--log-rsp", "--no-introspect"]
            elif n in (10, 11, 12, 13):
                desired_extra_args = ["--log-rsp", "--load"]
            else:
                desired_extra_args = ["--log-rsp"]
            desired_elf = args.g10_elf if n in (10, 11, 12, 13) else args.elf
            force_restart = (n in (11, 12, 13))
            if (force_restart or desired_extra_args != current_extra_args or
                    desired_elf != current_elf):
                kill_server(server)
                server_log = f"/tmp/avrosdb_gdb_g{n}.log"
                server = spawn_server(args.avros_bin, args.port, args.rsp_port,
                                      desired_elf, server_log,
                                      extra_args=desired_extra_args)
                current_extra_args = desired_extra_args
                current_elf = desired_elf
                if not wait_for_tcp(args.rsp_port, timeout_s=8.0):
                    print(f"hw-test: G{n} server did not bind", file=sys.stderr)
                    return 1

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
                     desired_elf],
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
            "hw-test: ---------------------------------------------------------",
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
