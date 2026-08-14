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

That's the whole dependency story: a C compiler. The binary lands in the repo
root.

## Use

```sh
elfctl list                 # identify the device (model, firmware, enabled layers)
elfctl get                  # show current bindings (all layers)
elfctl get 2                # show just layer 2
elfctl set 1 f13            # set key 1 to F13 (layer 1)
elfctl set 2 ctrl-c         # modifiers: ctrl- shift- alt- gui- (r* = right)
elfctl set 2:1 g            # set layer 2, key 1 to 'g'  (L:K syntax)
elfctl set 1 macro:3        # run macro slot 3 when key 1 is pressed
elfctl layers               # show enabled layers + which one is currently active
elfctl layers 3             # enable all 3 layers (so the S button cycles them)
elfctl switch 2             # switch the active layer (software S-button press)
elfctl macro list           # show the 8 macro slots
elfctl macro set 3 paste 'ctrl-c, 50ms, ctrl-v'   # define a macro
elfctl macro delete 3       # clear a macro slot
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

### Macros

The device has **8 macro slots**, each a named sequence of keystrokes with
optional per-step timing. Define a macro, then point any key at it:

```sh
elfctl macro set 1 paste 'ctrl-c, 50ms, ctrl-v'   # slot 1, named "paste"
elfctl set 3 macro:1                              # key 3 now plays it
elfctl macro list                                 # review all slots
```

A sequence is a comma-separated list of chords (same syntax as a binding). A
bare `<n>ms` token between chords sets the delay *after* the previous step
(default 10 ms). Macros are stored on the device; a key links to one by id, so
several keys can share a macro.

> Macro **playback modes** (repeat-while-held, toggle) and per-step
> press/release control are not exposed yet — every step is an atomic keypress.
> See the status note below.

### Config file format

```
# blanks and #-comments ignored
layers = 3          # how many layers to enable (1..3); optional

key1 = f13          # bare keyN targets layer 1 (backward compatible)
key2 = ctrl-c
key3 = gui-l
key4 = macro:1      # a key may link to a macro slot

layer2.key1 = g     # higher layers use layerL.keyN
layer2.key2 = h
layer3.key1 = m
```

`elfctl save` emits exactly this format (a `layers = N` line plus one block per
layer), so `save`/`load` round-trips the full multi-layer config including
`macro:N` key links. Macro *definitions* themselves are managed with
`elfctl macro set`/`delete`, not the config file.

## Device access (udev)

The config interface's `hidraw` node is root-only by default. Install the
bundled rule to grant the local user access (matched on `3553:c140`):

```sh
sudo make install-udev      # copies udev/60-elfctl.rules, reloads, re-triggers
```

It uses `uaccess` (logind seat ownership) with a `wheel` group fallback.

## Status

Working and verified by read-back on real hardware:

- single-key and modifier+key bindings across all **3 layers**,
- enabling / switching layers (the S button cycles enabled layers; LED
  red/green/blue),
- **macros**: 8 named slots, multi-step sequences with per-step delays, and
  linking a key to a macro.

Not implemented yet: macro playback modes (repeat/toggle) and per-step
press/release control, typed-string macros, and the LED-color / Bluetooth-name
/ sleep-timeout settings that also live in the ElfKey protocol family.

## Documentation

- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — the byte-level ElfKey protocol:
  opcodes, the layer index layout, the S-button notification, the layer-enable
  command, and the macro (`0xC0`) family. The reference for how the device
  actually talks.
- **[AGENTS.md](AGENTS.md)** — guide for working *on* elfctl: repo layout, the
  reverse-engineering harnesses in `docs/`, the protocol-safety rules, and how
  releases are cut.
