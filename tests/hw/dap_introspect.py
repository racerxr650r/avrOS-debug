#!/usr/bin/env python3
"""DAP avrOS-introspection acceptance harness (Phase 21, HLR-087).

Spawns a fresh ``avrOSdb --dap`` server against live AVR-Dx silicon and drives
the avrOS introspection custom requests (``avrosdb/fsmList``,
``avrosdb/eventList``, ``avrosdb/queueList``) that feed the VS Code avrOS view,
confirming they read the avrOS descriptor tables off the target and return
well-formed JSON.

Run against the ``avros_full`` fixture, whose FSM/event/queue table sections
hold one zero-filled descriptor each — enough to exercise the count>0 reader
path on silicon deterministically:

  DAP-I1  avrosdb/fsmList  -> well-formed `fsms` array (the null FSM slot is
          skipped, so 0 entries — graceful)
  DAP-I2  avrosdb/eventList -> 1 event, name "<null>", null status
  DAP-I3  avrosdb/queueList -> 1 queue, capacity 0

Point HW_DAP_ELF at a real avrOS application to list its live objects (the data
matches `monitor avros tasks/events/queues` on the GDB-RSP path). Manual-only
(``make hw-test-dap-introspect``); never wired into ``make test``.
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
    ap.add_argument("--elf", default="build/fixtures/avros_full.elf")
    ap.add_argument("--dap-port", type=int, default=int(os.getenv("DAP_PORT", "1234")))
    ap.add_argument("--bin", default=os.getenv("AVROSDB_BIN", "build/avrOSdb"))
    args = ap.parse_args()

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
        print("dap-introspect: FATAL — server did not start", file=sys.stderr)
        return 2

    results = []

    def record(name, ok, detail=""):
        results.append(ok)
        tag = (GREEN + "PASS" + RESET) if ok else (RED + "FAIL" + RESET)
        print(f"dap-introspect: {name:<42} {tag}"
              + (("  " + DIM + detail + RESET) if (detail and not ok) else ""))

    try:
        sock = socket.create_connection(("127.0.0.1", args.dap_port), timeout=5)
        dap = Dap(sock)
        dap.rpc("initialize", {"adapterID": "introspect"})
        dap.send("configurationDone")
        dap.wait(lambda m: m.get("event") == "stopped")

        def lst(cmd, key):
            r = dap.rpc(cmd)
            body = (r or {}).get("body", {})
            return body.get(key) if (r and r.get("success")) else None

        fsms = lst("avrosdb/fsmList", "fsms")
        record("DAP-I1  avrosdb/fsmList well-formed array",
               isinstance(fsms, list), repr(fsms))

        events = lst("avrosdb/eventList", "events")
        ev_ok = (isinstance(events, list) and len(events) == 1
                 and events[0].get("name") == "<null>"
                 and events[0].get("status") is None)
        record("DAP-I2  avrosdb/eventList reads the table", ev_ok, repr(events))

        queues = lst("avrosdb/queueList", "queues")
        q_ok = (isinstance(queues, list) and len(queues) == 1
                and queues[0].get("capacity") == 0)
        record("DAP-I3  avrosdb/queueList reads the table", q_ok, repr(queues))

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
    print(f"dap-introspect: {passed} passed, {len(results) - passed} failed")
    return 0 if passed == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
