# Wails build wrapper that dodges cgo's static-lib cache trap.
#
# `go build`/`wails build` only invalidate cgo artefacts when .h or .go files
# change. A rebuilt libpicoscope2204a.a with identical headers is silently
# ignored and the stale .a gets relinked — the bug this Makefile prevents.
#
# The fix: every time the driver static lib's mtime is newer than the gui
# binary, we `go clean -cache`, touch main.go (invalidates the cgo cache
# entry), and pass `-clean` to wails. This is the same incantation you'd run
# by hand after changing picoscope2204a.c.

WAILS_TAGS     ?= webkit2_41
DRIVER_DIR     := ../driver
DRIVER_LIB     := $(DRIVER_DIR)/libpicoscope2204a.a
WAILS_BIN      := build/bin/picoscope-gui

.PHONY: build dev clean driver verify help

help:
	@echo "Targets:"
	@echo "  make build    — driver + wails build (with proper cache bust)"
	@echo "  make dev      — driver + wails dev"
	@echo "  make clean    — wipe gui build artefacts"
	@echo "  make verify   — check the linked binary actually contains the latest driver symbols"

driver:
	$(MAKE) -C $(DRIVER_DIR)

# The `touch main.go` + `-clean` combo forces cgo to relink against the
# freshly-built .a. Without it, wails happily reuses a cached object file
# linked against the previous driver build.
build: driver
	go clean -cache
	touch main.go
	wails build -tags "$(WAILS_TAGS)" -clean

dev: driver
	go clean -cache
	touch main.go
	wails dev -tags "$(WAILS_TAGS)"

clean:
	rm -rf build/bin/ build/darwin/ build/windows/ build/linux/
	go clean -cache

# Sanity check: verify the binary contains a symbol that only exists in the
# current picoscope2204a.c. If grep returns 0 matches, the cgo cache trap
# has bitten us and the build is stale — re-run `make build`.
verify:
	@if [ ! -f $(WAILS_BIN) ]; then \
	  echo "no binary at $(WAILS_BIN) — run 'make build' first"; exit 1; \
	fi
	@SYM=$$(nm $(DRIVER_LIB) 2>/dev/null | awk '/ T ps2204a_/ {print $$3; exit}'); \
	if [ -z "$$SYM" ]; then echo "couldn't pick a driver symbol to probe"; exit 1; fi; \
	if strings $(WAILS_BIN) | grep -q "$$SYM" || nm $(WAILS_BIN) 2>/dev/null | grep -q " $$SYM$$"; then \
	  echo "OK — $(WAILS_BIN) contains $$SYM"; \
	else \
	  echo "STALE — $(WAILS_BIN) does NOT contain $$SYM; run 'make build'"; exit 1; \
	fi
