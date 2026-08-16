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

Other opcode families are documented in their own sections below: layers
(`0xD1`–`0xD4`), macros (`0xC0`–`0xC2`), and LEDs (`0x62`/`0x63`, `0xA3`/`0xA4`).

`elfctl` emits `0x81`/`0x82`/`0x83`, `0xC0`/`0xC1`/`0xC2`, `0xD1`–`0xD4` and the
read-only `0x63`, and nothing else. The LED **write** opcode `0x62` is
documented but deliberately not implemented — nothing in this repo emits it.

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
the only *at-a-glance* indicator, and the only one available on this channel —
the `0xd1` notification carries no index. (The active layer **can** be read back
over HID, but only by *querying* `0xD1` on the config interface; see the
`0xD0`-family section below.)
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
| `0xD1` | read active layer  | `01 D1 00 …` | `d1 <active-1> <mask>` | active at **byte[1]**, 0-based |
| `0xD2` | enable layers      | `01 D2 <mask> …` | (ACK) | set enabled-layers bitmask |
| `0xD3` | read enabled mask  | `01 D3 00 …` | `d3 <mask> <active-1>` | mask at **byte[1]** |
| `0xD4` | switch layer       | `01 D4 <layer> …` | (ACK) | software S-button press, **1-based** |

Both read opcodes return **both** fields, in mirrored order — `0xD1` gives
`<active> <mask>`, `0xD3` gives `<mask> <active>` — with the opcode echoed in
byte[0]. Observed empirically, not from the app, which abstracts the offsets.

**Two off-by-one traps, verified across all three layers on firmware V1.1:**

```
0xD4 switch to layer 1/2/3  →  send 1 / 2 / 3      (1-based)
0xD1 response byte[1]       →  reads 00 / 01 / 02  (0-based)
```

So the active layer is 0-based when read but 1-based when written. Reading
byte[2] of the `0xD1` response instead of byte[1] yields the *mask*, which
coincidentally equals the layer number when one layer is enabled (`0x01`→1) —
a bug that hides itself at the factory default. `elfctl` did exactly this until
it was caught by noticing the value tracked the mask instead of the switches.

`0xD4` **refuses** a switch to a layer that is not enabled: the write is ACKed
but the active layer is unchanged, so verify with a `0xD1` read afterwards
rather than trusting the ACK.

**Bitmask** (`byte[2]` of `0xD2`): bit0 = L1 (always on), bit1 = L2, bit2 = L3.
Only three values are used: `0x01` = L1 only (factory default), `0x03` = L1+L2,
`0x07` = all three. `elfctl layers N` writes `(1<<N)-1` and reads back via `0xD3`
to verify.

**Wire framing for our PID:** the app sends a leading report-ID byte only for a
specific allow-list of `3553` PIDs that does **not** include `0xC140`, so for the
MK424BT the report ID is `0` — i.e. exactly the `00 01 D2 …` framing `elfctl`'s
`write_cmd()` already uses. No special handling needed.

Note `0xD1` is the same opcode the S button emits as a notification
(`d1 00 84 …`). The two directions do **not** share a payload layout: on a
button press byte[2] carries `0x84` (an event marker, not a mask), whereas a
*query* answers `d1 <active-1> <mask>`. Same opcode, two framings — don't parse
an unsolicited notification with the query's offsets.

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

## Macros — the `0xC0`-family opcodes

The MK424BT supports **macros**: named, multi-step sequences with per-step
timing, stored in a separate **macro table** and referenced from a key slot.
This is a different mechanism from the single-chord `set-key` binding — a key
does not *contain* a macro, it *points at* one.

Source: the official ElfKey configurator (`out/main/index.js`, functions
`setMacro`/`readMacroList`/`deleteMacro`, opcode constants in its `constants`
module) **and** rgerganov's `footswitch` driver, which speaks the same PCsensor
protocol family (its VID/PID table includes `3553:b001`, a sibling of our
`3553:c140`). The string/key encodings match the ones `elfctl` already uses.
**(Verification status: opcodes and framing are documented in two independent
sources; the exact `type`/`mode` enum values are decoded from the app and want
one hardware read-back to pin down — see `macroprobe.c`.)**

### Two pieces: a macro table, and a key→macro link

| Opcode | Name | Send | Meaning |
|--------|------|------|---------|
| `0xC0` | set-macro    | `01 C0 <lenHi> <lenLo> <id> …` + data reports | define/overwrite macro `<id>` (id in **byte[4]**) |
| `0xC1` | read-macro   | `01 C1 00 <id> …` | read macro `<id>` (id in **byte[3]**) |
| `0xC2` | delete-macro | `01 C2 00 <id> …` | clear macro `<id>` (id in **byte[3]**) |

The id-byte positions differ between set (byte[4]) and read/delete (byte[3]) —
matches the app's `setMacroCmd[4]` vs. `readMacroListCmd[3]`/`deleteMacroCmd[3]`.
**Verified on hardware:** querying `0xC1` with the id in byte[3] returns the
all-`0xff` empty-slot sentinel for an undefined slot (the app's
`every(v === 255)` check); putting the id in the wrong byte returns a generic
malformed-command reply instead. Unlike the `0xD` layer family, `0xC1` does
**not** echo the opcode — the response is the name/action stream directly.

**8 macro slots** (`id` 1..8) on the MK424BT. The app caps at 6 only for the
"short macro" device class, which this device is **not** (`hasShortMacro()` is
false whenever `hasMacro()` is true, and `MK424` is in `hasMacro()`).

A **key slot points at a macro** by being written with the ordinary set-key
command (`0x81`) but with **key-type `0x0A`** (macro) and the macro id in place
of the keycode. The app's `saveMacro` does exactly:

```
header : 01 81 08 <keyNum> 00 00 00 00     (note byte[2]=0x08, not 0x04)
data   : 08 0A <macroId> 00 00 00 00 00
```

So `elfctl`'s `data[1]` byte — which the code currently calls "count" and always
sets to `0x01` — is really the **key TYPE**. The app's `readKey` switches on it:
`1`=single key, `2`=mouse, `7`=media, `0x0A`=macro link (others: game, MIDI,
loop/double-trigger). Reading a macro-linked key back yields type `0x0A` with the
id in byte[3]; that is how `get`/`save` can stay honest about macro bindings.

### Macro wire format (`0xC0` write)

Header then a stream of 8-byte data reports:

```
header        : 01 C0 <lenHi> <lenLo> <id> 00 00 00
4 × report    : 32 bytes of NAME (UTF-8, NUL-padded)
1 × report    : <lenHi> <lenLo> <mode> <action bytes 0..4>
N × report    : remaining action bytes, 8 per report
```

`len` is `actionBytes + 3` as a **16-bit big-endian** count (so macros can be far
longer than one report; the real per-device ceiling is unconfirmed). `mode` is
the macro playback mode (once / repeat-while-held / toggle — exact values
**unverified**, decode with `macroprobe.c`).

Each **action** is a length-prefixed record. Two kinds:

```
keyboard (len 7): 07 01 <modMask> <keyCode> <type> <delayHi> <delayLo>
mouse    (len 9): 09 02 <button> <x> <y> <wheel> <type> <delayHi> <delayLo>
```

- **One key OR one modifier per action**, not a combined chord: the encoder emits
  `<modMask>,0` for a modifier-only step and `0,<keyCode>` for a key step. A chord
  like `ctrl-c` is therefore several actions (press ctrl, press c, release c,
  release ctrl) sequenced via the `type` field.
- **`modMask`** bit order is identical to `elfctl`'s `MODS` table
  (bit0=ctrl … bit7=rgui). **`keyCode`** is the same HID usage table
  `keyname_to_code()` already implements.
- **`type`** = per-action press / release / click (default `1`). Exact values
  **unverified**. The live "record macro" flow uses a parallel `0xC3` family
  (`enter`=`01 C3 00 01`, `pressed`=`…02`, `released`=`…03`, `validate`=`…04`);
  `type` is the stored-form equivalent.
- **`delay`** is per-action milliseconds, **16-bit big-endian**. This pad stores
  real inter-step timing, not just a keystroke list.

### Reading back (`0xC1`)

Response is the inverse of the write: first report carries the start of the
32-byte name, three more complete it, then a report with `<lenHi> <lenLo> <mode>`
and the first action bytes, then the rest 8-per-report. `readMacroList` treats a
slot as empty when its bytes are all `0x00` or all `0xFF`. Actions are walked by
their leading length byte until a `0` length terminates the list.

### Safety

`0xC0`/`0xC1`/`0xC2` are documented opcodes (two independent sources), not blind
fuzz. `0xC1` is read-only. `0xC0`/`0xC2` change state but are fully recoverable:
read a slot first, and a macro can be rewritten or deleted with no effect on key
bindings or other slots. Linking a key to a macro is a normal `0x81` write and is
reversible by writing any ordinary binding back.

## LEDs / backlight — the `0x62`/`0x63` and `0xA3`/`0xA4` opcodes

The MK424BT's backlight is configurable: effect, RGB colour and a separate
"border" LED. This is distinct from the red/green/blue **layer indication**
described above — the layer colour is firmware-driven, while these settings are
stored device state.

**(Verification status: the read side (`0x63`) is VERIFIED on hardware** —
MK424BT firmware V1.1, stable across repeated reads, every hard-coded constant
landing exactly where the app's source says it should. **The write side
(`0x62`/`0x63`) has never been emitted**, and the `0xA4` border-LED reply is not
yet disambiguated — see below. Re-run `ledprobe` (read-only) before acting on
any of it.)

### Source

The opcodes come from the official ElfKey configurator. Version **3.0.0**
(macOS build, `ElfKey-3.0.0-mac-x64.dmg` → `Contents/Resources/app.asar` →
`out/main/index.js`) ships the main process as **plain JavaScript**. The current
**3.3.5** build compiles it to V8 bytecode (`out/main/index.jsc` + a
`bytecode-loader.js` shim), so the constants are no longer directly readable
there — though the bytecode's constant pool still leaks the *names*
(`readLedCmd`, `setLedCmd`, `setLightModeCmd`, …), which is how 3.0.0 was
identified as worth fetching. **Use 3.0.0 for protocol work.**

### Which family applies to this device

The app has three mutually-exclusive lighting capability predicates. Their
bodies decide by model string, and MK424 lands in exactly one:

```js
hasLed()        { … || this.model.includes("MK424"); }          // ← TRUE for us
hasLightMode()  { return this.model.includes("MK321U") && this.version < 1.7; }
hasLightFlash() { return … && !this.hasLed() && … ; }            // excluded by hasLed()
```

So the MK424BT uses the **`hasLed()`** family — `0x62`/`0x63` plus the border-LED
pair `0xA3`/`0xA4`. The `setLightModeCmd`/`setLightFlashCmd` constants (also
`0xA3`, with different payloads) belong to *other* models and must not be used
here. Note `0x62` sits next to the dangerous `0x60` set-model opcode — be exact.

| Opcode | Name | Send | Meaning |
|--------|------|------|---------|
| `0x63` | read-led        | `01 63 <slot> …` | read LED settings; `readLed()` uses slot `1`. **Implemented** as `elfctl light get` |
| `0x62` | set-led         | `01 62 0D 01 …` + 2 data reports | write LED settings |
| `0xA4` | read-border-led | `01 A4 00 …` | border-LED mode; reply offset ambiguous, see below |
| `0xA3` | set-border-led  | `01 A3 04 <mode> …` | set border-LED mode |

`0x63` byte[2] is a **slot index**, not a flag: the app's per-key scheme reader
loops `readLedCmd[2] = i+1` over the key count, and uses slots 10–12 for the
sleep-LED scheme. The simple `readLed()` path only ever reads slot 1.

### Write payload (`0x62`)

Header, then two data reports — a single length-prefixed 13-byte record split
across them, the same style as the macro action records:

```
header  : 01 62 0D 01 00 00 00 00
data 1  : 0D <effect> <colorMode> <r> <g> <b> 01 E8
data 2  : 03 E8 03 <mode> <flashes> 00 00 00
          → then read one ACK
```

Concatenated, the record is:

```
0D <effect> <colorMode> <r> <g> <b> 01 E8 03 E8 03 <mode> <flashes>
```

`0x0D` = 13 = the record length. The two `E8 03` pairs are **(inferred)** 16-bit
little-endian `1000` — plausibly timing/period in ms; the app hard-codes them and
never exposes them, so this is a guess. The `01` before them is unidentified.

The app follows every `setLed` with a border-LED write (`01 A3 04 <mode>`) and a
second ACK read, so the two settings are always written as a pair. Since the
border-LED offset is still unresolved (below), a first write experiment should
send only the `0x62` part and leave `0xA3` alone.

### Implementing the write — read this first

Everything above is decoded, but **nothing on the write path has been observed
on the wire**. Four things that will bite:

- **The −1 rule is a READ artifact — do NOT apply it to writes.** node-hid
  prepends a report-ID byte on *reads* only; the app's command arrays are sent
  verbatim. The write bytes above are already correct as-is. (elfctl's
  `write_cmd()` prepends the `0x00` report byte the kernel strips, which is the
  same framing the `0xD2` section documents for this PID.)
- **Header `byte[4]` is unverified.** `setLedCmd` is a shared mutable array in
  the app: `setLedScheme()` sets `[4] = keyIndex`, while `setLed()` sets only
  `[2]` and `[3]` — so whatever `[4]` holds is leftover from an earlier call,
  and is `0` on a fresh run. Send `0`; don't assume it is meaningful.
- **The ACK is unknown.** The app does one `read()` after the data reports and
  ignores the result. We have never seen what comes back, so don't gate success
  on a particular reply — verify by re-reading with `0x63` instead.
- **`colorMode` and `mode` must be preserved, not defaulted.** See the enum
  notes below: the app's `3`/`1` are *fallback placeholders*, not this device's
  values.

The safe way to attempt it, in order:

1. `elfctl light get` (or `ledprobe`) and **write the record down**.
2. Emit `0x62` with the byte-identical record. Nothing should visibly change.
3. Re-read. If it round-trips unchanged, the write framing is correct.
4. Only then change one field (`effect` is the safest — it is the one the app's
   own UI exposes), re-read to confirm, and restore.

**Known-good record for this device** (factory state, MK424BT V1.1) — restore by
writing exactly this back:

```
header : 01 62 0D 01 00 00 00 00
data 1 : 0d 03 02 00 ff 00 01 e8
data 2 : 03 e8 03 02 01 00 00 00
```

The write is recoverable in the sense that it is a plain settings overwrite —
it touches no key binding, macro or layer state, and the record above restores
it. It is not a firmware operation.

### Read payload (`0x63`) — verified

The device replies with **exactly the record it accepts on write**, same two
reports, no shift:

```
report 0 : 0D <effect> <colorMode> <r> <g> <b> 01 E8
report 1 : 03 E8 03 <mode> <flashes> 00 00 00
```

Observed on an MK424BT (stable across repeated reads):

```
report 0: 0d 03 02 00 ff 00 01 e8
report 1: 03 e8 03 02 01 00 00 00
→ effect=3 (breathing)  colorMode=2  color=#00ff00  mode=2  flashes=1
  byte[6]=1  timings=1000,1000
```

Both `E8 03` pairs decode to exactly **1000** as 16-bit little-endian,
confirming that reading (they are still hard-coded by the app and their meaning
is unconfirmed — plausibly a period in ms). `byte[6]` is `1`, matching the
constant the app writes.

#### The off-by-one against the app's source (important)

The app's `readLed()` takes `effect` from `data1[2]`, `colorMode` from
`data1[3]`, RGB from `data1[4..6]`, `mode` from `data2[4]` and `flashes` from
`data2[5]` — **every one of those is one higher than the offsets above**.
node-hid hands the app a leading report-ID byte, which the kernel strips from
`hidraw` reads. So:

> **app read index N == our index N−1.** Translate every read offset from the
> configurator's source by −1 before using it here.

Getting this wrong is not a crash, it is a *plausible-looking* misparse: the
naive reading of the same bytes yields `effect=2`, `colorMode=0` and
`color=#ff0001` — all valid-looking values, and all wrong. The giveaway is the
leading `0x0D` length byte and the `01 E8 03 E8 03` constant run, which only
line up under the correct alignment.

### Border LED (`0xA4`) — unresolved

The reply echoes the opcode in `byte[0]`, like the `0xD3` family:

```
a4 82 03 00 00 00 00 00
```

Applying the −1 rule to the app's `borderLedData[2]` gives **`byte[1]` = 0x82 =
130 = "colorful breath"**. But `byte[2]` = 3 is *also* a valid enum member
("flashes when key is pressed"), so the two candidates cannot be told apart from
one sample. Do not implement border-LED support until this is settled — e.g. by
changing the setting in the official app and re-reading, or by a usbmon capture.

A first read after another command returned `a4 82 00 32 34 42 54 5f`, whose
tail is ASCII `24BT_` — stale model-string bytes left in the reply buffer. The
trailing bytes of a short reply are **not** zeroed reliably; drain before
reading and ignore anything past the documented fields.

### Enum values

From the configurator's own UI (English strings quoted verbatim):

| Field | Value | Meaning |
|-------|-------|---------|
| `effect` | 1 | "Turn off the light" |
| | 2 | "Corresponding key pressed blinks" |
| | 3 | "Breathing light mode" |
| | 5 | "Flashes" |
| `flashes` | 1 | "Multicolor (high power consumption)" |
| | 0 | single colour (use `color`) |
| `borderLed` | 0 | off |
| | 1 | "Always on (high power consumption)" |
| | 2 | breathing |
| | 3 | "Flashes when key is pressed" |
| | 130 | "Colorful breath" |

`color` is plain 24-bit RGB (`r`,`g`,`b` bytes). `colorMode` and `mode` are read
and written back **unchanged** by the app — its UI never edits them, so their
semantics are **unknown**. Always preserve whatever the device reports.

The `colorMode: 3` / `mode: 1` values in the app's source are the **fallback
placeholders** it uses when a read fails, *not* device defaults — this MK424BT
reports `colorMode=2, mode=2`. Writing 3/1 because the app's source shows them
would clobber real state.

### What this does *not* explain

The **charging marquee** (a red running light while charging over USB, per the
MK424 manual) and the full-charge breathing indicator are firmware status
behaviours tied to power state. Nothing in the `hasLed()` path selects them, and
the app exposes no control for them. Treat them as **not configurable** unless
evidence turns up otherwise — a marquee appearing on USB is the charger
indicator, not a setting that got changed.

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
- `macroprobe.c` — **read-only** dump of all 8 macro slots via `0xC1`: prints raw
  reports plus a decode of name / mode / each action (mod, key, type, delay). Used
  to confirm the `0xC0`-family framing and pin down the `type`/`mode` enum values
  before they were folded into `elfctl`'s `macro` commands.
- `ledprobe.c` — **read-only** dump of the LED settings via `0x63` (key LED) and
  `0xA4` (border LED): prints the raw reports plus a decode of
  effect / colour / mode / flashes. Confirmed the `0x63` record on hardware and
  exposed the −1 read-offset rule; the `0xA4` border-LED offset is still open.
  See [LEDs / backlight](#leds--backlight--the-0x620x63-and-0xa30xa4-opcodes).

The `0xD1`–`0xD4` opcodes are documented in the official configurator's source,
so emitting them is not a blind fuzz. `0xD2`/`0xD4` change state but are
reversible (`0xD2 01` restores factory single-layer; bindings are never lost).
