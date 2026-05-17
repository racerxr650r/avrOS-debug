# Makefile — avrOS-debug (aod)
#
# Targets:
#   all          Build the avr-updi-gdb host binary
#   test         Build ELF fixtures, build all test binaries, run them
#   test-ci      Like test, but writes per-suite output to build/test-results/*.txt
#                (used by CI to build a structured test summary)
#   clean        Remove all build artefacts
#   check-tools  Verify all required host tools are present on PATH
#   install      Install binary + man page under $(PREFIX) [default: /usr/local]
#   uninstall    Remove files placed by `install` (idempotent)
#   bundle       Build dist/ packages: .deb, .rpm, Homebrew formula
#   prereqs      Install all dev prerequisites via apt and download the AVR-Dx DFP
#                (Debian/Ubuntu only; requires sudo)
#   help         Print this target list
#
# Variables:
#   ASAN=1     Add -fsanitize=address,undefined to both host and test builds
#   PREFIX     Install prefix (default: /usr/local)
#   VERSION    Package version string (default: `git describe` or 0.0.0)
#   V=1        Verbose build (show full commands)

# ── Toolchain ────────────────────────────────────────────────────────────────
CC        := gcc
AVR_CC    := avr-gcc
AVR_STRIP := avr-strip
PREFIX    ?= /usr/local
BINDIR    := $(PREFIX)/bin
MANDIR    := $(PREFIX)/share/man/man1
MANPAGE   := doc/avr-updi-gdb.1

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

# ── Sanitizer flag ────────────────────────────────────────────────────────────
ifeq ($(ASAN),1)
  SAN_FLAGS := -fsanitize=address,undefined
else
  SAN_FLAGS :=
endif

# ── Verbose flag ─────────────────────────────────────────────────────────────
ifeq ($(V),1)
  Q :=
else
  Q := @
endif

# ── Common flags ─────────────────────────────────────────────────────────────
CFLAGS := -std=c99 -D_POSIX_C_SOURCE=200809L \
           -Wall -Wextra -Wpedantic \
           -Wstrict-prototypes -Wmissing-prototypes \
           -Wshadow \
           -O2 -g \
           $(SAN_FLAGS)

# Test builds: suppress warnings on __wrap_* stubs (no header declares them),
# and pass -DUNIT_TEST so src/main.c can exclude its main() entry point.
TEST_CFLAGS := $(CFLAGS) -DUNIT_TEST \
               -Wno-missing-prototypes -Wno-strict-prototypes

# ── Directories ───────────────────────────────────────────────────────────────
SRCDIR      := src
TESTDIR     := tests
FIXTUREDIR  := tests/fixtures
UNITYDIR    := tests/unity
BUILDDIR    := build
TESTBINDIR  := $(BUILDDIR)/tests
FIXBINDIR   := $(BUILDDIR)/fixtures

# ── Main binary ───────────────────────────────────────────────────────────────
TARGET   := avr-updi-gdb
SRCS     := $(SRCDIR)/main.c \
             $(SRCDIR)/updi.c \
             $(SRCDIR)/elf_parser.c \
             $(SRCDIR)/fsm_mapper.c \
             $(SRCDIR)/monitor.c \
             $(SRCDIR)/gdb_rsp.c
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
                  $(FIXTUREDIR)/avros_partial.c
FIXTURE_ELFS   := $(patsubst $(FIXTUREDIR)/%.c,$(FIXBINDIR)/%.elf,$(FIXTURE_SRCS))
# not_avr.elf is a plain i386 ELF — built with the host gcc targeting ELF
NOT_AVR_SRC    := $(FIXTUREDIR)/not_avr.c
NOT_AVR_ELF    := $(FIXBINDIR)/not_avr.elf

# ── Per-test configuration ─────────────────────────────────────────────────────
# Each entry: TEST_SRCS_<name>, TEST_WRAP_<name>, TEST_EXTRA_LDFLAGS_<name>
#
# test_elf
TEST_SRCS_test_elf  := $(TESTDIR)/test_elf.c $(SRCDIR)/elf_parser.c
TEST_WRAP_test_elf  := malloc
TEST_EXTRA_LDFLAGS_test_elf :=

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
TEST_WRAP_test_monitor  := updi_mem_read rsp_send_packet
TEST_EXTRA_LDFLAGS_test_monitor :=

# test_rsp
TEST_SRCS_test_rsp := $(TESTDIR)/test_rsp.c \
                       $(SRCDIR)/gdb_rsp.c \
                       $(SRCDIR)/fsm_mapper.c \
                       $(SRCDIR)/monitor.c
TEST_WRAP_test_rsp  := updi_mem_read updi_mem_write updi_halt updi_run updi_step \
                       updi_nvm_write_flash updi_console_poll \
                       fsm_build_thread_list fsm_get_registers \
                       fsm_get_active_thread fsm_invalidate \
                       monitor_dispatch
TEST_EXTRA_LDFLAGS_test_rsp :=

# test_main — test_main.c #includes src/main.c so it can reach the
# static parse_args() / event_loop() / load_flash_segments() helpers.
TEST_SRCS_test_main := $(TESTDIR)/test_main.c
TEST_WRAP_test_main  := updi_open updi_close updi_console_poll \
                        updi_nvm_write_flash \
                        rsp_listen rsp_accept rsp_close \
                        rsp_recv_packet rsp_dispatch \
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
TEST_SRCS_test_device := $(TESTDIR)/test_device.c $(SRCDIR)/updi.c
TEST_WRAP_test_device  := select updi_open updi_close \
                          updi_nvm_write_flash updi_console_poll \
                          rsp_listen rsp_accept rsp_close \
                          rsp_recv_packet rsp_dispatch \
                          rsp_default_handlers \
                          elf_open elf_close elf_find_avros_tables \
                          fsm_build_thread_list
TEST_EXTRA_LDFLAGS_test_device := $(LUTIL) -lpthread

# Master list
TEST_NAMES := test_elf test_updi test_fsm test_monitor test_rsp test_main test_integration test_install test_device

# Build a --wrap flag string from a space-separated list of symbols
wrap_flags = $(foreach sym,$(1),-Wl,--wrap,$(sym))

# ── Default target ─────────────────────────────────────────────────────────────
.PHONY: all
all: $(BUILDDIR)/$(TARGET)

# ── Main binary link ──────────────────────────────────────────────────────────
$(BUILDDIR)/$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(Q)$(CC) $(CFLAGS) -o $@ $^
	@echo "  LD  $@"

# ── Compile host object files ─────────────────────────────────────────────────
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(Q)$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@
	@echo "  CC  $<"

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

$(NOT_AVR_ELF): $(NOT_AVR_SRC)
	@mkdir -p $(FIXBINDIR)
	$(Q)$(CC) -m32 -o $@ $< 2>/dev/null || \
	  $(CC) -o $@ $<
	@echo "  CC (non-AVR fixture)  $<"

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
	    $$(TEST_EXTRA_LDFLAGS_$(1)) $(SAN_FLAGS)
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
fixtures: $(FIXTURE_ELFS) $(NOT_AVR_ELF)

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
DEB_PKG   := $(DISTDIR)/avr-updi-gdb_$(VERSION)_amd64.deb
RPM_PKG   := $(DISTDIR)/avr-updi-gdb-$(VERSION)-1.x86_64.rpm
BREW_FILE := $(DISTDIR)/avr-updi-gdb.rb

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
	$(Q)printf 'Package: avr-updi-gdb\nVersion: %s\nArchitecture: amd64\nMaintainer: John Anderson <racerxr650r@example.com>\nDescription: UPDI-to-GDB debug stub with avrOS FSM awareness\n .\n A GDB Remote Serial Protocol server bridging avr-gdb to AVR DA/DB\n targets over the UPDI single-wire debug interface. Adds first-class\n awareness of avrOS cooperative FSM tasks as GDB virtual threads.\nSection: devel\nPriority: optional\n' $(VERSION) > $(BUILDDIR)/deb/DEBIAN/control
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
	$(Q)printf 'Name:    avr-updi-gdb\nVersion: %s\nRelease: 1\nSummary: UPDI-to-GDB debug stub with avrOS FSM awareness\nLicense: MIT\nBuildArch: x86_64\n\n%%description\nA GDB Remote Serial Protocol server bridging avr-gdb to AVR DA/DB\ntargets over the UPDI single-wire debug interface.\n\n%%install\nmkdir -p %%{buildroot}/usr/bin %%{buildroot}/usr/share/man/man1\ncp -a $(abspath $(BUILDDIR))/rpm/buildroot/usr/bin/$(TARGET) %%{buildroot}/usr/bin/\ncp -a $(abspath $(BUILDDIR))/rpm/buildroot/usr/share/man/man1/$(notdir $(MANPAGE)) %%{buildroot}/usr/share/man/man1/\n\n%%files\n/usr/bin/avr-updi-gdb\n/usr/share/man/man1/avr-updi-gdb.1\n' $(VERSION) > $(BUILDDIR)/rpm/SPECS/avr-updi-gdb.spec
	$(Q)rpmbuild --quiet --define "_topdir $(abspath $(BUILDDIR))/rpm" \
	             --define "_rpmdir $(abspath $(DISTDIR))" \
	             --define "_rpmfilename avr-updi-gdb-$(VERSION)-1.x86_64.rpm" \
	             --define "_build_id_links none" \
	             --target x86_64-linux \
	             -bb $(BUILDDIR)/rpm/SPECS/avr-updi-gdb.spec >/dev/null
	@echo "  BUNDLE  $(RPM_PKG)"

# Homebrew formula — a self-contained .rb file (no tarball download required).
bundle-brew: $(BUILDDIR)/$(TARGET) $(MANPAGE)
	@mkdir -p $(DISTDIR)
	$(Q)printf 'class AvrUpdiGdb < Formula\n  desc "UPDI-to-GDB debug stub with avrOS FSM awareness"\n  homepage "https://github.com/racerxr650r/avrOS-debug"\n  url "https://github.com/racerxr650r/avrOS-debug/archive/refs/tags/v%s.tar.gz"\n  sha256 "0000000000000000000000000000000000000000000000000000000000000000"\n  version "%s"\n  license "MIT"\n\n  def install\n    system "make"\n    bin.install "build/avr-updi-gdb"\n    man1.install "doc/avr-updi-gdb.1"\n  end\n\n  test do\n    assert_match "avr-updi-gdb", shell_output("#{bin}/avr-updi-gdb --help 2>&1", 1)\n  end\nend\n' $(VERSION) $(VERSION) > $(BREW_FILE)
	@echo "  BUNDLE  $(BREW_FILE)"
# ── prereqs target ───────────────────────────────────────────────────────────
# Install all development prerequisites (Debian/Ubuntu; requires sudo).
# Installs host build tools via apt, then downloads and installs the
# Microchip AVR-Dx Device Family Pack so avr-gcc can target AVR DA/DB parts.
# Also installs the packaging tools required by `make bundle`
# (dpkg-deb, rpmbuild, ruby) and the man(1) renderer used by tests.
.PHONY: prereqs
prereqs:
	@echo "── Installing apt packages ──────────────────────────────────────"
	sudo apt-get update -q
	sudo apt-get install -y --no-install-recommends \
	    make gcc binutils gcc-avr binutils-avr avr-libc wget unzip \
	    dpkg-dev rpm ruby man-db groff
	@echo "── Installing AVR-Dx DFP $(DFP_VER) ──────────────────────────"
	wget -q -O /tmp/$(DFP_PACK) $(DFP_URL)
	unzip -q -o /tmp/$(DFP_PACK) -d /tmp/Atmel.AVR-Dx_DFP.$(DFP_VER)
	sudo mkdir -p $(dir $(DFP))
	sudo cp -R /tmp/Atmel.AVR-Dx_DFP.$(DFP_VER) $(DFP)
	rm -rf /tmp/Atmel.AVR-Dx_DFP.$(DFP_VER) /tmp/$(DFP_PACK)
	@echo "── Prerequisites installed successfully ──────────────────────"
# ── clean target ──────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	$(Q)rm -rf $(BUILDDIR) $(DISTDIR)
	@echo "  CLEAN  $(BUILDDIR)/ $(DISTDIR)/"
# ── help target ───────────────────────────────────────────────────────────
.PHONY: help
help:
	@awk '/^# Targets:/{found=1} found{if(/^[^#]/ || /^#$$/)exit; sub(/^# ?/,""); print}' $(MAKEFILE_LIST)