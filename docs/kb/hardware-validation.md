# Hardware validation protocol & open risks

This is the field checklist for taking *any* tool built on this toolkit from "passes on
the emulator" to "trusted on a real cart". The whole SD/flashcart path is invisible to
mGBA/melonDS, so emulator green does **not** mean hardware green — read this before you
touch a real EZ-Flash Omega or Everdrive.

> Some facts here live in files vendored into this repo (`lib/`, `source/gba_rtc.*`,
> `source/log.*`). Others come from the **reference implementation: `/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`** (the Pokemon record-mixer — `source/main.c`, `source/savefile.*`, etc.); those are flagged inline.

See also: [../CAPABILITIES.md](../CAPABILITIES.md) · [../ROADMAP.md](../ROADMAP.md) · [./flashcart-io.md](./flashcart-io.md) · [./fatfs-fileops.md](./fatfs-fileops.md) · [./rtc.md](./rtc.md) · [./safety-pipeline.md](./safety-pipeline.md) · [./ui-and-logging.md](./ui-and-logging.md)

---

## Load-bearing gotchas

The things that *will* bite you if you skip them:

| # | Gotcha | Why it bites | Anchor |
|---|--------|--------------|--------|
| 1 | **The SD path is not emulated.** mGBA/melonDS don't model the EZFO/Everdrive OS-mode register protocol, so reads/writes either no-op or fake-succeed in emulation. | "Works in mGBA" tells you nothing about SD I/O. Anything touching the card MUST be tested on real silicon. | `lib/ezflashomega/io_ezfo.c:19-26`, [research](https://github.com/afska/gba-flashcartio) |
| 2 | **mGBA debug registers are NO-OPS on hardware.** `log_init()` writes `0xC0DE` to `0x4FFF780` and only enables the console if it reads back `0x1DEA`; on a real cart that handshake fails and `mgba_emit()` early-returns. | Any "debug logging" you relied on under mGBA is silent on hardware. Use the on-screen buffer and the SD log instead. | `source/log.c:19-26,30,39-40` |
| 3 | **WRITE has no retry.** `Write_SD_sectors` issues the command, waits once, and returns failure immediately on timeout (`if (res == 1) return 1;`) — unlike reads, which retry up to twice. | A single transient controller hiccup fails the write with no second attempt. You must verify every write yourself. | `lib/ezflashomega/io_ezfo.c:119-121` vs read retry `:68,84-89` |
| 4 | **Original Omega SRAM→SD autosave collision (~10s).** The non-DE Omega copies SRAM to microSD on every SRAM write and can hard-crash if a card read collides with it. | If your tool writes SRAM then reads the card too soon → crash. The DE uses FRAM and has no autosave, so it's the easy case. | [research](https://github.com/afska/gba-flashcartio) |
| 5 | **ROM disappears during SD I/O.** On EZFO all of ROM space is remapped to the bootloader page; every function/IRQ active during a transfer must run from EWRAM. Driver code is `EWRAM_CODE` and IRQs are forced off. | A ROM-resident IRQ handler firing mid-transfer crashes. Don't call `SoftReset` while `flashcartio_is_reading` is true. | `lib/sys.h:66`, `lib/ezflashomega/io_ezfo.c:11,201-203,211-214`; `flashcartio_is_reading` `lib/flashcartio.h:10` |
| 6 | **RTC may be unexposed → timestamps come out 0.** The Omega DE only answers the GPIO RTC for ROMs it treats as RTC-enabled; plain homebrew gets garbage, which the range check rejects, so `get_fattime()` returns `0` (unset). | File mtimes will be blank/epoch. Verify on *your* cart whether the RTC is live before trusting timestamps. | `source/gba_rtc.c:96-98,12-14`, `lib/fatfs/diskio_write.c:81-92` |

---

## The checklist

### Before you start

- [ ] **Make a full microSD backup** (image the card, or at minimum copy the files the tool touches). Every write path here destroys-then-replaces or relies on `f_rename`; a backup is your only undo.
- [ ] Confirm which cart you have: **original EZ-Flash Omega** (SRAM autosave, harder) vs **Omega DE** (FRAM, "works out of the box") vs **Everdrive GBA X5** (read-only in this toolkit — see below).
- [ ] On the **original Omega**, set ROM wait-states to **3,2 or slower**. `3,1` is documented to crash during SD access. ([research](https://github.com/afska/gba-flashcartio))
- [ ] Confirm `flashcartio_activate()` actually detects your cart. The reference app halts with `"No flashcart detected!"` if it returns false. *(reference impl: `source/main.c:371`)*

### Test on BOTH carts

- [ ] **Omega DE first** — it's the low-risk path (FRAM, no autosave). Get the tool fully green here.
- [ ] **Original Omega second** — re-run the *same* flow and specifically exercise the autosave hazard:
  - [ ] After any operation that writes SRAM, **insert a delay before reading the card** (the SRAM→SD autosave takes ~10s and will crash on collision).
  - [ ] Re-confirm wait-states are 3,2 or slower under the actual ROM you're running.
- [ ] **Everdrive GBA X5**: reads are wired, **writes are intentionally not** — `flashcartio_write_sector` hits the `default:` case and returns `false` for anything that isn't `EZ_FLASH_OMEGA`. Validate read-only flows here; do not expect write to work. (`lib/flashcartio_write.c:22-24`)

### During each run

- [ ] Do **not** trust mGBA-only behavior. If a code path only logs via the mGBA console, you're flying blind on hardware — make sure it also goes to the on-screen buffer (`log_text()`, `source/log.c:37`) and/or the SD log.
- [ ] **Verify every write.** Because writes don't retry (gotcha #3), the safety layer writes to a temp file, re-reads it, byte-compares, and only then `f_unlink` + `f_rename` over the original. Use that pattern (`sf_write_verified`) rather than a bare `f_write`. *(reference impl: `source/savefile.c:140`)*
- [ ] Note that `CTRL_SYNC` is a deliberate no-op (`return RES_OK`) — writes are synchronous/blocking, so there's no cache to flush, but that also means there is no second chance to catch a deferred error. (`lib/fatfs/diskio_write.c:56-58`)
- [ ] For **large multi-MB copies**: each transfer cycles into OS mode (ROM out, IRQs off) and back, in bursts of ≤4 sectors per hardware command. A multi-megabyte copy is thousands of these cycles back-to-back — validate sustained throughput and stability, not just a single small write. (`lib/ezflashomega/io_ezfo.c:106-126`)

### After each run

- [ ] **Pull the card and read the SD log.** The persistent artifact is written by `log_flush_to_sd(path)` (`source/log.h:21`, `source/log.c:76`). In the reference app the path is **`/recmix_log.txt`** (`#define LOG_PATH "/recmix_log.txt"`, flushed after every operation). *(reference impl: `source/main.c:38,90,382`)* For your own tool, read whatever path you pass to `log_flush_to_sd`.
- [ ] Confirm file **timestamps** on the card. If they're all zero, the RTC isn't exposed to your ROM (gotcha #6) — that's expected behavior, not a bug, but know which state you're in.
- [ ] Diff any written file against your pre-run backup to confirm the change is exactly what you intended and nothing else moved.

---

## Claims that still NEED hardware validation

Every "hardware_notes" item from the code reports plus the relevant research below. These are
asserted by source/spec but **not yet confirmed on the bench** — treat each as a test case.

| Claim to validate | Where it comes from | Cite |
|---|---|---|
| EZFO SD register protocol (unlock magic `0x9FE0000`/`0x8000000`/…, control `0x9400000`, data window `0x9E00000`, LBA `0x9600000`/`0x9620000`, count `0x9640000`) actually drives the card on real hardware | flashcartio / ezfo reports | `lib/ezflashomega/io_ezfo.c:19-26,77-79,92,109` |
| `0xEEE1` busy sentinel and `0x100000`-iteration timeout are correct on a real controller (polarity is easy to get wrong) | ezfo report | `lib/ezflashomega/io_ezfo.c:44-60` |
| Write data is DMA'd into `0x9E00000` *before* the command is issued — ordering opposite of reads — and this is the working sequence | ezfo report | `lib/ezflashomega/io_ezfo.c:109-117` |
| ROM-page detection (checksum at `0x8000000+188` after mapping bootloader page) reliably distinguishes EZFO from a real cart and locates the running ROM page | ezfo report | `lib/ezflashomega/io_ezfo.c:148-194` |
| **All of ROM is unmapped during EZFO SD access** (EverDrive only disables the last 16 MB) | research | [gba-flashcartio README](https://github.com/afska/gba-flashcartio) |
| SD-access code must run from RAM; **~1 KB of EWRAM is permanently consumed** by the driver, on top of your buffers | research | [gba-flashcartio](https://github.com/afska/gba-flashcartio) / [ezfo-disc_io](https://github.com/felixjones/ezfo-disc_io) |
| OS↔Game mode toggles via **bit 15 of the Rompage register** (`0x8000` = OS mode); PSRAM read-only in game mode, address shifts | research | [gbatemp ezfo thread](https://gbatemp.net/threads/ez-flash-omega-disc_io-library-project.511490/) |
| **PSRAM-resident execution is the supported path**; NOR-resident execution has documented game-mode-return complexity | research | [ezfo-disc_io](https://github.com/felixjones/ezfo-disc_io) |
| Original Omega **SRAM autosave ~10s collision crash** + **wait-state 3,2-or-slower** requirement | research | [gba-flashcartio](https://github.com/afska/gba-flashcartio) |
| Omega DE **"works great out of the box"** (FRAM, no autosave) | research | [gba-flashcartio](https://github.com/afska/gba-flashcartio) |
| If using **DMA for audio**, SD reads must move to DMA1 (`FLASHCARTIO_USE_DMA1`) or be disabled, or DMA1/2 priority corrupts reads | research | [gba-flashcartio](https://github.com/afska/gba-flashcartio); regs `lib/sys.h:46-57` |
| **EverDrive write path** (CMD25 multi-block, hardware CRC16, data-response token) works — *currently unwired in this toolkit, so untested here* | everdrive report | `lib/everdrivegbax5/disk.c:381-461` |
| EverDrive `ed_set_save_type` must match the game's real save type or OS-mode behavior corrupts; default is `ED_SAVE_TYPE_SRM` | everdrive / flashcartio reports | `lib/sys.h:6-9,27-29` |
| **RTC is exposed to this ROM at all** — Omega DE only answers GPIO RTC for ROMs it treats as RTC-enabled; otherwise timestamps are `0` | rtc report | `source/gba_rtc.c:12-14,96-98` |
| RTC protocol/timing (S-3511A bit-bang, redundant GPIO writes as settling delay) reads the real clock correctly | rtc report | `source/gba_rtc.c:33-56,73` |
| Write **timeout returns failure with no retry**, so callers must verify — validate that the verify-then-rename path actually recovers a failed write | fatfs / ezfo reports | `lib/ezflashomega/io_ezfo.c:119-121` |
| **No GBA-native SD file manager** prior art exists (GodMode9i is DS-only); this is novel tooling — validate the whole UX flow end-to-end on hardware | research | [GodMode9i](https://www.gamebrew.org/wiki/GodMode9i) / [superfw](https://github.com/davidgfnet/superfw) |

---

## Pre-ship checklist

Don't tag a release until all of these are checked on real hardware:

- [ ] microSD backup taken and stored off the card.
- [ ] Full flow passes on **Omega DE** (FRAM).
- [ ] Full flow passes on **original Omega** with wait-states 3,2-or-slower and the SRAM-autosave delay honored.
- [ ] Read-only flows passes on **Everdrive GBA X5** (write paths confirmed disabled / not exercised).
- [ ] At least one **large multi-MB copy** completes and round-trips byte-exact (sustained OS-mode cycling validated).
- [ ] Every write goes through **temp → re-read → byte-compare → rename** (no bare `f_write` ships) — verified a deliberately induced write failure leaves the original intact. *(reference impl: `source/savefile.c:140`)*
- [ ] SD log (`/recmix_log.txt` or your tool's path) reviewed after the final run; no unexplained errors. *(reference impl: `source/main.c:38`)*
- [ ] RTC state known: timestamps either populated (RTC exposed) or knowingly `0` (RTC not exposed). (`lib/fatfs/diskio_write.c:81-92`)
- [ ] No code path depends on mGBA debug registers for correctness — only for convenience. (`source/log.c:19-26`)
- [ ] Ship behind explicit warnings + "back up your card first" for any write/edit feature; read/browse is low-risk, write/edit is research-grade.

---

## Sources

**External references (research):**

- gba-flashcartio (afska) — autodetect, read-only FatFs, ROM-disappears-during-I/O, ~1 KB EWRAM cost, IRQs-off default, original-vs-DE caveats, DMA1 audio note: <https://github.com/afska/gba-flashcartio>
- ezfo-disc_io (felixjones) — code-must-run-from-RAM, PSRAM-vs-NOR mode complexity: <https://github.com/felixjones/ezfo-disc_io>
- EZ-Flash Omega disc_io GBAtemp thread — Rompage bit-15 OS/Game toggle, PSRAM address shift: <https://gbatemp.net/threads/ez-flash-omega-disc_io-library-project.511490/>
- ez-flash/omega-de-kernel (`Ezcard_OP.c`) — the driver `io_ezfo.c` is adapted from: <https://github.com/ez-flash/omega-de-kernel>
- GodMode9i (no GBA-native equivalent) / SuperFW (proof a GBA-native browser works): <https://www.gamebrew.org/wiki/GodMode9i> · <https://github.com/davidgfnet/superfw>

**Key source files (vendored in this repo):**

- `lib/ezflashomega/io_ezfo.c` — EZFO SD protocol, write-without-retry, EWRAM/IRQ handling, ROM-page switching
- `lib/flashcartio.h` / `lib/flashcartio_write.c` — public API, `flashcartio_is_reading` guard, EZFO-only write dispatch
- `lib/fatfs/diskio_write.c` — `disk_write`, `CTRL_SYNC` no-op, `get_fattime` RTC→0 fallback
- `lib/sys.h` — `EWRAM_CODE`/`EWRAM_BSS`, DMA channel selection, IRQ-disable defaults, EverDrive save-type
- `source/gba_rtc.c` — S-3511A reader, range-check "no RTC" behavior
- `source/log.c` / `source/log.h` — mGBA debug handshake (no-op on hardware), SD log flush

**Reference implementation** (`/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`, not vendored here):

- `source/main.c` — `flashcartio_activate()` gate, `/recmix_log.txt` log path
- `source/savefile.c` — `sf_write_verified` temp/verify/rename safe-write pattern
