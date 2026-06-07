-- tests/hw/dap_acceptance.lua — on-target DAP acceptance harness.
--
-- The DAP analogue of tests/hw/gdb_acceptance.py: it spawns a fresh `avrOSdb
-- --dap` server against live AVR-Dx silicon, drives it with the real
-- nvim-dap client over TCP, and applies per-case verdicts to the connection
-- lifecycle. Manual-only (needs hardware); run via `make hw-test-dap`.
--
-- Phase 16 covers the connection handshake (the DAP "Group-D" equivalent):
--   DAP1  initialize handshake completes + `initialized` event
--   DAP2  configurationDone reaches a stopped/`entry` state
--   DAP3  `threads` reports the single live CPU thread
--   DAP4  `disconnect` tears the session down cleanly
-- Execution control, breakpoints, stack/variables (Phases 17–19) add cases
-- here the same way Group-G grew for GDB.
--
-- Config comes from the environment (set by the Makefile):
--   AVROSDB_BIN  path to the built avrOSdb binary   (default build/avrOSdb)
--   HW_PORT      target serial device               (default /dev/ttyAMA2)
--   DAP_PORT     TCP port for the DAP server        (default 1234)
--   DAP_ELF      ELF passed to avrOSdb              (required)

local uv = vim.loop

-- ── tiny test framework (mirrors the gdb harness PASS/FAIL output) ──────────
local GREEN, RED, DIM, RESET = '\27[32m', '\27[31m', '\27[2m', '\27[0m'
local results = {}

local function record(name, ok, detail)
  results[#results + 1] = { name = name, ok = ok, detail = detail or '' }
  local tag = ok and (GREEN .. 'PASS' .. RESET) or (RED .. 'FAIL' .. RESET)
  io.write(string.format('dap-test: %-44s %s', name, tag))
  if not ok and detail ~= '' then io.write('  ' .. DIM .. detail .. RESET) end
  io.write('\n')
  io.flush()
end

local function die(msg)
  io.stderr:write('dap-test: FATAL — ' .. msg .. '\n')
  os.exit(2)
end

-- ── config ──────────────────────────────────────────────────────────────────
local BIN    = os.getenv('AVROSDB_BIN') or 'build/avrOSdb'
local SERIAL = os.getenv('HW_PORT')     or '/dev/ttyAMA2'
local PORT   = tonumber(os.getenv('DAP_PORT') or '1234')
local ELF    = os.getenv('DAP_ELF')
if not ELF or ELF == '' then die('DAP_ELF must name an ELF for avrOSdb') end

-- ── spawn the avrOSdb --dap server, wait for its listener ───────────────────
local listening = false
local srv_log = {}
local job = vim.fn.jobstart(
  { BIN, '--dap', '--port', tostring(PORT), SERIAL, ELF },
  {
    on_stderr = function(_, data)
      for _, l in ipairs(data) do
        if l ~= '' then
          srv_log[#srv_log + 1] = l
          if l:find('listening on') then listening = true end
        end
      end
    end,
  })
if job <= 0 then die('could not start avrOSdb (' .. BIN .. ')') end

local function teardown()
  pcall(function() vim.fn.jobstop(job) end)
end

if not vim.wait(6000, function() return listening end, 50) then
  teardown()
  die('avrOSdb did not report "listening on" within 6 s\n  '
      .. table.concat(srv_log, '\n  '))
end

-- ── drive the session with nvim-dap ─────────────────────────────────────────
local ok_dap, dap = pcall(require, 'dap')
if not ok_dap then teardown(); die('nvim-dap not available on the runtimepath') end

dap.adapters.avrosdb = { type = 'server', host = '127.0.0.1', port = PORT }
local config = {
  type = 'avrosdb', request = 'attach', name = 'avrOSdb DAP acceptance',
}

local ev = { initialized = false, stopped = false, reason = nil, terminated = false }
dap.listeners.after.event_initialized['acc'] = function() ev.initialized = true end
dap.listeners.after.event_stopped['acc'] =
  function(_, body) ev.stopped = true; ev.reason = body and body.reason end
dap.listeners.after.event_terminated['acc'] = function() ev.terminated = true end

local ok_run = pcall(dap.run, config)
if not ok_run then teardown(); die('dap.run() failed') end

-- DAP1: initialize handshake (the session becomes active + emits `initialized`)
vim.wait(5000, function() return ev.initialized end, 50)
record('DAP1  initialize handshake', ev.initialized,
       ev.initialized and '' or 'no `initialized` event')

-- DAP2: configurationDone -> stopped/entry
vim.wait(8000, function() return ev.stopped end, 50)
record('DAP2  configurationDone -> stopped(entry)',
       ev.stopped and ev.reason == 'entry',
       (not ev.stopped) and 'no `stopped` event'
         or ('reason=' .. tostring(ev.reason)))

-- DAP3: threads -> one live CPU thread
local threads_done, threads = false, nil
local session = dap.session()
if session then
  session:request('threads', nil, function(err, resp)
    threads_done = true
    if not err and resp then threads = resp.threads end
  end)
  vim.wait(4000, function() return threads_done end, 50)
end
local one_thread = threads ~= nil and #threads == 1
record('DAP3  threads -> single CPU thread', one_thread,
       threads == nil and 'no threads response'
         or ('#threads=' .. tostring(#threads)))

-- DAP4: disconnect tears the session down cleanly
local disc_done, disc_ok = false, false
if session then
  session:request('disconnect', { restart = false }, function(err)
    disc_done = true; disc_ok = (err == nil)
  end)
  vim.wait(4000, function() return disc_done end, 50)
end
record('DAP4  disconnect (clean teardown)', disc_done and disc_ok,
       (not disc_done) and 'no disconnect response' or '')

-- ── summary + exit status ───────────────────────────────────────────────────
teardown()
local passed, failed = 0, 0
for _, r in ipairs(results) do
  if r.ok then passed = passed + 1 else failed = failed + 1 end
end
io.write(string.format('dap-test: %d passed, %d failed\n', passed, failed))
io.flush()
os.exit(failed == 0 and 0 or 1)
