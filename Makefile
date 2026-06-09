# Makefile — avrOS-debug (aod)
#
# Targets:
#   all          Build the avrOSdb host binary
#   test         Build ELF fixtures, build all test binaries, run them
#   test-ci      Like test, but writes per-suite output to build/test-results/*.txt
#                (used by CI to build a structured test summary)
#   coverage     Rebuild with --coverage, run all unit tests, and emit a
#                line + branch coverage report under build/coverage/
#                (txt, Cobertura XML, JSON summary, drill-down HTML).
#                Requires `gcovr` (pip install gcovr).
#   hw-test      Run on-target hardware integration tests (Groups A + B, safe)
#                Manual only — never wired into `make test`.
#                Override port: make hw-test HW_PORT=/dev/ttyUSB0
#                                  (PORT= accepted as alias)
#                See `HW_*` variables below for all knobs.
#   hw-test-nvm  Add destructive NVM page write/verify; requires
#                HW_TEST_NVM_CONFIRM=YES
#   hw-test-rsp  Spawn the RSP server and probe it over TCP; requires
#                HW_TEST_ELF=path/to/fw.elf
#   hw-test-gdb  Full-stack acceptance: spawn avrOSdb, drive avr-gdb
#                through load/break/watch/monitor/detach. Destructive
#                (reflashes the target); requires HW_TEST_NVM_CONFIRM=YES
#                and a working `avr-gdb` on PATH.
#   hw-test-all  Run all hw-test groups (needs both opt-ins above)
#   hw-test-dap  Drive the DAP server with headless Neovim + nvim-dap over TCP
#                (attach handshake; non-destructive; needs `nvim`).
#   clean        Remove all build artefacts
#   check-tools  Verify all required host tools are present on PATH
#   install      Install binary + man page under $(PREFIX) [default: /usr/local]
#   uninstall    Remove files placed by `install` (idempotent)
#   bundle       Build dist/ packages: .deb, .rpm, Homebrew formula
#   prereqs      Install all dev prerequisites via apt and download the AVR-Dx DFP
#                (Debian/Ubuntu only; requires sudo). Also runs prereqs-nvim.
#   prereqs-nvim Set up Neovim for DAP debugging: install nvim-dap + the avrOSdb
#                DAP config into ~/.config/nvim/init.lua (idempotent; no sudo)
#   help         Print this target list
#
# Variables:
#   ASAN=1     Add -fsanitize=address,undefined to both host and test builds
#   COVERAGE=1 Add gcov instrumentation (--coverage -O0) to host and test builds
#              (normally set automatically by the `coverage` target)
#   PREFIX     Install prefix (default: /usr/local)
#   VERSION    Package version string (default: `git describe` or 0.0.0)
#   V=1        Verbose build (show full commands)
#
# hw-test variables (all optional; environment or `make VAR=value`):
#   HW_PORT              serial device (default /dev/ttyAMA2)
#                        PORT= is accepted as a shorthand alias.
#   HW_DEVICE_ID         expected SIGROW DEVICEID, e.g. "1E 97 0A"
#                        (enables Group-A A3 deviceid check)
#   HW_SRAM_ADDR         scratch SRAM byte address for B-group round-trips
#                        (default 0x7000 — mid-RAM on AVR128DA, clear of
#                        .data/.bss and stack; pick another address if the
#                        running firmware uses this region — B1c will warn)
#   HW_FLASH_PAGE_ADDR   scratch FLASH page for NVM test (default 0x7E00,
#                        the last page of AVR128DA28; override for larger
#                        parts, e.g. 0x1FE00 for AVR128DA48/DB)
#   HW_ADDR_24BIT        address >0xFFFF to exercise ST_PTR_LONG; 0 = skip
#   HW_TEST_ELF          ELF for hw-test-rsp child process
#   HW_RSP_PORT          TCP port for RSP smoke test (default 1234)
#   HW_VERBOSE           0/1/2 — per-step trace + hex dumps (default 0)
#   HW_TEST_NVM_CONFIRM  must be YES to allow hw-test-nvm (destructive)

# ── Toolchain ────────────────────────────────────────────────────────────────
CC        := gcc
AVR_CC    := avr-gcc
AVR_STRIP := avr-strip
PREFIX    ?= /usr/local
BINDIR    := $(PREFIX)/bin
MANDIR    := $(PREFIX)/share/man/man1
MANPAGE   := doc/avrOSdb.1

# Version string: command-line override > VERSION file > git describe > 0.0.0
VERSION   ?= $(strip $(or $(shell cat VERSION 2>/dev/null),\
                          $(shell git describe --tags --always 2>/dev/null),\
                          0.0.0))
DISTDIR   := dist

# ── Platform detection ────────────────────────────────────────────────────────
OS := $(shell uname -s)

# openpty() lives in libutil on Linux; on macOS it is part of libc
ifeq ($(OS),Linux)
  LUTIL := -lutil
else
  LUTIL :=
endif

# ── elfutils (libelf + libdw) — required ──────────────────────────────────────
# src/elf_parser.c is a thin adapter over elfutils: libelf parses the ELF and
# libdw the DWARF debug info, so format changes are absorbed by the library.
# These are hard dependencies; `make prereqs` installs libdw-dev/libelf-dev.
ELFUTILS_LIBS := -ldw -lelf

# ── Sanitizer flag ────────────────────────────────────────────────────────────
ifeq ($(ASAN),1)
  SAN_FLAGS := -fsanitize=address,undefined
else
  SAN_FLAGS :=
endif

# ── Coverage flag ────────────────────────────────────────────────────────────
# COVERAGE=1 instruments host + test builds with gcov line/branch counters.
# -O0 keeps the gcov line/branch map accurate; we override the optimisation
# level only inside the coverage step so normal builds stay at -O2.
ifeq ($(COVERAGE),1)
  COV_CFLAGS  := --coverage -O0 -fprofile-arcs -ftest-coverage -fno-inline -fno-inline-small-functions -fno-default-inline
  COV_LDFLAGS := --coverage
else
  COV_CFLAGS  :=
  COV_LDFLAGS :=
endif

# ── Verbose flag ─────────────────────────────────────────────────────────────
ifeq ($(V),1)
  Q :=
else
  Q :=
endif

# ── Common flags ─────────────────────────────────────────────────────────────
CFLAGS := -std=c99 -D_POSIX_C_SOURCE=200809L \
           -Wall -Wextra -Wpedantic \
           -Wstrict-prototypes -Wmissing-prototypes \
           -Wshadow \
           -O2 -g \
           -DAVROSDB_VERSION='"$(VERSION)"' \
           $(SAN_FLAGS) $(COV_CFLAGS)

# Test builds: suppress warnings on __wrap_* stubs (no header declares them),
# and pass -DUNIT_TEST so src/main.c can exclude its main() entry point.
# -Wno-missing-field-initializers: test fixtures positionally partial-init
# larger structs (e.g. dap_session) on purpose and reset the rest at runtime.
TEST_CFLAGS := $(CFLAGS) -DUNIT_TEST \
               -Wno-missing-prototypes -Wno-strict-prototypes \
               -Wno-missing-field-initializers

# ── Directories ───────────────────────────────────────────────────────────────
SRCDIR      := src
TESTDIR     := tests
FIXTUREDIR  := tests/fixtures
UNITYDIR    := tests/unity
BUILDDIR    := build
TESTBINDIR  := $(BUILDDIR)/tests
FIXBINDIR   := $(BUILDDIR)/fixtures

# ── Main binary ───────────────────────────────────────────────────────────────
TARGET   := avrOSdb
SRCS     := $(SRCDIR)/main.c \
             $(SRCDIR)/updi.c \
             $(SRCDIR)/elf_parser.c \
             $(SRCDIR)/fsm_mapper.c \
             $(SRCDIR)/monitor.c \
             $(SRCDIR)/debug_bp.c \
             $(SRCDIR)/gdb_rsp.c \
             $(SRCDIR)/dap.c
OBJS     := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

# ── Unity ──────────────────────────────────────────────────────────────────────
UNITY_SRC  := $(UNITYDIR)/unity.c
UNITY_OBJ  := $(BUILDDIR)/unity.o
UNITY_INC  := -I$(UNITYDIR)

# ── AVR ELF fixtures ──────────────────────────────────────────────────────────
# These are compiled with avr-gcc to produce genuine AVR ELF32 files.
# AVR-Dx devices require the Microchip Device Family Pack (DFP).
# Run 'make prereqs' once to install the DFP.  When the DFP directory
# does not exist avr-gcc 14+ has native AVR-Dx support and no -B is needed.
AVR_MCU    := avr128da28
DFP_VER    := 2.4.286
DFP_PACK   := Atmel.AVR-Dx_DFP.$(DFP_VER).atpack
DFP_URL    := http://packs.download.atmel.com/$(DFP_PACK)
DFP        := /usr/lib/gcc/avr/5.4.0/Atmel.AVR-Dx_DFP.$(DFP_VER)
DFP_FLAGS  := $(if $(wildcard $(DFP)/gcc/dev/$(AVR_MCU)),\
                   -B $(DFP)/gcc/dev/$(AVR_MCU) -I$(DFP)/include,)
AVR_CFLAGS := -mmcu=$(AVR_MCU) $(DFP_FLAGS) -Os -g
FIXTURE_SRCS   := $(FIXTUREDIR)/avros_full.c \
                  $(FIXTUREDIR)/avros_partial.c \
                  $(FIXTUREDIR)/avros_break.c \
                  $(FIXTUREDIR)/all_nvm.c \
                  $(FIXTUREDIR)/gdb_target.c
FIXTURE_ELFS   := $(patsubst $(FIXTUREDIR)/%.c,$(FIXBINDIR)/%.elf,$(FIXTURE_SRCS))

# LLR-MAIN-13 hardware fixture — built with -mmcu=avr64dd32 so the ELF
# carries an `.note.gnu.avr.deviceinfo` string of "avr64dd32" (family
# AVR-DD) while the bench board is AVR128DA28.  Used to exercise the
# ELF-vs-silicon family mismatch abort on real hardware.  Built into
# its own variable so the per-MCU override does not leak into the
# pattern rule used by every other AVR fixture.
WRONG_FAMILY_MCU := avr64dd32
WRONG_FAMILY_DFP := $(if $(wildcard $(DFP)/gcc/dev/$(WRONG_FAMILY_MCU)),\
                         -B $(DFP)/gcc/dev/$(WRONG_FAMILY_MCU) -I$(DFP)/include,)
WRONG_FAMILY_SRC := $(FIXTUREDIR)/wrong_family.c
WRONG_FAMILY_ELF := $(FIXBINDIR)/wrong_family.elf
# not_avr.elf is a plain i386 ELF — built with the host gcc targeting ELF
NOT_AVR_SRC    := $(FIXTUREDIR)/not_avr.c
NOT_AVR_ELF    := $(FIXBINDIR)/not_avr.elf

# G17 local-variable fixture — built at -O0 (NOT the generic -Os pattern
# rule) so every local gets a stable stack slot with trivial frame-base
# DWARF.  This makes the values GDB reports for locals_probe()'s variables
# deterministic, so the gdb_acceptance G17 test validates avrOSdb's
# SP/unwind/SRAM read path rather than the optimiser's eliding of locals.
GDB_LOCALS_SRC := $(FIXTUREDIR)/gdb_locals.c
GDB_LOCALS_ELF := $(FIXBINDIR)/gdb_locals.elf

# Phase 14 interactive debug-session fixture (Group-G G18..G24, HLR-070) —
# also built at -O0 (NOT the generic -Os pattern rule) so the explicit
# main->top->mid->leaf call chain keeps deterministic frames, per-frame
# locals/params, and trivial frame-base DWARF.  The acceptance harness
# drives a full interactive session against this fixture and checks exact
# first-hit values, so the optimiser must not elide or reorder anything.
GDB_DBG_SESSION_SRC := $(FIXTUREDIR)/gdb_debug_session.c
GDB_DBG_SESSION_ELF := $(FIXBINDIR)/gdb_debug_session.elf

# ── Per-test configuration ─────────────────────────────────────────────────────
# Each entry: TEST_SRCS_<name>, TEST_WRAP_<name>, TEST_EXTRA_LDFLAGS_<name>
#
# test_elf  (no malloc --wrap: libelf/libdw own their allocations)
TEST_SRCS_test_elf  := $(TESTDIR)/test_elf.c $(SRCDIR)/elf_parser.c
TEST_WRAP_test_elf  :=
TEST_EXTRA_LDFLAGS_test_elf :=

# test_dap  (DAP transport: JSON codec + Content-Length framing)
TEST_SRCS_test_dap  := $(TESTDIR)/test_dap.c $(SRCDIR)/dap.c $(SRCDIR)/debug_bp.c $(TESTDIR)/ocd_stubs.c
TEST_WRAP_test_dap  :=
TEST_EXTRA_LDFLAGS_test_dap :=

# test_updi
TEST_SRCS_test_updi  := $(TESTDIR)/test_updi.c $(SRCDIR)/updi.c
TEST_WRAP_test_updi  := select
TEST_EXTRA_LDFLAGS_test_updi := $(LUTIL) -lpthread

# test_fsm
TEST_SRCS_test_fsm  := $(TESTDIR)/test_fsm.c $(SRCDIR)/fsm_mapper.c
TEST_WRAP_test_fsm  := updi_mem_read
TEST_EXTRA_LDFLAGS_test_fsm :=

# test_monitor
TEST_SRCS_test_monitor := $(TESTDIR)/test_monitor.c \
                           $(SRCDIR)/monitor.c \
                           $(SRCDIR)/elf_parser.c
TEST_WRAP_test_monitor  := updi_mem_read rsp_send_packet \
                            updi_enter_debug updi_halt updi_run \
                            updi_chip_erase \
							fsm_invalidate fsm_get_active_thread \
							fsm_build_thread_list \
                            rsp_hw_bp_clear_all \
                            rsp_sw_bp_clear_all
TEST_EXTRA_LDFLAGS_test_monitor :=

# test_rsp
TEST_SRCS_test_rsp := $(TESTDIR)/test_rsp.c \
                       $(SRCDIR)/gdb_rsp.c \
                       $(SRCDIR)/debug_bp.c \
                       $(SRCDIR)/fsm_mapper.c \
                       $(SRCDIR)/monitor.c
TEST_WRAP_test_rsp  := updi_mem_read updi_mem_write updi_halt updi_run updi_step \
					   updi_step_32bit \
                       updi_nvm_write_flash updi_nvm_flash_patch \
                       updi_console_poll \
                       updi_enter_debug updi_chip_erase \
                       updi_ocd_poll_halted updi_ocd_read_halt_status \
                       updi_ocd_read_gpr updi_ocd_write_gpr \
                       updi_ocd_read_sreg updi_ocd_write_sreg \
                       updi_ocd_read_sp updi_ocd_write_sp \
					   updi_ocd_read_pc updi_ocd_write_pc \
					   updi_ocd_stabilize_pc_after_write \
					   updi_ocd_step_inject_word0 \
                       updi_ocd_emulate_cof_32bit \
                       updi_ocd_set_hw_bp updi_ocd_clear_hw_bp \
                       updi_save_peripherals updi_restore_peripherals \
                       fsm_build_thread_list \
                       fsm_get_active_thread fsm_invalidate \
                       monitor_dispatch monitor_dispatch_ex
TEST_EXTRA_LDFLAGS_test_rsp :=

# test_main — test_main.c #includes src/main.c so it can reach the
# static parse_args() / event_loop() / load_flash_segments() helpers.
TEST_SRCS_test_main := $(TESTDIR)/test_main.c $(SRCDIR)/dap.c $(SRCDIR)/debug_bp.c $(TESTDIR)/ocd_stubs.c
TEST_WRAP_test_main  := updi_open updi_close updi_console_poll \
                        updi_select_device updi_get_device \
                        updi_nvm_write_flash updi_nvm_flash_patch \
                        updi_nvm_write_eeprom updi_nvm_write_userrow \
                        updi_nvm_write_fuses updi_nvm_write_lockbits \
                        updi_chip_erase updi_enter_debug updi_halt updi_run \
                        updi_set_debug_in_sleep \
                        updi_nvm_read updi_probe_baud updi_crc32 \
                        updi_format_fuses updi_set_nvm_progress \
                        rsp_listen rsp_accept rsp_close \
                        rsp_recv_packet rsp_dispatch rsp_dispatch_n \
                        rsp_default_handlers \
                        elf_open elf_find_avros_tables elf_close \
                        fsm_build_thread_list select
TEST_EXTRA_LDFLAGS_test_main :=

# test_integration (no source wrapping — launches the real binary via execv)
TEST_SRCS_test_integration := $(TESTDIR)/test_integration.c
TEST_WRAP_test_integration  :=
TEST_EXTRA_LDFLAGS_test_integration :=

# test_install (no source wrapping — runs `make` sub-invocations and inspects fs)
TEST_SRCS_test_install := $(TESTDIR)/test_install.c
TEST_WRAP_test_install  :=
TEST_EXTRA_LDFLAGS_test_install :=

# test_device — Phase 7 --device diagnostic mode. test_device.c #includes
# src/main.c so it can reach parse_args / run_device_mode / app_main.
# updi.c is linked in real so updi_read_device_info exercises the PTY
# harness; updi_open / updi_close are wrapped to substitute a pre-opened
# PTY slave fd.
TEST_SRCS_test_device := $(TESTDIR)/test_device.c $(SRCDIR)/updi.c $(SRCDIR)/dap.c $(SRCDIR)/debug_bp.c
TEST_WRAP_test_device  := select updi_open updi_close \
                          updi_nvm_write_flash updi_console_poll \
                          updi_probe_baud updi_nvm_read \
                          rsp_listen rsp_accept rsp_close \
                          rsp_recv_packet rsp_dispatch rsp_dispatch_n \
                          rsp_default_handlers \
                          elf_open elf_close elf_find_avros_tables \
                          fsm_build_thread_list
TEST_EXTRA_LDFLAGS_test_device := $(LUTIL) -lpthread

# Master list
TEST_NAMES := test_elf test_updi test_fsm test_monitor test_rsp test_main test_integration test_install test_device test_dap

# Build a --wrap flag string from a space-separated list of symbols
wrap_flags = $(foreach sym,$(1),-Wl,--wrap,$(sym))

# ── Default target ─────────────────────────────────────────────────────────────
.PHONY: all
all: $(BUILDDIR)/$(TARGET)

# ── Main binary link ──────────────────────────────────────────────────────────
$(BUILDDIR)/$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(ELFUTILS_LIBS)
	@echo "  LD  $@"

# ── Compile host object files ─────────────────────────────────────────────────
# `-MMD -MP` emits a per-object `.d` makefrag listing the headers each object
# depends on, so editing a header (e.g. a field added to RspContext) forces a
# rebuild of every dependent object.  Without this, an incremental build leaves
# stale objects with a mismatched struct layout — a silent, memory-corrupting
# footgun.  (Test binaries compile all their sources in one invocation, so they
# are always consistent and need no `.d` tracking.)
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(Q)$(CC) $(CFLAGS) -MMD -MP -I$(SRCDIR) -c $< -o $@
	@echo "  CC  $<"

-include $(OBJS:.o=.d)

# ── Unity object ─────────────────────────────────────────────────────────────
$(UNITY_OBJ): $(UNITY_SRC)
	@mkdir -p $(BUILDDIR)
	$(Q)$(CC) $(CFLAGS) -I$(UNITYDIR) -c $< -o $@
	@echo "  CC  $<"

# ── AVR ELF fixtures ─────────────────────────────────────────────────────────
$(FIXBINDIR)/%.elf: $(FIXTUREDIR)/%.c
	@mkdir -p $(FIXBINDIR)
	$(Q)$(AVR_CC) $(AVR_CFLAGS) -o $@ $<
	@echo "  AVR-CC  $<"

# LLR-MAIN-13 mismatch fixture: built with -mmcu=avr64dd32 so the ELF's
# `.note.gnu.avr.deviceinfo` reports "avr64dd32" (family AVR-DD).
# Explicit recipe overrides the generic pattern rule above.
$(WRONG_FAMILY_ELF): $(WRONG_FAMILY_SRC)
	@mkdir -p $(FIXBINDIR)
	$(Q)$(AVR_CC) -mmcu=$(WRONG_FAMILY_MCU) $(WRONG_FAMILY_DFP) -Os -g \
	    -o $@ $<
	@echo "  AVR-CC  $<  (mcu=$(WRONG_FAMILY_MCU))"

$(NOT_AVR_ELF): $(NOT_AVR_SRC)
	@mkdir -p $(FIXBINDIR)
	$(Q)$(CC) -m32 -o $@ $< 2>/dev/null || \
	  $(CC) -o $@ $<
	@echo "  CC (non-AVR fixture)  $<"

# G17 locals fixture: -O0 so every local keeps a deterministic stack slot.
# Explicit recipe overrides the generic -Os pattern rule above.
$(GDB_LOCALS_ELF): $(GDB_LOCALS_SRC)
	@mkdir -p $(FIXBINDIR)
	$(Q)$(AVR_CC) -mmcu=$(AVR_MCU) $(DFP_FLAGS) -O0 -g -o $@ $<
	@echo "  AVR-CC  $<  (-O0, locals fixture)"

# Phase 14 debug-session fixture: -O0 so the call chain and per-frame
# locals/params stay deterministic.  Explicit recipe overrides the -Os pattern.
$(GDB_DBG_SESSION_ELF): $(GDB_DBG_SESSION_SRC)
	@mkdir -p $(FIXBINDIR)
	$(Q)$(AVR_CC) -mmcu=$(AVR_MCU) $(DFP_FLAGS) -O0 -g -o $@ $<
	@echo "  AVR-CC  $<  (-O0, debug-session fixture)"

# ── Generic rule: build one test binary ──────────────────────────────────────
# $(1) = test name (e.g. test_elf)
define TEST_template
$(TESTBINDIR)/$(1): $$(TEST_SRCS_$(1)) $(UNITY_OBJ)
	@mkdir -p $(TESTBINDIR)
	$(Q)$(CC) $(TEST_CFLAGS) \
	    -I$(SRCDIR) $(UNITY_INC) \
	    $$(call wrap_flags,$$(TEST_WRAP_$(1))) \
	    -o $$@ \
	    $$(TEST_SRCS_$(1)) $(UNITY_OBJ) \
	    $$(TEST_EXTRA_LDFLAGS_$(1)) $(SAN_FLAGS) $(ELFUTILS_LIBS)
	@echo "  LD  $$@"
endef

$(foreach t,$(TEST_NAMES),$(eval $(call TEST_template,$(t))))

# ── test target ───────────────────────────────────────────────────────────────
.PHONY: test
test: fixtures $(addprefix $(TESTBINDIR)/,$(TEST_NAMES))
	@echo ""
	@echo "══════════════════════════════════════════════════════"
	@echo " Running test suite"
	@echo "══════════════════════════════════════════════════════"
	@PASS=0; FAIL=0; \
	for t in $(TEST_NAMES); do \
	    echo ""; \
	    echo "── $$t ──────────────────────────────────────────────"; \
	    if $(TESTBINDIR)/$$t; then \
	        PASS=$$((PASS+1)); \
	    else \
	        FAIL=$$((FAIL+1)); \
	    fi; \
	done; \
	echo ""; \
	echo "══════════════════════════════════════════════════════"; \
	echo " Results: $$PASS passed, $$FAIL failed"; \
	echo "══════════════════════════════════════════════════════"; \
	test $$FAIL -eq 0

# ── fixtures target ───────────────────────────────────────────────────────────
.PHONY: fixtures
fixtures: $(FIXTURE_ELFS) $(NOT_AVR_ELF) $(WRONG_FAMILY_ELF) $(GDB_LOCALS_ELF) \
          $(GDB_DBG_SESSION_ELF)

# ── test-ci target ────────────────────────────────────────────────────────────
# Like 'test' but writes each suite's output to build/test-results/<name>.txt
# so the CI workflow can parse Unity results and post a PR summary.
# All suite output is also echoed to stdout for the Actions log.
.PHONY: test-ci
test-ci: fixtures $(addprefix $(TESTBINDIR)/,$(TEST_NAMES))
	@mkdir -p $(BUILDDIR)/test-results
	@FAIL=0; \
	for t in $(TEST_NAMES); do \
	    echo "── $$t ──────────────────────────────────────────────"; \
	    $(TESTBINDIR)/$$t > $(BUILDDIR)/test-results/$$t.txt 2>&1; \
	    RET=$$?; \
	    cat $(BUILDDIR)/test-results/$$t.txt; \
	    echo ""; \
	    if [ $$RET -ne 0 ]; then FAIL=1; fi; \
	done; \
	exit $$FAIL

# ── coverage target ──────────────────────────────────────────────────────────
# Build the host sources + every Unity test suite with gcov instrumentation,
# run them, then drive `gcovr` to produce a line + branch coverage report.
#
# Outputs (under build/coverage/):
#   summary.txt        plain-text gcovr --print-summary
#   coverage.txt       per-file gcovr report (lines + branches)
#   coverage.xml       Cobertura XML (for CI ingestion)
#   coverage.json      gcovr JSON summary (for programmatic parsing)
#   html/index.html    drill-down HTML report (uploaded as a CI artifact)
#
# Usage:
#   make coverage              # full clean build + tests + report
#   make coverage GCOVR_FILTER='^src/updi\.c'   # restrict to one file
#
# Notes:
#   * Forces a clean build because gcov data files are tied to the exact
#     compilation flags.
#   * COVERAGE=1 also disables inlining so branch counters stay aligned
#     with the source.
#   * ASAN and COVERAGE are mutually compatible but ASAN slows test runs;
#     CI runs coverage with ASAN off.
GCOVR_FILTER ?= ^src/
GCOVR        ?= gcovr

.PHONY: coverage
coverage:
	@echo "── coverage: rebuilding with --coverage ─────────────────"
	$(Q)$(MAKE) --no-print-directory clean
	$(Q)$(MAKE) --no-print-directory test-ci COVERAGE=1 ASAN=
	@mkdir -p $(BUILDDIR)/coverage/html
	@echo "── coverage: generating report (lines + branches) ───────"
	$(Q)$(GCOVR) --root . \
	    --filter '$(GCOVR_FILTER)' \
	    --exclude '^tests/' \
	    --exclude '^tools/' \
	    --print-summary \
	    --txt              $(BUILDDIR)/coverage/coverage.txt \
	    --cobertura        $(BUILDDIR)/coverage/coverage.xml \
	    --json-summary     $(BUILDDIR)/coverage/coverage.json \
	    --json-summary-pretty \
	    --html-details     $(BUILDDIR)/coverage/html/index.html \
	    --html-title       "avrOSdb coverage" \
	    | tee $(BUILDDIR)/coverage/summary.txt
	@echo ""
	@echo "── coverage: per-file (lines + branches) ────────────────"
	@cat $(BUILDDIR)/coverage/coverage.txt
	@echo ""
	@echo "Report artefacts:"
	@echo "  $(BUILDDIR)/coverage/summary.txt"
	@echo "  $(BUILDDIR)/coverage/coverage.txt"
	@echo "  $(BUILDDIR)/coverage/coverage.xml   (Cobertura)"
	@echo "  $(BUILDDIR)/coverage/coverage.json  (gcovr JSON summary)"
	@echo "  $(BUILDDIR)/coverage/html/index.html"

# ── hw-test target ────────────────────────────────────────────────────────────
# On-target hardware integration tests. MANUAL ONLY — never wired into `make
# test` (which must remain hardware-independent for CI). Runs against a real
# AVR-Dx target connected via UPDI (single-wire) on a serial device.
#
# Usage:
#   make hw-test                                     # safe (Groups A + B)
#   make hw-test HW_PORT=/dev/ttyUSB0
#   make hw-test PORT=/dev/ttyAMA2                   # PORT= alias accepted
#   make hw-test HW_SRAM_ADDR=0x6000 HW_VERBOSE=1    # probe alt SRAM, trace
#   make hw-test HW_DEVICE_ID="1E 97 0A"             # enable deviceid check
#   make hw-test HW_ADDR_24BIT=0x10000               # exercise ST_PTR_LONG
#   make hw-test-nvm   HW_TEST_NVM_CONFIRM=YES       # opt-in destructive NVM
#   make hw-test-nvm   HW_TEST_NVM_CONFIRM=YES HW_FLASH_PAGE_ADDR=0x1FE00
#   make hw-test-rsp   HW_TEST_ELF=path/to/fw.elf    # spawn RSP server + probe
#   make hw-test-rsp   HW_TEST_ELF=fw.elf HW_RSP_PORT=2345
#   make hw-test-all   HW_TEST_NVM_CONFIRM=YES HW_TEST_ELF=...
#
# Environment variables (all optional; passed through to the hw_test binary):
#   HW_PORT              serial device (default /dev/ttyAMA2)
#                        PORT= is accepted as a shorthand alias.
#   HW_DEVICE_ID         expected SIGROW DEVICEID, e.g. "1E 97 0A" — when
#                        set, Group-A A3 verifies the chip matches
#   HW_SRAM_ADDR         scratch SRAM byte address for B-group round-trips
#                        (default 0x7000 — mid-RAM on AVR128DA, clear of
#                        .data/.bss and stack). Pick another address if the
#                        running firmware uses this region — diagnostic case
#                        B1c will detect and warn.
#   HW_FLASH_PAGE_ADDR   scratch FLASH page for NVM test (default 0x7E00,
#                        which is the last page of AVR128DA28; override for
#                        DA48/DB48 etc., e.g. 0x1FE00 for 128 KiB parts)
#   HW_ADDR_24BIT        address >0xFFFF to exercise ST_PTR_LONG; 0 = skip B3
#   HW_TEST_ELF          ELF for --with-rsp child process (hw-test-rsp/all)
#   HW_RSP_PORT          TCP port for RSP smoke test (default 1234)
#   HW_VERBOSE           0/1/2 — per-step trace and hex dumps (default 0)
#   HW_TEST_NVM_CONFIRM  must be YES to allow hw-test-nvm (destructive)
HW_PORT              ?= /dev/ttyAMA2
HW_DEVICE_ID         ?=
HW_SRAM_ADDR         ?=
HW_FLASH_PAGE_ADDR   ?=
HW_ADDR_24BIT        ?=
HW_TEST_ELF          ?=
HW_TEST_NVM_ELF      ?=
HW_RSP_PORT          ?=
HW_VERBOSE           ?=
HW_TEST_NVM_CONFIRM  ?=
# Accept PORT= as a shorthand alias for HW_PORT=
ifneq ($(PORT),)
HW_PORT := $(PORT)
endif

HW_TEST_SRC := tests/hw/hw_test.c
HW_TEST_BIN := $(BUILDDIR)/hw_test
HW_ENV       = HW_PORT='$(HW_PORT)' \
               HW_DEVICE_ID='$(HW_DEVICE_ID)' \
               HW_SRAM_ADDR='$(HW_SRAM_ADDR)' \
               HW_FLASH_PAGE_ADDR='$(HW_FLASH_PAGE_ADDR)' \
               HW_ADDR_24BIT='$(HW_ADDR_24BIT)' \
               HW_TEST_ELF='$(HW_TEST_ELF)' \
               HW_TEST_NVM_ELF='$(HW_TEST_NVM_ELF)' \
               HW_RSP_PORT='$(HW_RSP_PORT)' \
               HW_VERBOSE='$(HW_VERBOSE)'

$(HW_TEST_BIN): $(HW_TEST_SRC) $(BUILDDIR)/updi.o
	@mkdir -p $(BUILDDIR)
	$(Q)$(CC) $(CFLAGS) -I$(SRCDIR) -o $@ $^ $(LUTIL)
	@echo "  LD  $@"

.PHONY: hw-test hw-test-nvm hw-test-rsp hw-test-gdb hw-test-all hw-test-dap hw-test-dap-unwind
hw-test: $(HW_TEST_BIN)
	$(Q)$(HW_ENV) $(HW_TEST_BIN)

# Destructive: programs the last FLASH page (HW_FLASH_PAGE_ADDR) and the
# EEPROM + USERROW windows (Phase-8 Group E, from the all_nvm.elf fixture).
# Requires explicit HW_TEST_NVM_CONFIRM=YES to fire.
hw-test-nvm: $(HW_TEST_BIN) $(FIXBINDIR)/all_nvm.elf
	@if [ "$(HW_TEST_NVM_CONFIRM)" != "YES" ]; then \
	    echo "hw-test-nvm: refused — set HW_TEST_NVM_CONFIRM=YES to confirm"; \
	    echo "             (this will erase + rewrite FLASH page at $(if $(HW_FLASH_PAGE_ADDR),$(HW_FLASH_PAGE_ADDR),0x7E00),"; \
	    echo "              and overwrite EEPROM @0x814000 + USERROW @0x810080 from all_nvm.elf)"; \
	    exit 1; \
	fi
	$(Q)$(HW_ENV) HW_TEST_NVM_ELF='$(if $(HW_TEST_NVM_ELF),$(HW_TEST_NVM_ELF),$(FIXBINDIR)/all_nvm.elf)' \
	    $(HW_TEST_BIN) --with-nvm

# Spawns the RSP server as a child, probes TCP. Requires HW_TEST_ELF.
hw-test-rsp: $(HW_TEST_BIN) all
	@if [ -z "$(HW_TEST_ELF)" ]; then \
	    echo "hw-test-rsp: refused — HW_TEST_ELF=path/to/fw.elf is required"; \
	    exit 1; \
	fi
	$(Q)$(HW_ENV) $(HW_TEST_BIN) --with-rsp

# Full-stack GDB acceptance (Group G, Phase 10 — avarice feature parity).
# Spawns build/avrOSdb, drives a real avr-gdb -batch session through the
# acceptance script, validates load/break/watch/monitor/detach. G1-G9 run
# against the existing gdb_target fixture; G10 runs against a local avrOS
# example ELF with real FSM symbols so the reset/thread regression stays
# reproducible. DESTRUCTIVE: `(gdb) load` reprograms FLASH from the active ELF.
HW_GDB_ELF ?= $(FIXBINDIR)/gdb_target.elf
HW_GDB_G10_ELF ?= tests/hw/fixtures/avrOS_example_main.elf
HW_GDB_G17_ELF ?= $(GDB_LOCALS_ELF)
HW_GDB_DBG_ELF ?= $(GDB_DBG_SESSION_ELF)
hw-test-gdb: $(FIXBINDIR)/gdb_target.elf $(GDB_LOCALS_ELF) $(GDB_DBG_SESSION_ELF) all
	@if [ "$(HW_TEST_NVM_CONFIRM)" != "YES" ]; then \
	    echo "hw-test-gdb: refused — set HW_TEST_NVM_CONFIRM=YES to confirm"; \
	    echo "             (this reflashes the target via `(gdb) load`)"; \
	    exit 1; \
	fi
	@if ! command -v avr-gdb >/dev/null 2>&1; then \
	    echo "hw-test-gdb: required tool not found: avr-gdb" >&2; \
	    exit 1; \
	fi
	$(Q)HW_PORT='$(HW_PORT)' HW_RSP_PORT='$(if $(HW_RSP_PORT),$(HW_RSP_PORT),1234)' \
	    python3 tests/hw/gdb_acceptance.py \
	        --port      '$(HW_PORT)' \
	        --rsp-port  '$(if $(HW_RSP_PORT),$(HW_RSP_PORT),1234)' \
	        --elf       '$(HW_GDB_ELF)' \
	        --g10-elf   '$(HW_GDB_G10_ELF)' \
	        --g17-elf   '$(HW_GDB_G17_ELF)' \
	        --dbg-elf   '$(HW_GDB_DBG_ELF)' \
	        --avros-bin '$(BUILDDIR)/$(TARGET)'

# Run Groups A + B + C + D in one go. NVM still requires explicit confirm.
hw-test-all: $(HW_TEST_BIN) $(FIXBINDIR)/all_nvm.elf all
	@if [ "$(HW_TEST_NVM_CONFIRM)" != "YES" ]; then \
	    echo "hw-test-all: refused — set HW_TEST_NVM_CONFIRM=YES to include NVM"; \
	    exit 1; \
	fi
	@if [ -z "$(HW_TEST_ELF)" ]; then \
	    echo "hw-test-all: refused — HW_TEST_ELF=path/to/fw.elf is required"; \
	    exit 1; \
	fi
	$(Q)$(HW_ENV) HW_TEST_NVM_ELF='$(if $(HW_TEST_NVM_ELF),$(HW_TEST_NVM_ELF),$(FIXBINDIR)/all_nvm.elf)' \
	    $(HW_TEST_BIN) --with-nvm --with-rsp

# DAP acceptance harness (Phase 16+). Spawns `avrOSdb --dap` and drives it with
# the real nvim-dap client (headless Neovim) over TCP, asserting the connection
# lifecycle. Non-destructive (attach only — does not reflash). Requires `nvim`
# (and network on first run to fetch nvim-dap into tests/hw/.nvim-dap).
HW_DAP_PORT ?= 1234
HW_DAP_ELF  ?= $(FIXBINDIR)/gdb_target.elf
hw-test-dap: $(FIXBINDIR)/gdb_target.elf all
	@if ! command -v nvim >/dev/null 2>&1; then \
	    echo "hw-test-dap: required tool not found: nvim" >&2; \
	    exit 1; \
	fi
	$(Q)AVROSDB_BIN='$(BUILDDIR)/$(TARGET)' HW_PORT='$(HW_PORT)' \
	    DAP_PORT='$(HW_DAP_PORT)' DAP_ELF='$(HW_DAP_ELF)' \
	    nvim --headless -u tests/hw/dap_init.lua -l tests/hw/dap_acceptance.lua

# hw-test-dap-unwind — DAP multi-frame stackTrace acceptance (Phase 18,
# HLR-080).  Spawns avrOSdb --dap against the gdb_debug_session fixture's
# main->top->mid->leaf chain and verifies the DWARF-CFI unwinder reports every
# frame.  Pure-Python DAP client (no nvim).  Non-destructive beyond reflashing
# the fixture it needs to debug.
hw-test-dap-unwind: $(GDB_DBG_SESSION_ELF) all
	$(Q)$(BUILDDIR)/$(TARGET) --prog --erase $(HW_PORT) $(GDB_DBG_SESSION_ELF)
	$(Q)AVROSDB_BIN='$(BUILDDIR)/$(TARGET)' HW_PORT='$(HW_PORT)' \
	    DAP_PORT='$(HW_DAP_PORT)' \
	    python3 tests/hw/dap_unwind.py --port '$(HW_PORT)' \
	        --dap-port '$(HW_DAP_PORT)' --elf '$(GDB_DBG_SESSION_ELF)'

# ── check-tools target ────────────────────────────────────────────────────────
# LLR-INST-01: verify every required host tool is on PATH.
.PHONY: check-tools
check-tools:
	@missing=0; \
	for tool in $(CC) make $(AVR_CC) avr-nm; do \
	    if ! command -v $$tool >/dev/null 2>&1; then \
	        echo "ERROR: required tool not found: $$tool" >&2; \
	        missing=1; \
	    fi; \
	done; \
	if [ $$missing -ne 0 ]; then exit 1; fi; \
	echo "All required tools found."

# ── install target ────────────────────────────────────────────────────────────
# LLR-INST-02: install binary (0755) and man page (0644) under $(PREFIX).
.PHONY: install
install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 0755 $(BUILDDIR)/$(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 0644 $(MANPAGE) $(DESTDIR)$(MANDIR)/$(notdir $(MANPAGE))
	@echo "  INSTALL  $(BINDIR)/$(TARGET)"
	@echo "  INSTALL  $(MANDIR)/$(notdir $(MANPAGE))"

# ── uninstall target ──────────────────────────────────────────────────────────
# LLR-INST-03: idempotent removal of files placed by `install`.
.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(MANDIR)/$(notdir $(MANPAGE))
	@echo "  UNINSTALL  $(BINDIR)/$(TARGET)"
	@echo "  UNINSTALL  $(MANDIR)/$(notdir $(MANPAGE))"

# ── bundle target ─────────────────────────────────────────────────────────────
# LLR-INST-06..08: build dist/*.deb, dist/*.rpm, dist/*.rb (Homebrew formula).
DEB_PKG   := $(DISTDIR)/avrOSdb_$(VERSION)_amd64.deb
RPM_PKG   := $(DISTDIR)/avrOSdb-$(VERSION)-1.x86_64.rpm
BREW_FILE := $(DISTDIR)/avrOSdb.rb

.PHONY: bundle bundle-deb bundle-rpm bundle-brew
bundle: bundle-deb bundle-rpm bundle-brew

# .deb — uses dpkg-deb if present; otherwise emits a clear error.
bundle-deb: $(BUILDDIR)/$(TARGET) $(MANPAGE)
	@command -v dpkg-deb >/dev/null 2>&1 || { \
	    echo "ERROR: dpkg-deb not found; cannot build $(DEB_PKG)" >&2; exit 1; }
	@mkdir -p $(DISTDIR)
	$(Q)rm -rf $(BUILDDIR)/deb
	$(Q)install -d $(BUILDDIR)/deb/DEBIAN \
	              $(BUILDDIR)/deb/usr/bin \
	              $(BUILDDIR)/deb/usr/share/man/man1
	$(Q)install -m 0755 $(BUILDDIR)/$(TARGET) $(BUILDDIR)/deb/usr/bin/$(TARGET)
	$(Q)install -m 0644 $(MANPAGE) $(BUILDDIR)/deb/usr/share/man/man1/$(notdir $(MANPAGE))
	$(Q)printf 'Package: avrOSdb\nVersion: %s\nArchitecture: amd64\nMaintainer: John Anderson <racerxr650r@example.com>\nDescription: UPDI-to-GDB debug stub with avrOS FSM awareness\n .\n A GDB Remote Serial Protocol server bridging avr-gdb to AVR DA/DB\n targets over the UPDI single-wire debug interface. Adds first-class\n awareness of avrOS cooperative FSM tasks as GDB virtual threads.\nSection: devel\nPriority: optional\n' $(VERSION) > $(BUILDDIR)/deb/DEBIAN/control
	$(Q)dpkg-deb --build --root-owner-group $(BUILDDIR)/deb $(DEB_PKG) >/dev/null
	@echo "  BUNDLE  $(DEB_PKG)"

# .rpm — uses rpmbuild if present; otherwise emits a clear error.
bundle-rpm: $(BUILDDIR)/$(TARGET) $(MANPAGE)
	@command -v rpmbuild >/dev/null 2>&1 || { \
	    echo "ERROR: rpmbuild not found; cannot build $(RPM_PKG)" >&2; exit 1; }
	@mkdir -p $(DISTDIR)
	$(Q)rm -rf $(BUILDDIR)/rpm
	$(Q)install -d $(BUILDDIR)/rpm/BUILD $(BUILDDIR)/rpm/RPMS $(BUILDDIR)/rpm/SOURCES \
	              $(BUILDDIR)/rpm/SPECS $(BUILDDIR)/rpm/SRPMS \
	              $(BUILDDIR)/rpm/buildroot/usr/bin \
	              $(BUILDDIR)/rpm/buildroot/usr/share/man/man1
	$(Q)install -m 0755 $(BUILDDIR)/$(TARGET) $(BUILDDIR)/rpm/buildroot/usr/bin/$(TARGET)
	$(Q)install -m 0644 $(MANPAGE) $(BUILDDIR)/rpm/buildroot/usr/share/man/man1/$(notdir $(MANPAGE))
	$(Q)printf 'Name:    avrOSdb\nVersion: %s\nRelease: 1\nSummary: UPDI-to-GDB debug stub with avrOS FSM awareness\nLicense: MIT\nBuildArch: x86_64\n\n%%description\nA GDB Remote Serial Protocol server bridging avr-gdb to AVR DA/DB\ntargets over the UPDI single-wire debug interface.\n\n%%install\nmkdir -p %%{buildroot}/usr/bin %%{buildroot}/usr/share/man/man1\ncp -a $(abspath $(BUILDDIR))/rpm/buildroot/usr/bin/$(TARGET) %%{buildroot}/usr/bin/\ncp -a $(abspath $(BUILDDIR))/rpm/buildroot/usr/share/man/man1/$(notdir $(MANPAGE)) %%{buildroot}/usr/share/man/man1/\n\n%%files\n/usr/bin/avrOSdb\n/usr/share/man/man1/avrOSdb.1\n' $(VERSION) > $(BUILDDIR)/rpm/SPECS/avrOSdb.spec
	$(Q)rpmbuild --quiet --define "_topdir $(abspath $(BUILDDIR))/rpm" \
	             --define "_rpmdir $(abspath $(DISTDIR))" \
	             --define "_rpmfilename avrOSdb-$(VERSION)-1.x86_64.rpm" \
	             --define "_build_id_links none" \
	             --target x86_64-linux \
	             -bb $(BUILDDIR)/rpm/SPECS/avrOSdb.spec >/dev/null
	@echo "  BUNDLE  $(RPM_PKG)"

# Homebrew formula — a self-contained .rb file (no tarball download required).
bundle-brew: $(BUILDDIR)/$(TARGET) $(MANPAGE)
	@mkdir -p $(DISTDIR)
	$(Q)printf 'class Avrosdb < Formula\n  desc "UPDI-to-GDB debug stub with avrOS FSM awareness"\n  homepage "https://github.com/racerxr650r/avrOS-debug"\n  url "https://github.com/racerxr650r/avrOS-debug/archive/refs/tags/v%s.tar.gz"\n  sha256 "0000000000000000000000000000000000000000000000000000000000000000"\n  version "%s"\n  license "MIT"\n\n  def install\n    system "make"\n    bin.install "build/avrOSdb"\n    man1.install "doc/avrOSdb.1"\n  end\n\n  test do\n    assert_match "avrOSdb", shell_output("#{bin}/avrOSdb --help 2>&1", 1)\n  end\nend\n' $(VERSION) $(VERSION) > $(BREW_FILE)
	@echo "  BUNDLE  $(BREW_FILE)"
# ── prereqs target ───────────────────────────────────────────────────────────
# Install all development prerequisites (Debian/Ubuntu; requires sudo).
# Installs host build tools via apt, then downloads and installs the
# Microchip AVR-Dx Device Family Pack so avr-gcc can target AVR DA/DB parts.
# Also installs the packaging tools required by `make bundle`
# (dpkg-deb, rpmbuild, ruby) and the man(1) renderer used by tests, plus
# the elfutils dev libraries (libdw/libelf) that enable the optional
# DWARF source-level features auto-detected by the build (see the DWARF
# block above and doc/reference/dual-protocol-architecture.md).  Finally it
# sets up Neovim for DAP debugging via `make prereqs-nvim` (below).
.PHONY: prereqs
prereqs:
	@echo "── Installing apt packages ──────────────────────────────────────"
	sudo apt-get update -q
	sudo apt-get install -y --no-install-recommends \
	    make gcc binutils gcc-avr binutils-avr avr-libc wget unzip \
	    dpkg-dev rpm ruby man-db groff \
	    libdw-dev libelf-dev neovim git
	@echo "── Installing AVR-Dx DFP $(DFP_VER) ──────────────────────────"
	wget -q -O /tmp/$(DFP_PACK) $(DFP_URL)
	unzip -q -o /tmp/$(DFP_PACK) -d /tmp/Atmel.AVR-Dx_DFP.$(DFP_VER)
	sudo mkdir -p $(dir $(DFP))
	sudo cp -R /tmp/Atmel.AVR-Dx_DFP.$(DFP_VER) $(DFP)
	rm -rf /tmp/Atmel.AVR-Dx_DFP.$(DFP_VER) /tmp/$(DFP_PACK)
	@$(MAKE) --no-print-directory prereqs-nvim
	@echo "── Prerequisites installed successfully ──────────────────────"

# ── prereqs-nvim target ───────────────────────────────────────────────────────
# Set up Neovim for DAP debugging of `avrOSdb --dap`: install nvim-dap as a
# native Neovim package and install the avrOSdb DAP config into the user's
# init.lua.  Idempotent and non-destructive: an existing init.lua is never
# clobbered — the marked config block (tools/nvim/avrosdb-dap.lua) is appended
# only if not already present.  Edits the invoking user's HOME (no sudo).
NVIM_PACK_DIR := $(HOME)/.local/share/nvim/site/pack/dap/start/nvim-dap
NVIM_INIT     := $(HOME)/.config/nvim/init.lua
NVIM_DAP_CFG  := tools/nvim/avrosdb-dap.lua
.PHONY: prereqs-nvim
prereqs-nvim:
	@echo "── Neovim nvim-dap setup ─────────────────────────────────────"
	@if [ -d "$(NVIM_PACK_DIR)/.git" ]; then \
	    echo "  nvim-dap already installed at $(NVIM_PACK_DIR)"; \
	else \
	    mkdir -p "$(dir $(NVIM_PACK_DIR))"; \
	    git clone --depth=1 https://github.com/mfussenegger/nvim-dap "$(NVIM_PACK_DIR)"; \
	fi
	@mkdir -p "$(dir $(NVIM_INIT))"
	@if [ ! -f "$(NVIM_INIT)" ]; then \
	    cp "$(NVIM_DAP_CFG)" "$(NVIM_INIT)"; \
	    echo "  wrote $(NVIM_INIT)"; \
	elif grep -q "avrOSdb DAP config" "$(NVIM_INIT)"; then \
	    echo "  avrOSdb DAP config already present in $(NVIM_INIT)"; \
	else \
	    printf '\n' >> "$(NVIM_INIT)"; \
	    cat "$(NVIM_DAP_CFG)" >> "$(NVIM_INIT)"; \
	    echo "  appended avrOSdb DAP config to $(NVIM_INIT)"; \
	fi
# ── clean target ──────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	$(Q)rm -rf $(BUILDDIR) $(DISTDIR) AI-tmp
	@echo "  CLEAN  $(BUILDDIR)/ $(DISTDIR)/ AI-tmp/"
# ── help target ───────────────────────────────────────────────────────────
# Prints the top-of-file comment block from `# Targets:` up to (but not
# including) the first `# ──` section separator or the first non-comment line.
.PHONY: help
help:
	@awk '/^# Targets:/{found=1} found{if(/^[^#]/ || /^# ──/)exit; sub(/^# ?/,""); print}' $(MAKEFILE_LIST)