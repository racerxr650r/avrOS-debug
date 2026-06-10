#!/usr/bin/env python3
"""On-target DAP source-line stepping acceptance (HLR-078).

Drives `avrOSdb --dap` against live AVR-Dx silicon running the
`gdb_debug_session` fixture (call chain main -> top -> mid -> leaf) and verifies
the three step requests behave with **source-line** granularity and distinct
semantics — the bug being that next/stepIn/stepOut all single-stepped one
instruction:

  ST1  stepIn advances a whole source line in ONE step (not one instruction)
  ST2  stepIn at a call line descends INTO the callee
  ST3  stepOut returns to the caller
  ST4  next steps OVER calls (never descends into leaf) and is line-granular

Manual-only (`make hw-test-dap-step`) — never wired into `make test`.
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time

GREEN, RED, DIM, RESET = "\033[32m", "\033[31m", "\033[2m", "\033[0m"


def func_addr(elf, nm, name):
    for line in subprocess.run([nm, "-n", elf], capture_output=True,
                               text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in ("t", "T") and p[2] == name:
            return int(p[0], 16)
    raise SystemExit(f"dap-step: FATAL — symbol {name!r} not found")


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
                chunk = self.s.recv(8192)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk

    def wait(self, pred, timeout=12):
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv(end - time.time())
            if m and pred(m):
                return m
        return None

    def rpc(self, command, args=None, timeout=12):
        self.send(command, args)
        return self.wait(lambda m: m.get("command") == command
                         and m.get("type") == "response", timeout)

    def where(self):
        st = self.rpc("stackTrace", {"threadId": 1})
        fr = ((st or {}).get("body", {}).get("stackFrames") or [{}])[0]
        return fr.get("name"), fr.get("line")

    def step(self, command):
        """Issue a step request, wait for the stopped event, return where()."""
        self.send(command, {"threadId": 1})
        self.wait(lambda m: m.get("event") == "stopped", timeout=30)
        return self.where()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.getenv("HW_PORT", "/dev/ttyAMA2"))
    ap.add_argument("--elf", default="build/fixtures/gdb_debug_session.elf")
    ap.add_argument("--dap-port", type=int, default=int(os.getenv("DAP_PORT", "1234")))
    ap.add_argument("--bin", default=os.getenv("AVROSDB_BIN", "build/avrOSdb"))
    ap.add_argument("--nm", default=os.getenv("HW_NM", "avr-nm"))
    args = ap.parse_args()

    top_a = func_addr(args.elf, args.nm, "top")
    mid_a = func_addr(args.elf, args.nm, "mid")

    srv = subprocess.Popen(
        [args.bin, "--dap", "--port", str(args.dap_port), args.port, args.elf],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    up, deadline = False, time.time() + 6
    while time.time() < deadline:
        if "listening on" in (srv.stderr.readline() or ""):
            up = True
            break
    if not up:
        srv.kill()
        print("dap-step: FATAL — server did not start", file=sys.stderr)
        return 2

    results = []

    def record(name, ok, detail=""):
        results.append(ok)
        tag = (GREEN + "PASS" + RESET) if ok else (RED + "FAIL" + RESET)
        print(f"dap-step: {name:<48} {tag}"
              + (("  " + DIM + str(detail) + RESET) if (detail and not ok) else ""))

    def hexref(a):
        return {"instructionReference": "0x%x" % a}

    try:
        sock = socket.create_connection(("127.0.0.1", args.dap_port), timeout=5)
        dap = Dap(sock)
        dap.rpc("initialize", {"adapterID": "step"})
        dap.send("configurationDone")
        dap.wait(lambda m: m.get("event") == "stopped")

        # ── Phase 1: break at top entry → stepIn (granularity + descend), stepOut.
        dap.rpc("setInstructionBreakpoints", {"breakpoints": [hexref(top_a)]})
        dap.send("continue", {"threadId": 1})
        dap.wait(lambda m: m.get("event") == "stopped"
                 and (m.get("body") or {}).get("reason") == "breakpoint")
        n0, l0 = dap.where()

        # ST1: one stepIn moves a whole source line within top (line changes,
        # still in top) — proving line- not instruction-granularity.
        n1, l1 = dap.step("stepIn")
        record("ST1  stepIn advances one source line (in top)",
               n1 == "top" and l1 is not None and l1 != l0, f"{n0}:{l0} -> {n1}:{l1}")

        # ST2: keep stepIn-ing; at the mid() call line it descends into mid.
        name, line, descended = n1, l1, False
        for _ in range(5):
            if name == "mid":
                descended = True
                break
            name, line = dap.step("stepIn")
        descended = descended or name == "mid"
        record("ST2  stepIn descends into the callee (mid)",
               descended, f"ended in {name}:{line}")

        # ST3: stepOut from mid returns to the caller top.
        n3, l3 = dap.step("stepOut")
        record("ST3  stepOut returns to the caller (top)",
               n3 == "top", f"{n3}:{l3}")

        # ── Phase 2: break at mid entry → next must step OVER the leaf() calls.
        dap.rpc("setInstructionBreakpoints", {"breakpoints": [hexref(mid_a)]})
        dap.send("continue", {"threadId": 1})
        dap.wait(lambda m: m.get("event") == "stopped"
                 and (m.get("body") or {}).get("reason") == "breakpoint")
        name, line = dap.where()

        seen_leaf = False
        lines_in_mid = [line] if name == "mid" else []
        for _ in range(8):
            name, line = dap.step("next")
            if name == "leaf":
                seen_leaf = True
                break
            if name == "mid":
                lines_in_mid.append(line)
            else:
                break          # returned out of mid (into top) — done walking
        # next stepped over both leaf() calls (never entered leaf) …
        record("ST4a next steps OVER calls (never enters leaf)", not seen_leaf,
               "entered leaf" if seen_leaf else "")
        # … and advanced with SOURCE-LINE granularity, not instruction: the
        # lines are monotonic and span several of mid's lines over a handful of
        # `next`s (instruction-granularity would crawl within one line).  A
        # call-line may legitimately appear twice (avr-gcc -O0 splits `acc +=
        # leaf(...)` into the call row then the post-call assign row).
        monotonic = all(b >= a for a, b in zip(lines_in_mid, lines_in_mid[1:]))
        span = (max(lines_in_mid) - min(lines_in_mid)) if lines_in_mid else 0
        record("ST4b next is line-granular within mid",
               monotonic and span >= 3 and len(set(lines_in_mid)) >= 4,
               str(lines_in_mid))

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
    print(f"dap-step: {passed} passed, {len(results) - passed} failed")
    return 0 if passed == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
