#!/usr/bin/env python3
"""DAP variables / evaluate / memory acceptance harness (Phase 19, HLR-082..084).

The DAP analogue of the GDB Group-G state-inspection tests (G17/G22/G23). Spawns
a fresh ``avrOSdb --dap`` server against live AVR-Dx silicon running the
``gdb_debug_session`` fixture, breaks inside ``leaf``, and verifies the
front-end's source-level state inspection against the fixture's documented
values:

    g_marker = 49374 (const)   g_cfg = {base=100, gain=-7}   g_arr = {10,20,30,40}
    leaf(7, 2): a=7  b=2

  DAP-V1  scopes -> Locals / Registers / Globals
  DAP-V2  Globals: g_marker scalar reads 49374
  DAP-V3  Globals: g_cfg expands to base=100, gain=-7  (struct members)
  DAP-V4  Globals: g_arr expands to {10,20,30,40}      (array elements)
  DAP-V5  Locals: a=7, b=2                              (frame params)
  DAP-V6  evaluate g_cfg.base=100, g_arr[2]=30, g_marker=49374
  DAP-V7  readMemory(g_arr, 8) -> 10 20 30 40 (LE u16)
  DAP-V8  setVariable g_counter=1234 -> reads back 1234

Manual-only (``make hw-test-dap-vars``); never wired into ``make test``.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import socket
import subprocess
import sys
import time

GREEN, RED, DIM, RESET = "\033[32m", "\033[31m", "\033[2m", "\033[0m"


def leaf_line(elf, nm, a2l):
    start = None
    for line in subprocess.run([nm, "-n", elf], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in ("t", "T") and p[2] == "leaf":
            start = int(p[0], 16)
    if start is None:
        raise SystemExit("dap-vars: FATAL — leaf symbol not found")
    out = subprocess.run([a2l, "-e", elf, hex(start + 0x1a)], capture_output=True, text=True).stdout
    m = re.search(r":(\d+)\s*$", out)
    return int(m.group(1)) if m else 72


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

    def wait(self, pred, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv(end - time.time())
            if m and pred(m):
                return m
        return None

    def rpc(self, command, args=None, timeout=10):
        self.send(command, args)
        return self.wait(lambda m: m.get("command") == command
                         and m.get("type") == "response", timeout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.getenv("HW_PORT", "/dev/ttyAMA2"))
    ap.add_argument("--elf", default="build/fixtures/gdb_debug_session.elf")
    ap.add_argument("--dap-port", type=int, default=int(os.getenv("DAP_PORT", "1234")))
    ap.add_argument("--bin", default=os.getenv("AVROSDB_BIN", "build/avrOSdb"))
    ap.add_argument("--nm", default=os.getenv("AVR_NM", "avr-nm"))
    ap.add_argument("--addr2line", default=os.getenv("AVR_A2L", "avr-addr2line"))
    args = ap.parse_args()

    line = leaf_line(args.elf, args.nm, args.addr2line)

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
        print("dap-vars: FATAL — server did not start", file=sys.stderr)
        return 2

    results = []

    def record(name, ok, detail=""):
        results.append(ok)
        tag = (GREEN + "PASS" + RESET) if ok else (RED + "FAIL" + RESET)
        print(f"dap-vars: {name:<46} {tag}"
              + (("  " + DIM + detail + RESET) if (detail and not ok) else ""))

    try:
        sock = socket.create_connection(("127.0.0.1", args.dap_port), timeout=5)
        dap = Dap(sock)
        dap.rpc("initialize", {"adapterID": "vars"})
        dap.send("configurationDone")
        dap.wait(lambda m: m.get("event") == "stopped")
        dap.rpc("setBreakpoints", {"source": {"path": "gdb_debug_session.c"},
                                   "breakpoints": [{"line": line}]})
        dap.send("continue", {"threadId": 1})
        dap.wait(lambda m: m.get("event") == "stopped"
                 and (m.get("body") or {}).get("reason") == "breakpoint", timeout=12)
        st = dap.rpc("stackTrace", {"threadId": 1})

        # The frame's source path must be absolute and openable, so VS Code
        # loads it directly instead of falling back to the DAP `source` request.
        fr0 = ((st or {}).get("body", {}).get("stackFrames") or [{}])[0]
        spath = (fr0.get("source") or {}).get("path")
        record("DAP-V9  frame source path absolute + on disk",
               bool(spath) and spath.startswith("/") and os.path.exists(spath),
               str(spath))
        # …and the `source` request still serves the content as a fallback.
        srcr = dap.rpc("source", {"source": {"path": spath}}) if spath else None
        record("DAP-V10 source request returns file content",
               bool(srcr) and srcr.get("success")
               and len(((srcr or {}).get("body") or {}).get("content", "")) > 0,
               str((srcr or {}).get("success")))

        sc = dap.rpc("scopes", {"frameId": 0})
        scopes = {x["name"]: x["variablesReference"]
                  for x in (sc or {}).get("body", {}).get("scopes", [])}
        record("DAP-V1  scopes -> Locals/Registers/Globals",
               {"Locals", "Registers", "Globals"} <= set(scopes), str(list(scopes)))

        def vars_of(ref):
            r = dap.rpc("variables", {"variablesReference": ref})
            return {v["name"]: v for v in (r or {}).get("body", {}).get("variables", [])}

        gl = vars_of(scopes.get("Globals", 0)) if "Globals" in scopes else {}
        record("DAP-V2  g_marker == 49374",
               gl.get("g_marker", {}).get("value") == "49374",
               gl.get("g_marker", {}).get("value"))

        cfg = vars_of(gl["g_cfg"]["variablesReference"]) if gl.get("g_cfg", {}).get("variablesReference") else {}
        record("DAP-V3  g_cfg = {base=100, gain=-7}",
               cfg.get("base", {}).get("value") == "100" and
               cfg.get("gain", {}).get("value") == "-7",
               f"base={cfg.get('base',{}).get('value')} gain={cfg.get('gain',{}).get('value')}")

        arr = vars_of(gl["g_arr"]["variablesReference"]) if gl.get("g_arr", {}).get("variablesReference") else {}
        got = [arr.get(f"[{i}]", {}).get("value") for i in range(4)]
        record("DAP-V4  g_arr = {10,20,30,40}", got == ["10", "20", "30", "40"], str(got))

        lo = vars_of(scopes.get("Locals", 0)) if "Locals" in scopes else {}
        record("DAP-V5  locals a=7, b=2",
               lo.get("a", {}).get("value") == "7" and lo.get("b", {}).get("value") == "2",
               f"a={lo.get('a',{}).get('value')} b={lo.get('b',{}).get('value')}")

        def ev(expr):
            r = dap.rpc("evaluate", {"expression": expr, "frameId": 0, "context": "watch"})
            return (r or {}).get("body", {}).get("result") if (r or {}).get("success") else None
        e_ok = ev("g_cfg.base") == "100" and ev("g_arr[2]") == "30" and ev("g_marker") == "49374"
        record("DAP-V6  evaluate g_cfg.base/g_arr[2]/g_marker", e_ok,
               f"base={ev('g_cfg.base')} [2]={ev('g_arr[2]')} marker={ev('g_marker')}")

        memref = gl.get("g_arr", {}).get("memoryReference")
        rm = dap.rpc("readMemory", {"memoryReference": memref, "count": 8}) if memref else None
        u16 = []
        if rm and rm.get("body", {}).get("data"):
            d = base64.b64decode(rm["body"]["data"])
            u16 = [d[i] | (d[i + 1] << 8) for i in range(0, min(8, len(d)), 2)]
        record("DAP-V7  readMemory(g_arr) -> 10,20,30,40", u16 == [10, 20, 30, 40], str(u16))

        sv = dap.rpc("setVariable", {"variablesReference": scopes.get("Globals", 0),
                                     "name": "g_counter", "value": "1234"})
        sv_ok = (sv or {}).get("success") and ev("g_counter") == "1234"
        record("DAP-V8  setVariable g_counter=1234 reads back", bool(sv_ok),
               f"resp={(sv or {}).get('success')} readback={ev('g_counter')}")

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
    print(f"dap-vars: {passed} passed, {len(results) - passed} failed")
    return 0 if passed == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
