#!/usr/bin/env python3
"""DAP conditional-breakpoint acceptance harness (Phase 18, HLR-081).

The DAP analogue of the GDB G21 conditional-breakpoint test.  Spawns a fresh
``avrOSdb --dap`` server against live AVR-Dx silicon running the
``gdb_debug_session`` fixture (whose ``main -> top -> mid -> leaf`` chain calls
``leaf(7, 2)`` then ``leaf(7, 3)`` on every loop), sets a *source* breakpoint in
``leaf`` carrying a DAP ``condition``, and verifies the adapter evaluates the
condition on the live target — stepping over and auto-resuming when it is false,
and stopping only when it is true:

  DAP-K1  `b == 99`  (always false; b is only ever 2 or 3) -> target keeps
          running, no breakpoint stop  (proves local read + false -> resume)
  DAP-K2  `b == 2`   (true on leaf(7,2)) -> stopped(breakpoint)
  DAP-K3  `g_marker == 49374` (a global constant, always true) ->
          stopped(breakpoint)  (proves global-variable conditions)

The leaf source line is derived from the ELF with ``avr-addr2line`` at runtime.
Manual-only (``make hw-test-dap-cond``); never wired into ``make test``.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time

GREEN, RED, DIM, RESET = "\033[32m", "\033[31m", "\033[2m", "\033[0m"


def sh(*cmd: str) -> str:
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def leaf_line(elf: str, nm: str, a2l: str) -> tuple[str, int]:
    """A (basename, line) inside leaf(), past its prologue, from the ELF."""
    start = None
    for line in sh(nm, "-n", elf).splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in ("t", "T") and p[2] == "leaf":
            start = int(p[0], 16)
    if start is None:
        raise SystemExit("dap-cond: FATAL — leaf symbol not found")
    out = sh(a2l, "-e", elf, hex(start + 0x1a)).strip()
    m = re.search(r"([^/\\]+):(\d+)\s*$", out)
    if not m:
        raise SystemExit(f"dap-cond: FATAL — cannot map leaf body: {out}")
    return m.group(1), int(m.group(2))


class Dap:
    def __init__(self, sock):
        self.s, self.seq, self.buf = sock, 0, b""

    def send(self, command, args=None):
        self.seq += 1
        m = {"seq": self.seq, "type": "request", "command": command}
        if args is not None:
            m["arguments"] = args
        b = json.dumps(m).encode()
        self.s.sendall(b"Content-Length: %d\r\n\r\n%s" % (len(b), b))

    def recv(self, timeout):
        self.s.settimeout(max(0.05, timeout))
        while True:
            if b"\r\n\r\n" in self.buf:
                head, rest = self.buf.split(b"\r\n\r\n", 1)
                clen = next((int(l.split(b":")[1]) for l in head.split(b"\r\n")
                             if l.lower().startswith(b"content-length:")), 0)
                if len(rest) >= clen:
                    self.buf = rest[clen:]
                    return json.loads(rest[:clen])
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk

    def wait(self, pred, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv(end - time.time())
            if m and pred(m):
                return m
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.getenv("HW_PORT", "/dev/ttyAMA2"))
    ap.add_argument("--elf", default="build/fixtures/gdb_debug_session.elf")
    ap.add_argument("--dap-port", type=int,
                    default=int(os.getenv("DAP_PORT", "1234")))
    ap.add_argument("--bin", default=os.getenv("AVROSDB_BIN", "build/avrOSdb"))
    ap.add_argument("--nm", default=os.getenv("AVR_NM", "avr-nm"))
    ap.add_argument("--addr2line", default=os.getenv("AVR_A2L", "avr-addr2line"))
    args = ap.parse_args()

    src, line = leaf_line(args.elf, args.nm, args.addr2line)

    srv = subprocess.Popen(
        [args.bin, "--dap", "--port", str(args.dap_port), args.port, args.elf],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    up, deadline = False, time.time() + 6
    while time.time() < deadline:
        if "listening on" in srv.stderr.readline():
            up = True
            break
    if not up:
        srv.kill()
        print("dap-cond: FATAL — server did not start", file=sys.stderr)
        return 2

    results = []

    def record(name, ok, detail=""):
        results.append(ok)
        tag = (GREEN + "PASS" + RESET) if ok else (RED + "FAIL" + RESET)
        print(f"dap-cond: {name:<46} {tag}"
              + (("  " + DIM + detail + RESET) if (detail and not ok) else ""))

    def set_cond_bp(dap, cond):
        dap.send("setBreakpoints",
                 {"source": {"path": src},
                  "breakpoints": [{"line": line, "condition": cond}]})
        r = dap.wait(lambda m: m.get("command") == "setBreakpoints")
        b = ((r or {}).get("body", {}).get("breakpoints") or [{}])[0]
        return bool(b.get("verified"))

    try:
        sock = socket.create_connection(("127.0.0.1", args.dap_port), timeout=5)
        dap = Dap(sock)
        dap.send("initialize", {"adapterID": "cond"})
        dap.wait(lambda m: m.get("command") == "initialize")
        dap.send("configurationDone")
        dap.wait(lambda m: m.get("event") == "stopped")

        # DAP-K1: always-false condition -> never stops at leaf.
        ok_v = set_cond_bp(dap, "b == 99")
        dap.send("continue", {"threadId": 1})
        hit = dap.wait(lambda m: m.get("event") == "stopped"
                       and (m.get("body") or {}).get("reason") == "breakpoint",
                       timeout=4)
        record(f"DAP-K1  `b == 99` (false) skips leaf @ {src}:{line}",
               ok_v and hit is None,
               "breakpoint not verified" if not ok_v else "stopped despite false cond")
        # re-halt for the next case
        dap.send("pause", {"threadId": 1})
        dap.wait(lambda m: m.get("event") == "stopped", timeout=5)

        # DAP-K2: true local condition -> stops.
        ok_v = set_cond_bp(dap, "b == 2")
        dap.send("continue", {"threadId": 1})
        hit = dap.wait(lambda m: m.get("event") == "stopped"
                       and (m.get("body") or {}).get("reason") == "breakpoint",
                       timeout=8)
        record("DAP-K2  `b == 2` (true) stops at leaf",
               ok_v and hit is not None, "no breakpoint stop")

        # DAP-K3: true global condition -> stops.
        ok_v = set_cond_bp(dap, "g_marker == 49374")
        dap.send("continue", {"threadId": 1})
        hit = dap.wait(lambda m: m.get("event") == "stopped"
                       and (m.get("body") or {}).get("reason") == "breakpoint",
                       timeout=8)
        record("DAP-K3  `g_marker == 49374` (global, true) stops",
               ok_v and hit is not None, "no breakpoint stop")

        dap.send("disconnect", {})
        time.sleep(0.3)
        sock.close()
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=3)
        except subprocess.TimeoutExpired:
            srv.kill()

    passed = sum(1 for r in results if r)
    print(f"dap-cond: {passed} passed, {len(results) - passed} failed")
    return 0 if passed == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
