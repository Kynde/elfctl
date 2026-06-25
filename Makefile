CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

BIN      = elfctl
EXTRA    = probe experiment readsweep layerprobe listen layerwrite layerctl macroprobe

.PHONY: all help clean install uninstall install-udev diag

all: $(BIN)  ## Build elfctl (default)

help:  ## List available targets
	@echo "elfctl — make targets:"
	@grep -E '^[a-zA-Z0-9_-]+:.*## ' $(MAKEFILE_LIST) \
		| sort \
		| awk -F':.*## ' '{printf "  \033[1m%-14s\033[0m %s\n", $$1, $$2}'

$(BIN): $(BIN).c
	$(CC) $(CFLAGS) -o $@ $<

# Diagnostic helpers (sources live in docs/ — they're reference artifacts now),
# built on demand to the repo root; not part of `all`.
diag: $(EXTRA)  ## Build all diagnostic helpers

$(EXTRA): %: docs/%.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(BIN)  ## Install elfctl to $(BINDIR)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:  ## Remove the installed elfctl
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

install-udev:  ## Install the device-access udev rule (needs root)
	install -Dm644 udev/60-elfctl.rules /etc/udev/rules.d/60-elfctl.rules
	udevadm control --reload
	udevadm trigger --subsystem-match=hidraw

clean:  ## Remove built binaries
	rm -f $(BIN) $(EXTRA) *.o
