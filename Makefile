CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

BIN      = elfctl
EXTRA    = probe experiment

.PHONY: all help clean install uninstall install-udev

all: $(BIN)  ## Build elfctl (default)

help:  ## List available targets
	@echo "elfctl — make targets:"
	@grep -E '^[a-zA-Z0-9_-]+:.*## ' $(MAKEFILE_LIST) \
		| sort \
		| awk -F':.*## ' '{printf "  \033[1m%-14s\033[0m %s\n", $$1, $$2}'

$(BIN): $(BIN).c
	$(CC) $(CFLAGS) -o $@ $<

# Diagnostic helpers, built on demand (not part of `all`).
probe: probe.c  ## Build the read-only protocol probe
	$(CC) $(CFLAGS) -o $@ $<

experiment: experiment.c  ## Build the protocol reverse-engineering scratch harness
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
