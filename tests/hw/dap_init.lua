-- tests/hw/dap_init.lua — minimal headless Neovim init for the DAP acceptance
-- harness. Puts nvim-dap on the runtimepath, cloning it into a local cache on
-- first run if it is not already present. Used as:
--
--   nvim --headless -u tests/hw/dap_init.lua -l tests/hw/dap_acceptance.lua
--
-- The cache (tests/hw/.nvim-dap) is gitignored. Requires `git` + network on
-- first run only.

local this   = debug.getinfo(1, 'S').source:sub(2)        -- strip leading '@'
local hwdir  = vim.fn.fnamemodify(this, ':h')
local depdir = hwdir .. '/.nvim-dap'

if vim.fn.isdirectory(depdir) == 0 then
  io.stderr:write('dap_init: cloning nvim-dap into ' .. depdir .. ' ...\n')
  vim.fn.system({ 'git', 'clone', '--depth=1',
                  'https://github.com/mfussenegger/nvim-dap', depdir })
  if vim.v.shell_error ~= 0 then
    io.stderr:write('dap_init: ERROR — failed to clone nvim-dap. Install it '
                 .. 'manually at ' .. depdir .. ' or on the runtimepath.\n')
  end
end

vim.opt.runtimepath:prepend(depdir)
