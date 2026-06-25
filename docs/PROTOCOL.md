# ElfKey protocol notes — MK424BT (3553:c140)

Reverse-engineered from an MK424BT (firmware V1.1) over raw `/dev/hidraw`.
Everything here is empirically verified except where explicitly marked
**(unverified)** or **(inferred)**.

## USB layout

Two HID interfaces (both mislabel themselves `bInterfaceProtocol = Mouse`):

- **Interface 0** — the typing interface: boot keyboard + consumer/media +
  mouse collection. Standard HID; this is what the host sees as key presses.
  IN endpoint `ep_81`.
- **Interface 1** — vendor collection, usage page `0xff00`. This is the
  **config** interface `elfctl` drives. IN endpoint `ep_84`, OUT endpoint
  `ep_05`. Declares an **unnumbered** 8-byte report (hence the leading `0x00`
  report-number byte on each `write()`, which the kernel strips).

## Config command framing

8-byte payloads on interface 1. `byte[0]` is always `0x01`; `byte[1]` is the
opcode.

| Opcode | Meaning | Notes |
|--------|---------|-------|
| `0x82` | read-key | `byte[3]` = index; response framed `[0x04, count, mod, key]` |
| `0x81` | set-key | header `{01,81,04,idx,…}` then data `{04,01,mod,key,…}`; emits ACK |
| `0x83` | read-model | two chunks: `"MK424BT_"`, then `"V1.1\0…"` |
| `0x20` | firmware-flash | **never emitted by elfctl** |
| `0x60` | set-model | **never emitted by elfctl** |

`elfctl` only ever emits `0x82`/`0x81`/`0x83`.

## Layers — the "S button"

The MK424BT has a physical **S button** (the round button next to key 1; *not*
a power button — the device has no power button, only an OFF/ON battery
slide that matters on battery/BT, not over USB). The manual:

> "Switching button. Press to switch key layers. Factory default condition,
> there's only one layer of keys. Can add second and third layers with the
> software, each key value / layers can be set with different functions."

### What the S button emits

Pressing S emits a **fixed** notification on interface 1's IN endpoint:

```
d1 00 84 00 00 00 00 00
```

- Opcode `0xd1` = "layer switch" notification. Distinct from read/set/ACK.
- It is **NOT** on the keyboard interface — invisible to `evtest`/normal input
  tools. That is why the button "does nothing" from userspace's view.
- The payload is **identical on every press** (verified across 8 presses). It
  does **not** carry the current layer index. So the active layer can be
  *switched* but not *read back* over this channel.

### Layer is indicated by LED colour (not over HID)

Per the ElfKey docs: **red = layer 1, green = layer 2, blue = layer 3**. This is
the only layer-readback available (the `0xd1` notification carries no index).
Verified: at factory default the backlight **stays red** across repeated S
presses — it never goes green/blue — confirming the device is locked to layer 1
and the S "blink" is only a press-acknowledge with no enabled layer to switch
to. Three independent observations agree the device is single-layer until
enabled: (1) no output change across 8 presses, (2) writing a layer-2 binding
did not enable it, (3) LED never leaves red.

### Layer storage layout — `index = layer*16 + key`

Verified by a perturbation test: writing key1 `esc`(0x29)→`a`(0x04) and diffing
a full read sweep before/after. Indices that *changed* were echoing key1 (a
fallback, see below); indices that *stayed put* are genuine storage.

Three real storage blocks, stride 16, six slots each:

```
Layer 1:  idx  1- 6   esc up down enter | e f      (keys 1-4 = user config; 5-6 default)
Layer 2:  idx 17-22   g  h  i  j         | k l      (factory defaults, untouched)
Layer 3:  idx 33-38   m  n  o  p         | q r      (factory defaults, untouched)
```

The factory defaults are 18 consecutive HID keycodes `a`..`r` (0x04–0x15) laid
end-to-end across the three blocks — an unambiguous counting pattern. Key
addressing for `set-key`:

```
Layer 1 keys 1-4 → idx  1- 4   ← the only indices elfctl currently writes
Layer 2 keys 1-4 → idx 17-20
Layer 3 keys 1-4 → idx 33-36
```

**Exactly 3 layers.** Confirmed two ways: (1) the manual says 2nd and 3rd; (2)
after writing key1=`a`, indices 49–54 (a hypothetical layer-4 region) returned
`0x04` — echoing key1 — instead of continuing the default sequence `s,t,u,…`.
Real default slots continue the sequence; phantom slots echo key1. They echoed.

**(Inferred)** The 6 slots per layer (vs. 4 physical keys) are likely shared
firmware with a 6-key sibling, or the S/connect buttons being remappable. The
two extra slots per layer are not exposed by any physical key we can press.

### Fallback-echo behaviour (important gotcha)

The firmware returns **key1's current value** as the response for **any
unmapped index** (0, 7–16, 23–32, 39+, and large/out-of-range indices). This is
a read-time fallback, not stored data. Before the perturbation test these all
read as `esc` and looked like real "empty = esc" slots; after key1→`a` they all
read `a`, exposing them as echoes. **Do not mistake echoed indices for
storage** — only indices that ignore a key1 change are real.

## Enabling / switching layers — the `0xD0`-family opcodes (SOLVED)

The manual says layers are *"added with the software"*, yet the layer-2/3
storage already physically exists (pre-filled with the factory `a..r` pattern).
The reconciliation — earlier inferred, now **verified** — is that what the
software "adds" is a separate **enabled-layers bitmask**, defaulting to `0x01`
(layer 1 only). The S button only cycles among *enabled* layers, so at the
factory default pressing S blinks the backlight but never changes layer.

Four opcodes control this. Verified two ways: against the device, and against
the official ElfKey configurator's own source (an Electron app; constants named
`readFuncLayerCmd` / `enableFuncLayerCmd` / `readEnabledFuncLayerCmd` /
`switchFuncLayerCmd`).

| Opcode | Name | Send | Response | Meaning |
|--------|------|------|----------|---------|
| `0xD1` | read active layer  | `01 D1 00 …` | `d1 00 <active>` | active layer is **byte[2]** |
| `0xD2` | enable layers      | `01 D2 <mask> …` | (ACK) | set enabled-layers bitmask |
| `0xD3` | read enabled mask  | `01 D3 00 …` | `d3 <mask> 00` | enabled mask is **byte[1]** |
| `0xD4` | switch layer       | `01 D4 <layer> …` | (ACK) | software S-button press |

(Response framing differs per opcode — `0xD1`'s data is at byte[2], `0xD3`'s at
byte[1]; both echo the opcode in byte[0]. Observed empirically, not from the
app, which abstracts the offsets.)

**Bitmask** (`byte[2]` of `0xD2`): bit0 = L1 (always on), bit1 = L2, bit2 = L3.
Only three values are used: `0x01` = L1 only (factory default), `0x03` = L1+L2,
`0x07` = all three. `elfctl layers N` writes `(1<<N)-1` and reads back via `0xD3`
to verify.

**Wire framing for our PID:** the app sends a leading report-ID byte only for a
specific allow-list of `3553` PIDs that does **not** include `0xC140`, so for the
MK424BT the report ID is `0` — i.e. exactly the `00 01 D2 …` framing `elfctl`'s
`write_cmd()` already uses. No special handling needed.

Note `0xD1` is the same opcode the S button emits as a notification
(`d1 00 84 …`). On a button press byte[2] carried `0x84` (an event marker); when
*queried* it returns the active layer number in byte[2]. Same opcode, two
directions.

### Verified on hardware

With all three layers enabled (`01 D2 07`), pressing S cycled the LED
**red → green → blue** and the four keys emitted layer 1 (`esc up down enter`),
layer 2 (`g h i j`), then layer 3 (`m n o p`) respectively — exactly the stored
bindings. `01 D2 01` returns to factory single-layer behaviour (bindings are
retained, just unreachable via S).

### Earlier dead-end (recorded so it isn't re-tried)

Before finding `0xD2`, we tested whether merely *editing* a layer-2 binding
would enable the layer: wrote idx17 `g`(0x0a)→`x`(0x1b). The write stored and
read back fine with no bleed into layer 1, but S still would not reach layer 2.
So enabling a layer is independent of editing its bindings — it requires `0xD2`.
(idx17 was restored to 0x0a.)

## Diagnostic tools (this directory)

These `.c` files live alongside this doc as reverse-engineering artifacts. Build
any with `make <name>` from the repo root (or `make diag` for all); binaries
land in the repo root. All read-only except where noted; none emit
firmware-flash/set-model.

- `probe.c` — minimal read-model + read-key 1..4 (read-only).
- `readsweep.c` — wide read-only sweep of the index space; the tool that
  revealed the stride-16 layer blocks and the fallback-echo behaviour.
- `layerprobe.c` — read-only tests of layer-addressing hypotheses (extended
  index vs. selector byte). Ruled out a layer-selector byte in the read command.
- `listen.c` — passive listener on BOTH interfaces (read-only); captured the
  `d1 00 84` S-button notification.
- `experiment.c` — recoverable single-key write harness (writes key1, restores).
- `layerwrite.c` — recoverable arbitrary-index write harness (used to prove the
  `(layer<<4)|key` map and rule out edit-enables-layer); reads/restores.
- `layerctl.c` — drives the layer opcodes `0xD1`–`0xD4`: read active layer / read
  enabled mask (read-only), enable a bitmask, switch layer. The layer logic here
  is what got folded into `elfctl`'s `layers`/`switch` commands.

The `0xD1`–`0xD4` opcodes are documented in the official configurator's source,
so emitting them is not a blind fuzz. `0xD2`/`0xD4` change state but are
reversible (`0xD2 01` restores factory single-layer; bindings are never lost).
