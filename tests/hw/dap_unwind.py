#!/usr/bin/env python3
"""DAP multi-frame stackTrace acceptance harness (Phase 18, HLR-080).

The DAP analogue of the GDB G18 backtrace test.  Spawns a fresh ``avrOSdb
--dap`` server against live AVR-Dx silicon running the ``gdb_debug_session``
fixture (``main -> top -> mid -> leaf`` nested call chain), drives it with a
minimal DAP client over TCP, breaks inside ``leaf`` via an instruction
breakpoint, and verifies the DWARF-CFI unwinder reports the full call chain:
each frame's PC must fall inside the expected function and resolve to a source
line.  Manual-only (needs hardware); run via ``make hw-test-dap-unwind``.

The expected frame functions (leaf, mid, top, main) and the break address are
derived from the ELF with ``avr-nm`` / ``avr-objdump`` at runtime, so the test
survives a fixture recompile.  Exit code 0 on success, 1 on any failure.
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


def func_ranges(elf: str, nm: str, names: list[str]) -> dict[str, tuple[int, int]]:
    """Return {name: (start, end)} byte-address ranges from the symbol table.

    Uses ``nm -S`` so each function's true size bounds the range — local
    compiler labels (.Loc/L0^A) sit between functions and would otherwise
    truncate a next-symbol estimate.
    """
    out = {}
    for line in sh(nm, "-S", "-n", elf).splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in ("t", "T") and p[3] in names:
            start, size = int(p[0], 16), int(p[1], 16)
            out[p[3]] = (start, start + size)
    return out


def first_body_addr(elf: str, objdump: str, fn: str, start: int) -> int:
    """A post-prologue instruction address inside `fn` (skip the Y-setup)."""
    dis = sh(objdump, "-d", elf)
    body = re.search(rf"^[0]*{start:x} <{fn}>:\n(.*?)(?=\n\n|\Z)", dis,
                     re.S | re.M)
    addrs = [int(m, 16) for m in re.findall(r"^\s*([0-9a-f]+):", body.group(1),
                                            re.M)] if body else []
    # Skip the first ~6 prologue instructions (push/in/sbiw/out) to land in body.
    return addrs[6] if len(addrs) > 6 else (addrs[1] if len(addrs) > 1 else start)


class Dap:
    def __init__(self, sock: socket.socket):
        self.s, self.seq, self.buf = sock, 0, b""

    def send(self, command, args=None):
        self.seq += 1
        msg = {"seq": self.seq, "type": "request", "command": command}
        if args is not None:
            msg["arguments"] = args
        b = json.dumps(msg).encode()
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
    ap.add_argument("--objdump", default=os.getenv("AVR_OBJDUMP", "avr-objdump"))
    args = ap.parse_args()

    chain = ["leaf", "mid", "top", "main"]
    ranges = func_ranges(args.elf, args.nm, chain)
    missing = [f for f in chain if f not in ranges]
    if missing:
        print(f"dap-unwind: FATAL — symbols not found: {missing}", file=sys.stderr)
        return 2
    bp = first_body_addr(args.elf, args.objdump, "leaf", ranges["leaf"][0])

    srv = subprocess.Popen(
        [args.bin, "--dap", "--port", str(args.dap_port), args.port, args.elf],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    # wait for the listener
    up, deadline = False, time.time() + 6
    while time.time() < deadline:
        line = srv.stderr.readline()
        if "listening on" in line:
            up = True
            break
    if not up:
        srv.kill()
        print("dap-unwind: FATAL — server did not start", file=sys.stderr)
        return 2

    results = []

    def record(name, ok, detail=""):
        results.append(ok)
        tag = (GREEN + "PASS" + RESET) if ok else (RED + "FAIL" + RESET)
        print(f"dap-unwind: {name:<46} {tag}"
              + (("  " + DIM + detail + RESET) if (detail and not ok) else ""))

    try:
        sock = socket.create_connection(("127.0.0.1", args.dap_port), timeout=5)
        dap = Dap(sock)
        dap.send("initialize", {"adapterID": "unwind"})
        dap.wait(lambda m: m.get("command") == "initialize")
        dap.send("configurationDone")
        dap.wait(lambda m: m.get("event") == "stopped")

        dap.send("setInstructionBreakpoints",
                 {"breakpoints": [{"instructionReference": hex(bp)}]})
        r = dap.wait(lambda m: m.get("command") == "setInstructionBreakpoints")
        b0 = ((r or {}).get("body", {}).get("breakpoints") or [{}])[0]
        record(f"DAP-U1  instr bp @ leaf (0x{bp:x}) verified",
               bool(b0.get("verified")), json.dumps(b0))

        dap.send("continue", {"threadId": 1})
        hit = dap.wait(lambda m: m.get("event") == "stopped"
                       and (m.get("body") or {}).get("reason") == "breakpoint",
                       timeout=12)
        record("DAP-U2  continue -> stopped(breakpoint)", hit is not None,
               "no breakpoint stop")

        dap.send("stackTrace", {"threadId": 1})
        r = dap.wait(lambda m: m.get("command") == "stackTrace")
        frames = (r or {}).get("body", {}).get("stackFrames", [])

        record("DAP-U3  stackTrace returns >= 4 frames", len(frames) >= 4,
               f"got {len(frames)}")

        # Each of the first four frames must sit inside the expected function.
        def pc_of(f):
            return int(f.get("instructionPointerReference", "0x0"), 0)

        chain_ok, detail = True, []
        for i, fn in enumerate(chain):
            if i >= len(frames):
                chain_ok = False
                detail.append(f"#{i} missing")
                continue
            pc = pc_of(frames[i])
            lo, hi = ranges[fn]
            inside = lo <= pc < hi
            detail.append(f"#{i}=0x{pc:x}({'in' if inside else 'OUT'} {fn})")
            chain_ok = chain_ok and inside
        record("DAP-U4  frames map to leaf->mid->top->main", chain_ok,
               " ".join(detail))

        # Frames 0..3 should each resolve to a source line (> 0).
        lines_ok = all(isinstance(frames[i].get("line"), int)
                       and frames[i]["line"] > 0
                       for i in range(min(4, len(frames))))
        record("DAP-U5  each chain frame resolves a source line", lines_ok,
               " ".join(f"#{i}:{frames[i].get('line')}"
                        for i in range(min(4, len(frames)))))

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
    print(f"dap-unwind: {passed} passed, {len(results) - passed} failed")
    return 0 if passed == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
