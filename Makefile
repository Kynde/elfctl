CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

BIN      = elfctl
EXTRA    = probe experiment

.PHONY: all clean install uninstall install-udev

all: $(BIN)

$(BIN): $(BIN).c
	$(CC) $(CFLAGS) -o $@ $<

# Diagnostic helpers, built on demand (not part of `all`).
probe: probe.c
	$(CC) $(CFLAGS) -o $@ $<

experiment: experiment.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

# Install the udev rule that grants the local user access to the device.
install-udev:
	install -Dm644 udev/60-elfctl.rules /etc/udev/rules.d/60-elfctl.rules
	udevadm control --reload
	udevadm trigger --subsystem-match=hidraw

clean:
	rm -f $(BIN) $(EXTRA) *.o
