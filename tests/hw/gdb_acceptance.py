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
    G10 ``monitor avros tasks`` lists the avrOS FSMs both before and after
        a ``monitor reset`` — the introspection view survives a reset and
        no FSM-as-GDB-thread regresses (single GDB thread throughout)
    G11 stop at ``main.c:139`` in the avrOS example and ``step`` into
        ``fsmDispatch()`` rather than running on to the later
        ``gpioClearOutput()`` call
    G17 break inside ``locals_probe()`` and read back every local (one per
        representative C data type) from its own frame, verifying each
        reported value matches the constant the fixture assigned — catches
        SP/SRAM read bugs that surface as garbage locals in GDB / Cortex-Debug
    G18..G24 (Phase 14, HLR-070) emulate a full *interactive* debug session
        against the ``gdb_debug_session`` fixture's ``main -> top -> mid ->
        leaf`` call chain: a multi-frame backtrace + frame selection (G18);
        ``step``/``next``/``finish`` with the correct return value (G19);
        multiple breakpoints hit in call order (G20); a conditional breakpoint
        (G21); global scalar/struct/array reads (G22); per-frame ``info args``
        / ``info locals`` (G23); and a start-to-finish capstone session (G24)

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
    # G10: the avrOS FSM introspection view must survive a target reset.
    # `monitor avros tasks` lists the FSMs (state=...) before and after a
    # `monitor reset`; both listings must enumerate FSMs and the session must
    # stay a single GDB thread (Phase 14 replaced the FSM-as-GDB-threads model
    # with `monitor avros tasks` — see G14).
    10: ("info threads\n"
         "monitor avros tasks\n"
         "monitor reset\n"
         "monitor avros tasks\n"
         "info threads\n"),
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
    # G14: demote regression net — the monitor-reset -> break -> hit -> bt ->
    # info-threads -> monitor-avros-tasks sequence that used to crash avr-gdb.
    # bt must be a clean backtrace reaching main, info threads must show a
    # single GDB thread, and `monitor avros tasks` must list the FSMs via
    # introspection (which replaces FSM-as-GDB-threads).  Issue #42.
    14: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "bt\n"
         "info threads\n"
         "monitor avros tasks\n"),
    # G15: HW-comparator arbiter (issue #45). A user hardware breakpoint
    # occupies the single user comparator (slot 0). fsmDispatch's first
    # statement is a 32-bit LDS (0x1c0a: `lds r24, 0x467E`); single-stepping
    # through it uses the RESERVED comparator (slot 1), and the user hbreak
    # must survive — so `continue` re-hits fsmDispatch (>=2 hits total).
    # (No software breakpoint is used to position at the LDS: a SW BP would
    # add an NVMPROG round-trip that is unrelated to what this test checks.)
    15: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "hbreak fsmDispatch\n"          # user HW breakpoint -> slot 0
         "continue\n"                    # hit fsmDispatch entry (#1)
         "x/4i $pc\n"                    # entry includes the 32-bit lds
         "stepi\n"                       # step through the entry, over the LDS
         "stepi\n"
         "stepi\n"                       # (slot 1 = reserved step comparator)
         "x/i $pc\n"
         "continue\n"                    # next dispatch must re-hit the hbreak (#2)
         "info registers pc\n"),
    # G16: parallel to G15 with a user SOFTWARE breakpoint (FLASH BREAK, Z0)
    # instead of a hardware one. The `mem ... rw` hints let GDB place a SW
    # breakpoint in flash. Stepping over the 32-bit LDS at fsmDispatch entry
    # uses the reserved HW comparator (slot 1); the user SW breakpoint must
    # coexist with that and survive the step.
    #
    # NB: unlike the HW case (G15) we do NOT verify survival by re-firing.
    # Every SW-breakpoint flash patch enters NVMPROG, whose mandatory
    # ASI_RESET_REQ system-reset pulse (updi.c:updi_enter_nvmprog) resets the
    # AVR-Dx peripherals — including the avrOS system-tick timer.  After any
    # SW-BP flash op the firmware's tick-driven `sysSleep()` in the main loop
    # blocks, so the loop never completes another iteration and fsmDispatch is
    # never re-entered.  (Verified on hardware: fsmDispatch returns to the
    # main loop — gpioClearOutput is reached — but the next fsmDispatch is
    # not, because sysSleep waits forever for a tick the reset stopped.)  This
    # is a fundamental UPDI/NVMPROG constraint, not an avrOSdb defect; the HW
    # path (G15) is the one that exercises re-fire.  We therefore verify SW-BP
    # survival structurally: the BP fires once, the three stepis advance the
    # PC across the whole 32-bit LDS block, and the BP is still installed
    # afterward (`info breakpoints` shows it, hit once).
    16: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "set breakpoint auto-hw off\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break fsmDispatch\n"           # user SOFTWARE breakpoint (Z0)
         "continue\n"                    # hit fsmDispatch entry (#1)
         "x/4i $pc\n"                    # entry includes the 32-bit lds
         "stepi\n"                       # step off the SW BP, over the 32-bit
         "stepi\n"                       #   LDS, via the reserved step slot
         "stepi\n"
         "info registers pc\n"           # PC advanced past the LDS block
         "info breakpoints\n"),          # user SW BP survived (still installed)
    # G17: local-variable value correctness + backtrace integrity. Break
    # INSIDE locals_probe() on the g_probe_hit anchor line where every local
    # is assigned and still live, read each back, then `bt`. Both surfaces
    # read SRAM off the stack and both were broken by the ST_PTR_WORD
    # stale-high-byte bug (a Flash read by GDB left the UPDI pointer high byte
    # at 0x80, so the following 16-bit-pointer SRAM read was misdirected into
    # mapped Flash and returned erased 0xFF): locals came back as 0xFF/garbage
    # and the return-address read gave a bogus 0x1fffe frame. With the 24-bit
    # ST_PTR_LONG fix every local reads its known constant and `bt` reaches
    # main. The `break gdb_locals.c:67` line must track the g_probe_hit anchor
    # in tests/fixtures/gdb_locals.c. Runs against the -O0 gdb_locals fixture
    # so the values are deterministic and not optimiser-dependent.
    17: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break gdb_locals.c:67\n"       # the g_probe_hit anchor line
         "continue\n"                    # stop inside locals_probe()
         "info locals\n"
         "print u8\n"
         "print i8\n"
         "print u16\n"
         "print i16\n"
         "print u32\n"
         "print i32\n"
         "print u64\n"
         "print ch\n"
         "print flag\n"
         "print f\n"
         "print arr\n"
         "print pt\n"
         "print name\n"
         "print *pmark\n"
         "bt\n"),                        # must reach main (return-addr SRAM read)
    # ── Phase 14 interactive debug-session suite (G18..G24, HLR-070) ─────────
    # All run against the -O0 gdb_debug_session fixture (--dbg-elf), flashed via
    # the server's --load path.  The fixture's call chain is
    # main -> top(7) -> mid(7) -> leaf(7,2) then leaf(7,3); breakpoints are set
    # by SYMBOL (robust to line edits).  Known first-hit values:
    #   leaf(7,2)=49397  leaf(7,3)=49405  g_marker=49374
    #   g_cfg={base=100,gain=-7}  g_arr={10,20,30,40}
    #
    # G18: multi-frame backtrace + frame selection. Stop in leaf(7,2); `bt`
    # must unwind leaf->mid->top->main in order; frame selection + `info args`
    # must show each frame's own parameter (mid n=7, top seed=7).
    18: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break leaf\n"
         "continue\n"                    # stop in leaf(7,2)
         "bt\n"                          # #0 leaf #1 mid #2 top #3 main
         "info args\n"                   # frame 0: a=7 b=2
         "frame 1\n"                     # select mid
         "info args\n"                   # n = 7
         "frame 2\n"                     # select top
         "info args\n"                   # seed = 7
         "up\n"                          # -> main
         "down\n"                        # -> top
         "delete breakpoints\n"),
    # G19: source-level step (into) / next (over) / finish (out + return value).
    # From top(7): `step` descends into mid; `next` steps over leaf(7,2);
    # `step` descends into leaf(7,3); `finish` returns 49405.
    19: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break top\n"
         "continue\n"                    # stop in top(7) @ r = mid(seed)
         "delete breakpoints\n"          # clear so stepping is undisturbed
         "step\n"                        # -> into mid(7) @ acc = 0
         "bt\n"                          # step-into proof: #0 mid #1 top
         "step\n"                        # @ acc += leaf(n,2)
         "next\n"                        # over leaf(7,2) -> @ acc += leaf(n,3)
         "step\n"                        # -> into leaf(7,3)
         "finish\n"),                    # Value returned is N = 49405
    # G20: multiple simultaneous breakpoints, hit in call order top->mid->leaf.
    20: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break top\n"
         "break mid\n"
         "break leaf\n"
         "info breakpoints\n"            # lists 3 user BPs
         "continue\n"                    # hit top
         "continue\n"                    # hit mid
         "continue\n"                    # hit leaf
         "delete breakpoints\n"),
    # G21: conditional breakpoint. `break leaf if b == 3` must skip leaf(7,2)
    # (b==2) and stop only at leaf(7,3).
    21: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break leaf if b == 3\n"
         "continue\n"                    # stop at leaf(7,3), skipping leaf(7,2)
         "print a\n"                     # 7
         "print b\n"                     # 3
         "bt\n"
         "delete breakpoints\n"),
    # G22: globals — scalar, struct, array, struct field, array element.
    22: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break leaf\n"
         "continue\n"                    # stop in leaf (C runtime .data init done)
         "print g_marker\n"              # 49374
         "print g_cfg\n"                 # {base = 100, gain = -7}
         "print g_cfg.base\n"            # 100
         "print g_cfg.gain\n"            # -7
         "print g_arr\n"                 # {10, 20, 30, 40}
         "print g_arr[2]\n"              # 30
         "print/x g_marker\n"            # 0xc0de
         "delete breakpoints\n"),
    # G23: per-frame info args + info locals with exact values. At leaf(7,2)
    # args are live at entry; after two `next` the locals prod/sum are assigned.
    23: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break leaf\n"
         "continue\n"                    # stop in leaf(7,2)
         "info args\n"                   # a = 7, b = 2
         "next\n"                        # exec prod = a*b
         "next\n"                        # exec sum  = a+b
         "info locals\n"                 # prod = 14, sum = 9
         "print prod\n"                  # 14
         "print sum\n"                   # 9
         "delete breakpoints\n"),
    # G24: capstone — one realistic interactive session start to finish.
    24: ("mem 0x0 0x20000 rw\nmem 0x804000 0x808000 rw\n"
         "monitor reset\n"
         "tbreak main\n"
         "continue\n"
         "break top\n"
         "break leaf if b == 3\n"
         "continue\n"                    # hit top(7)
         "bt\n"                          # #0 top #1 main
         "info args\n"                   # seed = 7
         "continue\n"                    # conditional -> leaf(7,3)
         "bt\n"                          # #0 leaf #1 mid #2 top #3 main
         "info args\n"                   # a=7 b=3
         "print g_marker\n"              # 49374
         "finish\n"                      # Value returned is N = 49405
         "delete breakpoints\n"),
    # G25: read an avrOS FSM state-name string. Stopped inside ledsFlash(),
    # `stateMachine->currStateName` is a char* into the AVR-Dx mapped-flash
    # data window (.rodata aliased at data >= 0x8000). Recreates the
    # "couldn't read the address" failure for flash-resident strings.
    25: ("break ledsFlash\n"
         "continue\n"
         "print stateMachine\n"                 # (fsmStateMachine_t *) 0x....
         "print/x stateMachine->currStateName\n"# raw 16-bit mapped-flash ptr
         "print stateMachine->currStateName\n"  # the failing string read
         "x/s stateMachine->currStateName\n"    # ditto, as a C string
         "delete breakpoints\n"),
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
    8: 40.0,
    10: 15.0,
    11: 20.0,
    12: 25.0,
    13: 20.0,
    14: 20.0,
    15: 25.0,
    16: 25.0,
    17: 25.0,
    18: 30.0,
    19: 35.0,
    20: 30.0,
    21: 30.0,
    22: 30.0,
    23: 30.0,
    24: 40.0,
    25: 25.0,
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
    # avr-gdb prints a single GDB thread for --no-introspect. Accept both the
    # bare "Thread 1" form (avr-gdb 16.3) and the legacy "Thread <pid>.1" form.
    if "Id   Target Id" in sect and re.search(r"1\s+Thread (?:\d+\.)?1\b", sect):
        return "PASS", ""
    return "FAIL", "No threads found"

def verdict_G10(sect: str) -> Tuple[str, str]:
    # The avrOS FSM introspection view must survive a `monitor reset`.
    # Phase 14 replaced the FSM-as-GDB-threads model with `monitor avros
    # tasks`, so we no longer select a synthetic thread 2; instead the FSM
    # listing (state=...) must appear both before and after the reset, and
    # the session must remain a single GDB thread (no Thread 2 regression).
    if "Error in sourced command file" in sect:
        return "FAIL", "monitor command failed"
    if re.search(r"\bThread\s+2\b", sect):
        return "FAIL", "FSM-as-GDB-threads regressed (Thread 2 present)"
    # Two `monitor avros tasks` listings, one before and one after reset.
    # Each FSM line is `  * <name>  state=<state>` (monitor.c): the state is a
    # NAME string (a state-function name, or the `(init)` fallback before the
    # avrOS runtime has populated the descriptor at the reset vector), never a
    # hex address — so match `state=` followed by any non-space token. Two
    # listings (before + after the reset) yield at least two matches.
    listings = len(re.findall(r"state=\S", sect))
    if listings < 2:
        return "FAIL", "monitor avros tasks did not list FSMs before and after reset"
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

def verdict_G16(sect: str) -> Tuple[str, str]:
    # Parallel to G15 with a user SOFTWARE breakpoint. The SW BP must coexist
    # with single-stepping over the 32-bit LDS at fsmDispatch entry (which
    # uses the reserved step comparator) and survive it.  Re-firing is NOT
    # checked: every SW-BP flash patch enters NVMPROG, whose system-reset
    # pulse resets the avrOS tick timer, so the firmware's tick-driven
    # sysSleep blocks and the dispatch loop cannot iterate again (see the
    # PER_TEST_CMDS comment).  Survival is verified structurally instead.
    if "lds" not in sect.lower():
        return "FAIL", "did not disassemble the 32-bit LDS in fsmDispatch"
    hits = len(re.findall(r"Breakpoint \d+,.*\bfsmDispatch\b", sect))
    if hits < 1:
        return "FAIL", "SW breakpoint at fsmDispatch never fired"
    # Three stepis must advance the PC across the LDS block.  The breakpoint
    # sits at fsmDispatch+10 (0x1c0a); each LDS is 4 bytes, so after 3 stepis
    # the PC is at fsmDispatch+22 (0x1c16).  avr-gdb prints `info registers
    # pc` as a *word* address but annotates it with the byte-offset symbol
    # `<fsmDispatch+NN>`, so match that offset (decimal) and require it to be
    # past the entry LDS — proving the reserved-comparator step over the
    # 32-bit LDS worked with the SW BP set.
    m = re.search(r"\bpc\b\s+0x[0-9a-fA-F]+\s+<fsmDispatch\+(\d+)>", sect)
    if m is None:
        return "FAIL", "no `info registers pc` landing in fsmDispatch after stepping"
    off = int(m.group(1))
    if off < 14:
        return "FAIL", (f"PC at fsmDispatch+{off} after 3 stepis "
                        f"(expected past the LDS block, +22)")
    # The user SW breakpoint must still be installed after the stepi — GDB's
    # `info breakpoints` reports it as hit once and still kept.
    if not re.search(r"breakpoint already hit", sect):
        return "FAIL", "user SW breakpoint at fsmDispatch did not survive the stepi"
    return "PASS", ""

def verdict_G15(sect: str) -> Tuple[str, str]:
    # The single-step over the 32-bit LDS must use the reserved comparator
    # and leave the user hbreak intact. The hbreak at fsmDispatch must fire
    # both before the stepi (#1) and again on the next dispatch after it
    # (#2): >= 2 hits proves it survived the LDS step. If the step had
    # clobbered the user comparator, the final `continue` would not re-hit.
    if "lds" not in sect.lower():
        return "FAIL", "did not reach / disassemble the 32-bit LDS at fsmDispatch entry"
    hits = len(re.findall(r"Breakpoint \d+,.*\bfsmDispatch\b", sect))
    if hits < 2:
        return "FAIL", f"hbreak at fsmDispatch fired {hits}x (<2): did not survive the LDS stepi"
    return "PASS", ""

def verdict_G14(sect: str) -> Tuple[str, str]:
    # Demote regression net (issue #42): bt must produce a clean backtrace
    # reaching main (avr-gdb must NOT crash on the FSM-aware session), info
    # threads must show a single GDB thread, and `monitor avros tasks` must
    # list FSMs via introspection.
    if not re.search(r"#0\s+.*\bmain\b", sect):
        return "FAIL", "bt produced no main frame (possible avr-gdb crash)"
    if re.search(r"\bThread\s+2\b", sect):
        return "FAIL", "more than one GDB thread present (FSM-as-threads regressed)"
    if "state=" not in sect:
        return "FAIL", "monitor avros tasks listed no FSMs"
    return "PASS", ""

def verdict_G17(sect: str) -> Tuple[str, str]:
    # Local-variable value correctness. Stopped inside locals_probe(), each
    # `print <var>` must report the exact constant the fixture assigned. A
    # wrong value means avrOSdb mis-read SP/the frame pointer or mis-read
    # SRAM — the failure that shows garbage locals in GDB / Cortex-Debug. We
    # match each value as it appears in GDB's default formatting; the
    # constants are deliberately distinctive.
    if "Error in sourced command file" in sect:
        return "FAIL", "GDB command failed before reading locals"
    if "remote failure" in sect.lower():
        return "FAIL", "remote read failed while inspecting locals"
    if re.search(r"No symbol .* in current context", sect):
        return "FAIL", "locals not in scope (frame/DWARF not resolved)"
    if re.search(r"No locals\.", sect):
        return "FAIL", "`info locals` reported no locals in locals_probe frame"

    # (label, regex against the transcript) for each typed local. Anchored on
    # `= <value>` so we match the `print` result, not an incidental address.
    checks = [
        ("u8",        r"=\s*165\b"),
        ("i8",        r"=\s*-42\b"),
        ("u16",       r"=\s*48879\b"),
        ("i16",       r"=\s*-12345\b"),
        ("u32",       r"=\s*3735928559\b"),
        ("i32",       r"=\s*-123456789\b"),
        ("u64",       r"=\s*81985529216486895\b"),
        ("ch",        r"=\s*81 '[Q]'"),
        ("flag",      r"=\s*true\b"),
        ("f",         r"=\s*3\.5\b"),
        ("arr",       r"=\s*\{4369,\s*8738,\s*13107,\s*17476\}"),
        ("pt",        r"=\s*\{x\s*=\s*1000,\s*y\s*=\s*-2000\}"),
        ("name",      r'=\s*"avrOS"'),
        ("*pmark",    r"=\s*51966\b"),
    ]
    missing = [name for name, pat in checks
               if not re.search(pat, sect)]
    if missing:
        return "FAIL", ("wrong/missing local value(s): "
                        + ", ".join(missing))
    # Backtrace must unwind locals_probe -> main. The same SRAM-read bug that
    # corrupts locals also corrupts the return address read off the stack,
    # which surfaces as a bogus "#1  0x0001fffe in ?? ()" frame.
    if not re.search(r"^#1\s+.*\bmain\b", sect, re.MULTILINE):
        if re.search(r"^#1\s+0x0*1fffe", sect, re.MULTILINE):
            return "FAIL", ("backtrace return address read as 0xFF "
                            "(#1 = 0x1fffe): stack SRAM read corrupted")
        return "FAIL", "backtrace did not unwind locals_probe -> main"
    return "PASS", ""

# ── Phase 14 interactive debug-session verdicts (G18..G24, HLR-070) ─────────

# Backtrace-frame patterns for the main->top->mid->leaf chain. Frame 0 prints
# bare (`#0  leaf ...`); deeper frames carry a return address (`#1  0x.. in
# mid ...`), so the `in` prefix is optional.
_BT_LEAF = re.compile(r"^#0\s+leaf\b", re.MULTILINE)
_BT_MID  = re.compile(r"^#1\s+(?:0x[0-9a-fA-F]+\s+in\s+)?mid\b", re.MULTILINE)
_BT_TOP  = re.compile(r"^#2\s+(?:0x[0-9a-fA-F]+\s+in\s+)?top\b", re.MULTILINE)
_BT_MAIN = re.compile(r"^#3\s+(?:0x[0-9a-fA-F]+\s+in\s+)?main\b", re.MULTILINE)

def verdict_G18(sect: str) -> Tuple[str, str]:
    # Full backtrace in frame order reaching main, plus per-frame info args.
    if "Error in sourced command file" in sect:
        return "FAIL", "GDB command failed"
    if "remote failure" in sect.lower():
        return "FAIL", "remote read failed during backtrace"
    for label, rx in (("#0 leaf", _BT_LEAF), ("#1 mid", _BT_MID),
                      ("#2 top", _BT_TOP), ("#3 main", _BT_MAIN)):
        if not rx.search(sect):
            return "FAIL", f"backtrace missing frame {label}"
    if not re.search(r"\bn = 7\b", sect):
        return "FAIL", "frame 1 (mid) `info args` did not show n = 7"
    if not re.search(r"\bseed = 7\b", sect):
        return "FAIL", "frame 2 (top) `info args` did not show seed = 7"
    return "PASS", ""

def verdict_G19(sect: str) -> Tuple[str, str]:
    # step (into mid), next (over leaf(7,2)), step (into leaf(7,3)), finish.
    if "Error in sourced command file" in sect:
        return "FAIL", "GDB step command failed"
    if "remote failure" in sect.lower():
        return "FAIL", "remote step/finish failed"
    if not re.search(r"^#0\s+mid\b", sect, re.MULTILINE):
        return "FAIL", "`step` did not descend into mid (no #0 mid frame)"
    # `finish` from leaf(7,3) reports the exact return value. Reaching it
    # requires the step-into/next-over/step-into chain to have landed in
    # leaf(7,3) — value 49405 (not 49397, which would be leaf(7,2)).
    if not re.search(r"Value returned is\s+\$\d+\s*=\s*49405\b", sect):
        if re.search(r"Value returned is\s+\$\d+\s*=\s*49397\b", sect):
            return "FAIL", ("finish returned 49397 (leaf(7,2)): next/step "
                            "landed in the wrong call")
        return "FAIL", "finish did not report leaf(7,3) return value 49405"
    return "PASS", ""

def verdict_G20(sect: str) -> Tuple[str, str]:
    # Three breakpoints listed, then hit in call order top -> mid -> leaf.
    bps = re.findall(r"^\s*\d+\s+breakpoint", sect, re.MULTILINE)
    if len(bps) < 3:
        return "FAIL", f"only {len(bps)} BPs listed (expected 3)"
    m_top  = re.search(r"Breakpoint \d+,\s+top\b", sect)
    m_mid  = re.search(r"Breakpoint \d+,\s+mid\b", sect)
    m_leaf = re.search(r"Breakpoint \d+,\s+leaf\b", sect)
    missing = [n for n, m in (("top", m_top), ("mid", m_mid), ("leaf", m_leaf))
               if not m]
    if missing:
        return "FAIL", "missing breakpoint hit(s): " + ", ".join(missing)
    if not (m_top.start() < m_mid.start() < m_leaf.start()):
        return "FAIL", "breakpoint hit order was not top -> mid -> leaf"
    return "PASS", ""

def verdict_G21(sect: str) -> Tuple[str, str]:
    # Conditional `break leaf if b == 3`: must stop at leaf(7,3), never (7,2).
    if "Error in sourced command file" in sect:
        return "FAIL", "conditional-breakpoint command failed"
    if re.search(r"Breakpoint \d+,\s+leaf\s*\(a\s*=\s*7,\s*b\s*=\s*2\)", sect):
        return "FAIL", "stopped at leaf(7,2) despite `if b == 3` condition"
    if not re.search(r"Breakpoint \d+,\s+leaf\s*\(a\s*=\s*7,\s*b\s*=\s*3\)", sect):
        return "FAIL", "did not stop at leaf with b == 3 (condition not honoured)"
    return "PASS", ""

def verdict_G22(sect: str) -> Tuple[str, str]:
    # Globals: scalar, struct (+fields), array (+element). Exact values.
    if "Error in sourced command file" in sect:
        return "FAIL", "global-print command failed"
    if re.search(r"No symbol .* in current context", sect):
        return "FAIL", "globals not resolved (DWARF/symbols missing)"
    checks = [
        ("g_marker",     r"=\s*49374\b"),
        ("g_cfg",        r"\{base\s*=\s*100,\s*gain\s*=\s*-7\}"),
        ("g_cfg.base",   r"base\s*=\s*100\b"),
        ("g_cfg.gain",   r"gain\s*=\s*-7\b"),
        ("g_arr",        r"\{10,\s*20,\s*30,\s*40\}"),
        ("g_arr[2]",     r"=\s*30\b"),
        ("g_marker/x",   r"=\s*0xc0de\b"),
    ]
    missing = [name for name, pat in checks
               if not re.search(pat, sect, re.IGNORECASE)]
    if missing:
        return "FAIL", "wrong/missing global value(s): " + ", ".join(missing)
    return "PASS", ""

def verdict_G23(sect: str) -> Tuple[str, str]:
    # Per-frame info args (a=7,b=2) and, after two `next`, info locals
    # (prod=14, sum=9).
    if "Error in sourced command file" in sect:
        return "FAIL", "locals/args command failed"
    if re.search(r"No symbol .* in current context", sect):
        return "FAIL", "locals/args not in scope"
    checks = [
        ("a (arg)", r"\ba = 7\b"),
        ("b (arg)", r"\bb = 2\b"),
        ("prod",    r"\bprod = 14\b"),
        ("sum",     r"\bsum = 9\b"),
    ]
    missing = [name for name, pat in checks if not re.search(pat, sect)]
    if missing:
        return "FAIL", "wrong/missing local/arg value(s): " + ", ".join(missing)
    return "PASS", ""

def verdict_G24(sect: str) -> Tuple[str, str]:
    # Capstone: top hit (seed=7, bt->main), conditional leaf(7,3) with full
    # backtrace, global read, finish return value.
    if "Error in sourced command file" in sect:
        return "FAIL", "capstone session command failed"
    if "remote failure" in sect.lower():
        return "FAIL", "remote failure during capstone session"
    if not re.search(r"Breakpoint \d+,\s+top\b", sect):
        return "FAIL", "top breakpoint never hit"
    if not re.search(r"\bseed = 7\b", sect):
        return "FAIL", "top frame `info args` did not show seed = 7"
    if not re.search(r"Breakpoint \d+,\s+leaf\s*\(a\s*=\s*7,\s*b\s*=\s*3\)", sect):
        return "FAIL", "conditional leaf(7,3) breakpoint not honoured"
    for label, rx in (("#0 leaf", _BT_LEAF), ("#1 mid", _BT_MID),
                      ("#2 top", _BT_TOP), ("#3 main", _BT_MAIN)):
        if not rx.search(sect):
            return "FAIL", f"capstone backtrace missing frame {label}"
    if not re.search(r"=\s*49374\b", sect):
        return "FAIL", "g_marker global misread (expected 49374)"
    if not re.search(r"Value returned is\s+\$\d+\s*=\s*49405\b", sect):
        return "FAIL", "finish from leaf(7,3) did not return 49405"
    return "PASS", ""

def verdict_G25(sect: str) -> Tuple[str, str]:
    # avrOS FSM state-name string read. Stopped inside ledsFlash(), GDB reads
    # stateMachine->currStateName — a char* into the AVR-Dx mapped-flash data
    # window (.rodata aliased at data >= 0x8000). Before the dh_read_mem
    # mapped-flash translation + qXfer ROM advertisement, GDB refuses the read
    # ("Cannot access memory at address 0x80....") because the window is
    # undescribed; after the fix the flash-resident string reads back.
    if "Error in sourced command file" in sect:
        return "FAIL", "GDB command failed before reading currStateName"
    if "remote failure" in sect.lower():
        return "FAIL", "remote read failed while inspecting currStateName"
    if re.search(r"No symbol .* in current context", sect):
        return "FAIL", "stateMachine not in scope (frame/DWARF not resolved)"
    if re.search(r"Cannot access memory at address", sect):
        m = re.search(r"Cannot access memory at address (0x[0-9a-fA-F]+)", sect)
        where = m.group(1) if m else "?"
        return "FAIL", (f"currStateName string unreadable at {where} "
                        "(mapped-flash data window not served)")
    # A quoted, non-empty string in the transcript can only come from the
    # currStateName print / x/s — its presence means the flash read worked.
    m = re.search(r'=\s*(?:0x[0-9a-fA-F]+\s+)?"([^"]*)"', sect)
    if not m or m.group(1) == "":
        return "FAIL", "currStateName did not resolve to a non-empty string"
    return "PASS", f'currStateName = "{m.group(1)}"'

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
    10: ("monitor avros tasks survives a reset (single GDB thread)", verdict_G10),
    11: ("step enters fsmDispatch from main", verdict_G11),
    12: ("finish from fsmDispatch returns to main (CALL stack push)",
         verdict_G12),
    13: ("cortex-debug attach sequence reaches main", verdict_G13),
    14: ("demote: bt clean single-thread + avros tasks lists FSMs", verdict_G14),
    15: ("HW-comparator arbiter: user hbreak survives a 32-bit LDS stepi", verdict_G15),
    16: ("HW-comparator arbiter: user SW breakpoint survives a 32-bit LDS stepi", verdict_G16),
    17: ("local variable values are reported correctly", verdict_G17),
    18: ("multi-frame backtrace + frame selection", verdict_G18),
    19: ("step into / next over / finish (return value)", verdict_G19),
    20: ("multiple breakpoints hit in call order", verdict_G20),
    21: ("conditional breakpoint honoured (leaf if b==3)", verdict_G21),
    22: ("globals: scalar, struct, array reads", verdict_G22),
    23: ("per-frame info args + info locals values", verdict_G23),
    24: ("capstone: full interactive debug session", verdict_G24),
    25: ("avrOS FSM state-name string read (mapped-flash char*)", verdict_G25),
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
                    help="avrOS ELF used for G10 FSM-introspection reset "
                         "coverage (default %(default)s)")
    ap.add_argument("--g17-elf",    default=os.environ.get("HW_GDB_G17_ELF",
                                            "build/fixtures/gdb_locals.elf"),
                    help="ELF used only for G17 local-variable value "
                         "coverage; built at -O0 (default %(default)s)")
    ap.add_argument("--dbg-elf",    default=os.environ.get("HW_GDB_DBG_ELF",
                                            "build/fixtures/gdb_debug_session.elf"),
                    help="ELF used for the Phase-14 interactive debug-session "
                         "suite (G18..G24); built at -O0 (default %(default)s)")
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
    ap.add_argument("--only", type=int, action="append", metavar="N",
                    help="Run only the given Group-G test number(s); repeatable. "
                         "Speeds up single-case debug cycles.")
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
    if not os.path.isfile(args.g17_elf):
        print(f"hw-test: gdb_acceptance: missing G17 ELF {args.g17_elf}",
              file=sys.stderr)
        return 1
    if not os.path.isfile(args.dbg_elf):
        print(f"hw-test: gdb_acceptance: missing debug-session ELF "
              f"{args.dbg_elf}", file=sys.stderr)
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
        selected = sorted(n for n in VERDICTS if not args.only or n in args.only)
        for n in selected:
            if n == 9:
                desired_extra_args = ["--log-rsp", "--no-introspect"]
            elif n in (10, 11, 12, 13, 14, 15, 16, 17,
                       18, 19, 20, 21, 22, 23, 24, 25):
                desired_extra_args = ["--log-rsp", "--load"]
            else:
                desired_extra_args = ["--log-rsp"]
            if n in (18, 19, 20, 21, 22, 23, 24):
                desired_elf = args.dbg_elf
            elif n == 17:
                desired_elf = args.g17_elf
            elif n in (10, 11, 12, 13, 14, 15, 16, 25):
                desired_elf = args.g10_elf      # avrOS example (has ledsFlash)
            else:
                desired_elf = args.elf
            force_restart = (n in (11, 12, 13, 14, 15, 16, 17,
                                   18, 19, 20, 21, 22, 23, 24, 25))
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
        for n, tr in zip(selected, all_transcripts):
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
