# elfctl

A tiny, dependency-free CLI to reconfigure PCsensor **ElfKey** devices on Linux
— developed against the **MK424BT** 4-key macropad (USB `3553:c140`).

![PCsensor MK424BT 4-key macropad](docs/kb.jpg)

PCsensor's newer devices (VID `3553`) speak the *ElfKey* protocol, which the
popular [`rgerganov/footswitch`](https://github.com/rgerganov/footswitch) tool
does **not** support (it covers `3553:b001` and the older `0c45`/`413d`
pedals). `elfctl` implements the ElfKey protocol directly over raw
`/dev/hidraw` — no `hidapi`, no `libusb`, just a single C file.

## Build

```sh
make
```

## Use

```sh
elfctl list                 # identify the device (model, firmware, enabled layers)
elfctl get                  # show current bindings (all layers)
elfctl get 2                # show just layer 2
elfctl set 1 f13            # set key 1 to F13 (layer 1)
elfctl set 2 ctrl-c         # modifiers: ctrl- shift- alt- gui- (r* = right)
elfctl set 2:1 g            # set layer 2, key 1 to 'g'  (L:K syntax)
elfctl layers               # show how many layers are enabled
elfctl layers 3             # enable all 3 layers (so the S button cycles them)
elfctl switch 2             # switch the active layer (software S-button press)
elfctl save my.conf         # dump config incl. layers to a file (or stdout)
elfctl load my.conf         # apply a config file
elfctl keys                 # list every supported key/modifier name
elfctl --version            # print the elfctl version
```

### Layers (the "S" button)

The MK424BT stores **3 layers** of key bindings. The small round **S button**
cycles the *active* layer among those that are **enabled**, and the backlight
shows which is active: **red = layer 1, green = layer 2, blue = layer 3**.

At the factory only **layer 1 is enabled**, so pressing S appears to do nothing
(it blinks the LED but has nowhere to switch). Enable the others first:

```sh
elfctl layers 3             # enable layers 1+2+3
elfctl set 2:1 f13          # give layer 2 its own bindings
elfctl set 3:1 gui-l
```

`elfctl layers 1` returns to the factory single-layer behaviour (bindings are
preserved in the device; they just stop being reachable via S). Layer/key
addressing is `L:K` everywhere a key is taken; a bare `K` means layer 1.

### Binding syntax

A single key with optional modifier prefixes joined by `-` or `+`:

- Keys: `a`–`z`, `0`–`9`, `f1`–`f24`, `enter` `esc` `tab` `space`
  `backspace` arrows (`up`/`down`/`left`/`right`), `home`/`end`/`pageup`/…,
  and common punctuation names (`minus`, `equal`, `slash`, …).
- Modifiers: `ctrl` `shift` `alt` `gui`(=super/win), each with an `r`-prefixed
  right-hand variant (`rctrl`, `ralt`/`altgr`, …).

Examples: `f13`, `ctrl-c`, `shift-tab`, `gui-l`, `ctrl-shift-esc`.

Run `elfctl keys` for the full, authoritative list (it's generated from the
parser's own tables, so it always matches what `set` accepts).

### Config file format

```
# blanks and #-comments ignored
layers = 3          # how many layers to enable (1..3); optional

key1 = f13          # bare keyN targets layer 1 (backward compatible)
key2 = ctrl-c
key3 = gui-l
key4 = f14

layer2.key1 = g     # higher layers use layerL.keyN
layer2.key2 = h
layer3.key1 = m
```

`elfctl save` emits exactly this format (a `layers = N` line plus one block per
layer), so `save`/`load` round-trips the full multi-layer config.

## Device access (udev)

The config interface's `hidraw` node is root-only by default. Install the
bundled rule to grant the local user access (matched on `3553:c140`):

```sh
sudo make install-udev      # copies udev/60-elfctl.rules, reloads, re-triggers
```

It uses `uaccess` (logind seat ownership) with a `wheel` group fallback.

## Protocol notes

- Config happens on the **OUT-endpoint HID interface** (`bInterfaceNumber == 1`);
  `elfctl` finds it via sysfs, robust against `hidraw` renumbering.
- The interface declares an **unnumbered** 8-byte report, so each `write()`
  is prefixed with a `0x00` report number (kernel-stripped); the protocol's
  own leading `0x01` is the first payload byte.
- Key read response is framed `[len=0x04, count, modifier, keycode]`. After a
  set, the device emits an ACK report (`byte[0]=0x81`) which `elfctl` skips so
  it always verifies against a real read-back.
- **Layers** are addressed as device index `(layer-1)*16 + key` (layer 1 →
  indices 1–4, layer 2 → 17–20, layer 3 → 33–36) using the same set/read-key
  opcodes. Which layers are *enabled* is a separate bitmask set by `0xD2`
  (`0x01`/`0x03`/`0x07`) and read by `0xD3`; the active layer is read by `0xD1`
  and switched by `0xD4`. See `docs/PROTOCOL.md` for the full byte-level writeup.
- `elfctl` only ever emits read/set-key (`0x82`/`0x83`/`0x81`) and the layer
  opcodes (`0xD1`–`0xD4`). It never sends the firmware-flash (`0x20`) or
  set-model (`0x60`) opcodes that also live in this protocol family.

## Status

Single-key and shortcut (modifier+key) bindings work across all **3 layers**
and are verified by read-back; enabling/switching layers works (verified on
hardware — the S button cycles enabled layers, LED red/green/blue). **Macros /
typed strings** (the ElfKey multi-report macro family) and LED-color /
Bluetooth-name / sleep-timeout settings are not implemented yet.

## Releases

The git tag is the source of truth for the version (`v`-prefixed, e.g.
`v0.1.0`); the bare number is mirrored in `ELFCTL_VERSION` in `elfctl.c` and
reported by `elfctl --version`. Releases are cut with the bundled `/release`
Claude Code skill (`.claude/skills/release/`): `/release patch|minor|major`
bumps the latest tag, drags the macro along, pushes `master`, and creates the
GitHub release with an oldest-first changelog.

## Files

- `elfctl.c` — the tool.
- `docs/PROTOCOL.md` — byte-level protocol writeup (opcodes, layer layout, the
  S-button notification, the layer-enable command and its provenance).
- `docs/*.c` — diagnostic / reverse-engineering harnesses, kept as reference for
  how the protocol was worked out. Build any of them with `make <name>` (or all
  with `make diag`); binaries land in the repo root. They are:
  `probe` (read-only model+key probe), `readsweep` (read-only index sweep that
  revealed the layer layout), `layerprobe` (read-only layer-addressing probe),
  `listen` (passive dual-interface listener), `experiment`/`layerwrite`
  (recoverable key-write harnesses), and `layerctl` (drives the `0xD1`–`0xD4`
  layer opcodes).
- `udev/60-elfctl.rules` — device-access rule.
- `.claude/skills/release/` — the `/release` skill.
