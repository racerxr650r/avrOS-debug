-- >>> avrOSdb DAP config (managed by `make prereqs`) >>>
-- Neovim debug setup for avrOSdb's native DAP front-end (`avrOSdb --dap`).
--
-- nvim-dap is installed as a native package at
--   ~/.local/share/nvim/site/pack/dap/start/nvim-dap
-- (cloned by `make prereqs`), so it loads automatically — no plugin manager.
--
-- Workflow:
--   1. On the target host, start the DAP server:
--        build/avrOSdb --dap --port 1234 /dev/ttyAMA2 firmware.elf
--   2. Open a source file and press <F5> (or :DapAvrOSdb) to attach. The
--      server stops the target at entry; use the step keys below.
--
-- NOTE: the avrOSdb DAP front-end is built up across phases. Phase 16
-- implements the connection handshake (attach -> stopped at entry, threads,
-- disconnect); execution control, breakpoints, and variable inspection land in
-- later phases. The keymaps are wired now so they work as each feature arrives.
--
-- This block is self-contained and append-safe: it only runs when nvim-dap is
-- present and uses no top-level `return`, so it can be appended to an existing
-- init.lua without affecting the rest of the file.
local _avrosdb_ok, _avrosdb_dap = pcall(require, "dap")
if _avrosdb_ok then
  local dap = _avrosdb_dap

  -- Adapter: connect to a running `avrOSdb --dap` TCP server. Override per
  -- session with vim.g.avrosdb_dap_host / vim.g.avrosdb_dap_port.
  dap.adapters.avrosdb = function(callback)
    callback({
      type = "server",
      host = vim.g.avrosdb_dap_host or "127.0.0.1",
      port = vim.g.avrosdb_dap_port or 1234,
      options = { max_retries = 30, initialize_timeout_sec = 5 },
    })
  end

  -- Configuration: attach to the already-running target. The firmware ELF is
  -- the one passed to `avrOSdb --dap` on the server side; nvim-dap uses
  -- `program` to resolve source paths for the editor.
  local avrosdb_attach = {
    type    = "avrosdb",
    request = "attach",
    name    = "Attach to avrOSdb (--dap)",
    program = function()
      return vim.fn.input("Firmware ELF: ", vim.fn.getcwd() .. "/", "file")
    end,
  }
  dap.configurations.c   = { avrosdb_attach }
  dap.configurations.cpp = { avrosdb_attach }

  -- Keymaps (mirror common debugger bindings).
  local map = vim.keymap.set
  map("n", "<F5>",  function() dap.continue() end,          { desc = "DAP continue / attach" })
  map("n", "<F10>", function() dap.step_over() end,         { desc = "DAP step over" })
  map("n", "<F11>", function() dap.step_into() end,         { desc = "DAP step into" })
  map("n", "<F12>", function() dap.step_out() end,          { desc = "DAP step out" })
  map("n", "<F9>",  function() dap.toggle_breakpoint() end, { desc = "DAP toggle breakpoint" })
  map("n", "<F6>",  function() dap.terminate() end,         { desc = "DAP terminate" })
  map("n", "<leader>dr", function() dap.repl.toggle() end,  { desc = "DAP REPL" })
  map("n", "<leader>dt", function() dap.terminate() end,    { desc = "DAP terminate" })

  -- :DapAvrOSdb [host] [port] — attach without a keymap.
  vim.api.nvim_create_user_command("DapAvrOSdb", function(o)
    if o.fargs[1] then vim.g.avrosdb_dap_host = o.fargs[1] end
    if o.fargs[2] then vim.g.avrosdb_dap_port = tonumber(o.fargs[2]) end
    dap.run(avrosdb_attach)
  end, { nargs = "*", desc = "Attach to avrOSdb --dap [host] [port]" })
else
  vim.notify("nvim-dap not found — run `make prereqs` (or clone nvim-dap into "
    .. "~/.local/share/nvim/site/pack/dap/start/nvim-dap)", vim.log.levels.WARN)
end
-- <<< avrOSdb DAP config <<<
