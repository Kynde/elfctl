# ElfKey opcode inventory

Every command constant defined by the official ElfKey configurator (v3.0.0,
`out/main/index.js`), converted to hex. This is a **reference index**, not a
protocol spec: it tells you an opcode exists and what the app calls it. Payload
layouts live in [PROTOCOL.md](PROTOCOL.md), and only for the families we have
actually worked out.

Recorded because the app is the only public source for these, it is not
guaranteed to stay downloadable, and newer builds ship the main process as V8
bytecode (see [AGENTS.md](../AGENTS.md) for how to obtain 3.0.0). Nothing here
is verified on hardware unless PROTOCOL.md says so — **most of it is not**, and
much of it belongs to other devices in the PCsensor family, not the MK424BT.

Framing is `01 <opcode> …` over 8-byte reports on the config interface, with the
one exception flagged below.

## Never emit

| Opcode | App constant | Why |
|--------|--------------|-----|
| `0x20` | `updateFirmwareCmd` | firmware flash |
| `0x22` | `updateCrcCmd` | firmware CRC — part of the flash flow |
| `0x60` | `setModelCmd` | rewrites the device's model identity |

`0x60` sits directly below `0x62` (set-led). Be exact when writing LED code.

## Implemented by elfctl

| Opcode | App constant | Meaning |
|--------|--------------|---------|
| `0x81` | `setKeyValueCmd` | set key binding (also `gameCmd`, same opcode) |
| `0x82` | `readKeyCmd` | read key binding |
| `0x83` | `readModelCmd` | read model + firmware string |
| `0xC0` | `setMacroCmd` | define macro |
| `0xC1` | `readMacroListCmd` | read macro slot |
| `0xC2` | `deleteMacroCmd` | clear macro slot |
| `0xD1` | `readFuncLayerCmd` | read active layer |
| `0xD2` | `enableFuncLayerCmd` | set enabled-layer bitmask |
| `0xD3` | `readEnabledFuncLayerCmd` | read enabled-layer bitmask |
| `0xD4` | `switchFuncLayerCmd` | switch active layer |
| `0x63` | `readLedCmd` | read backlight settings (read-only; `light get`) |

## Decoded but not implemented

See PROTOCOL.md "LEDs / backlight" for the payloads.

| Opcode | App constant | Meaning |
|--------|--------------|---------|
| `0x62` | `setLedCmd` | write backlight settings |
| `0xA3` | `setBorderLedCmd` | set border-LED mode |
| `0xA4` | `readBorderLedCmd` | read border-LED mode |
| `0xD5` | `checkLedCmd` | `01 D5 FF FE …` — purpose unknown |

`0xA3`/`0xA4` are shared: `setLightModeCmd` and `setLightFlashCmd` are also
`0xA3`, and `readLightFlashCmd` is also `0xA4`, with **different payloads** for
device classes the MK424BT is not in. Check the capability predicate first.

## Not investigated

Present in the app; payloads unknown; **most target other devices**. Listed so
nobody has to re-extract the bundle to find out whether a feature exists.

| Opcode | App constant | Apparent purpose |
|--------|--------------|------------------|
| `0x30` | `setOneClickOpenCmd` | "one-click open" — **note `byte[0]` is `08`, not `01`** |
| `0x80` | `flashCmd` | `01 80 80 01 …` — unknown; *not* the firmware flash |
| `0x85` | `setTriggerCmd` | trigger mode (footswitch family) |
| `0x86` | `readTriggerCmd` | ” |
| `0x87` | `setSensitivityCmd` | sensitivity (sensor devices) |
| `0x88` | `readSensitivityCmd` | ” |
| `0x8A` | `readManufactureDateCmd` | manufacture date |
| `0xA7` | `readBTC24BatteryCmd` | battery level (BTC24 devices) |
| `0xA8` | `setSleepTimeCmd` / `setTimeToSleepCmd` | sleep timeout |
| `0xA9` | `readSleepTimeCmd` / `readTimeToSleepCmd` | ” |
| `0xAA` | `setDeviceModeCmd` | device mode (USB / 2.4G / BT) |
| `0xAB` | `readDeviceModeCmd` | ” |
| `0xAC` | `initBtCmd` | Bluetooth init / re-pair |
| `0xB0` | `setBTNameCmd` | Bluetooth name |
| `0xB1` | `readBTNameCmd` | ” |
| `0xC3` | macro-record family | `01 C3 00 01/02/03/04` = enter / pressed / released / validate |
| `0xC7` | `setThresholdCmd` | threshold (sensor devices) |
| `0xC8` | `readThresholdCmd` | ” |
| `0xCF` | `setSocketCmd` | "socket" |
| `0xD0` | `readSocketCmd` | ” |
| `0xE0` | `setLowPowerAlarmCmd` | low-power alarm |
| `0xE1` | `readLowPowerAlarmCmd` | ” |
| `0xF1` | `setLongStringKeyValueCmd` | long/typed-string key value |

The sleep-timeout and Bluetooth-name settings the README lists as unimplemented
are `0xA8`/`0xA9` and `0xB0`/`0xB1` respectively.

## Reading this safely

Reads are safe; writes are not, and an opcode existing here is **not** evidence
the MK424BT implements it. Before touching any of these, confirm the model is in
the corresponding `has*()` predicate in the app's main bundle — the configurator
is shared across the whole PCsensor range and most of this table is for other
hardware. Then follow the protocol-safety rules in [AGENTS.md](../AGENTS.md):
never fuzz, always read back.
