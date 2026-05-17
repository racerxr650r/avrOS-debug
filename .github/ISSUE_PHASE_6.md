# Phase 6 — Installation Targets & Documentation

Tracks the work defined in [doc/SDP.md §8 — Phase 6](../doc/SDP.md#phase-6--installation-targets--documentation).

**Depends on:** Phases 0–5 complete (binary `avr-updi-gdb` builds clean, all 102 unit + 4 integration tests pass).

## Scope

Add installation/packaging Makefile targets, hand-authored end-user documentation, a Unix man page, and install-level integration tests.

## Deliverables

### Makefile targets
- [ ] `make check-tools` — validates `gcc`/`cc`, `make`, `avr-gcc`, `avr-nm` are on `PATH`; prints diagnostic naming each missing tool; exits non-zero if any absent.
- [ ] `make install` — installs binary to `$(PREFIX)/bin/avr-updi-gdb` (mode 0755) and man page to `$(PREFIX)/share/man/man1/avr-updi-gdb.1` (mode 0644). Default `PREFIX=/usr/local`. Creates intermediate dirs via `install -d`.
- [ ] `make uninstall` — removes the two installed files with `rm -f`; idempotent (exits 0 when files already absent).
- [ ] `make bundle` — produces under `dist/`:
  - [ ] `dist/avr-updi-gdb_$(VERSION)_amd64.deb` via `dpkg-deb` (with `DEBIAN/control` declaring Package, Version, Architecture: amd64, Maintainer, Description).
  - [ ] `dist/avr-updi-gdb-$(VERSION)-1.x86_64.rpm` via `rpmbuild` (generated `.spec` with Name, Version, Release, Summary, License, `%install`, `%files`).
  - [ ] Homebrew formula artefact.
- [ ] `VERSION` variable (default `git describe --tags --always`, overridable; sourced from the root `VERSION` file) parameterises all package version strings.

### Documentation
- [ ] `doc/UserManual.md` — covers: prerequisites + minimum tool versions; build instructions (`make`, `make test`, `make install`); UPDI serial-adapter wiring (1 kΩ resistor, TX/RX orientation); every CLI option; ≥ 2 complete usage examples (one with `--load`, one without); VS Code built-in debugger integration walkthrough.
- [ ] `doc/avr-updi-gdb.1` — `groff` man page with sections: NAME, SYNOPSIS, DESCRIPTION, OPTIONS, OPERANDS, EXIT STATUS, EXAMPLES, SEE ALSO (`avr-gdb(1)`, `avrdude(1)`). Must parse cleanly via `man -l doc/avr-updi-gdb.1`.

### Tests (`tests/test_install.c` — 8 tests)
- [ ] (a) `check-tools` exits non-zero when a required tool is missing.
- [ ] (b) `make install PREFIX=...` places the binary at the correct path.
- [ ] (c) `make install` places the man page and it renders without error.
- [ ] (d) `make uninstall` removes installed files.
- [ ] (e) `doc/UserManual.md` exists and contains all required section headings.
- [ ] (f) `make bundle` produces a valid `.deb`.
- [ ] (g) `make bundle` produces a valid `.rpm`.
- [ ] (h) `make bundle` produces a valid Homebrew formula.

## Acceptance Criteria
- `make check-tools` exits 0 when all tools present.
- `make install PREFIX=/tmp/test` and `make uninstall PREFIX=/tmp/test` succeed.
- `make bundle VERSION=0.1.0` produces all artefacts under `dist/`.
- `man -l doc/avr-updi-gdb.1` exits 0.
- `make test` runs `tests/test_install` reporting 8/8 passing.
- `python3 tools/lint_project.py` reports 0 errors, 0 warnings.
- `make` completes clean under `-Wall -Wextra -Wpedantic`.

## Reference implementation pattern (from SDP)

```makefile
PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin
MANDIR  := $(PREFIX)/share/man/man1

install: all
	install -d $(BINDIR) $(MANDIR)
	install -m 0755 $(BINFILE) $(BINDIR)/avr-updi-gdb
	install -m 0644 doc/avr-updi-gdb.1 $(MANDIR)/avr-updi-gdb.1

uninstall:
	rm -f $(BINDIR)/avr-updi-gdb $(MANDIR)/avr-updi-gdb.1

check-tools:
	@command -v $(CC)   >/dev/null 2>&1 || { echo "ERROR: C compiler not found ($(CC))"; exit 1; }
	@command -v avr-gcc >/dev/null 2>&1 || { echo "ERROR: avr-gcc not found"; exit 1; }
	@command -v avr-nm  >/dev/null 2>&1 || { echo "ERROR: avr-nm not found"; exit 1; }
	@echo "All required tools found."
```

## Effort
S/M per SDP §10 — Makefile targets ~30 lines each; RPM/`.deb` boilerplate is moderate; most effort is the user manual and man-page prose.
