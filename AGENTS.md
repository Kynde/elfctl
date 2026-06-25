# Working on elfctl

Notes for anyone — human or agent — hacking on this repo. End-user docs live in
[README.md](README.md); the byte-level protocol lives in
[docs/PROTOCOL.md](docs/PROTOCOL.md). This file is about the codebase and the
conventions for changing it safely.

## Layout

- `elfctl.c` — the entire tool, one C file, no dependencies beyond libc. Built
  with `make` (`cc -O2 -Wall -Wextra`); keep it warning-clean.
- `docs/PROTOCOL.md` — the reverse-engineered ElfKey protocol writeup. Update it
  in the same change whenever you learn or rely on a new wire detail.
- `docs/*.c` — diagnostic / reverse-engineering harnesses (see below).
- `udev/60-elfctl.rules` — device-access rule (`uaccess` + `wheel` fallback).
- `.claude/skills/release/` — the `/release` skill (see Releases).

## Build & diagnostics

`make` builds `elfctl`. The `docs/*.c` harnesses are reference artifacts for how
the protocol was worked out; build any with `make <name>` (or all with
`make diag`). Binaries land in the repo root and are gitignored.

| Tool | What it does | Writes? |
|------|--------------|---------|
| `probe` | read-only model + key 1–4 probe | no |
| `readsweep` | read-only sweep of the index space; revealed the stride-16 layer blocks and the fallback-echo behaviour | no |
| `layerprobe` | read-only tests of layer-addressing hypotheses | no |
| `listen` | passive dual-interface listener; captured the S-button `0xd1` notification | no |
| `macroprobe` | read-only dump of the 8 macro slots via `0xC1` | no |
| `experiment` | single-key write harness (writes, then restores) | recoverable |
| `layerwrite` | arbitrary-index write harness; proved the `(layer<<4)|key` map | recoverable |
| `layerctl` | drives the layer opcodes `0xD1`–`0xD4` (read active/enabled; enable mask; switch) | reversible |

## Protocol-safety rules (read before touching device I/O)

The cardinal rule: **only ever emit opcodes we know.** This protocol family also
contains a firmware-flash opcode (`0x20`) and a set-model opcode (`0x60`).
`elfctl` must **never** emit those, and must **never** write an *unknown* opcode
to probe it.

- **Reads can't change state** — read sweeps (`0x82`/`0x83`, `0xC1`,
  `0xD1`/`0xD3`) are always safe to run freely.
- **Writes must be recoverable** — capture the current value first and verify by
  read-back, the way `set_key`/`set_macro` and the `experiment`/`layerwrite`
  harnesses do. Every write path in `elfctl.c` reads back and compares.
- **To learn an unknown command, don't fuzz it.** Get it from the official
  configurator's source or a usbmon/USBPcap capture. The layer (`0xD0`-family)
  and macro (`0xC0`-family) opcodes were both pinned down this way — documented
  in two independent sources, then confirmed on hardware — never by guessing.

Anything emitting opcodes belongs behind the same find-device / `write_cmd` /
verify-by-read-back path the existing commands use.

## Things the hardware taught us (easy to get wrong)

These cost real debugging rounds; they're the kind of detail that looks fine in
code review and only fails on the wire:

- **set-key byte[1] is a key TYPE, not a count.** `0x01` = single key,
  `0x0A` = macro link. A single-key write uses payload length 4; a macro link
  uses length **8** with the id at `data[2]` (and reads back at `r[2]`).
- **Macro id byte position differs by opcode**: read/delete macro put the id in
  byte[3] (`01 C1 00 <id>`); set-macro puts it in byte[4]. An empty macro slot
  replies with a single all-`0xff` report.
- **Read macros deterministically**, not "read until quiet": 4 name reports, one
  header report, then `ceil(byteCount/8)-1` action reports. The firmware can
  pause between the name and action blocks longer than an inter-report timeout,
  so a drain-loop intermittently returns 0 steps.

## Releases

The **git tag is the source of truth** for the version (`v`-prefixed, e.g.
`v0.3.0`). The bare number is mirrored in `ELFCTL_VERSION` in `elfctl.c` and
reported by `elfctl --version` — it is *dragged along* to match the tag, never
the other way round (the macro may be pre-bumped ahead of the latest tag).

Releases are cut with the bundled `/release` Claude Code skill
(`.claude/skills/release/`): `/release patch|minor|major` bumps the latest tag,
updates the macro, pushes `master`, and creates the GitHub release with an
oldest-first changelog. The tag is lightweight and created on the remote, so the
skill fetches it back afterward.
