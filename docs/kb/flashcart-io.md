# Flashcart I/O — SD sector read/write (EZ-Flash Omega DE & Everdrive GBA X5)

A thin backend-dispatch layer (`lib/flashcartio.*`) that detects which GBA flashcart is running the ROM, then exposes a uniform 512-byte LBA sector read/write contract over the cart's microSD by delegating to the EZ-Flash Omega DE or Everdrive GBA X5 hardware driver. This doc grounds the public API, detection order, the EZ-Flash OS-mode register protocol, the Everdrive native SD protocol, and the EWRAM/IRQ/DMA discipline that keeps it from crashing.

See also: [../CAPABILITIES.md](../CAPABILITIES.md), [./fatfs-fileops.md](./fatfs-fileops.md) (FatFs diskio sits on top of this), [./safety-pipeline.md](./safety-pipeline.md) (write verification), [./build-and-toolchain.md](./build-and-toolchain.md) (EWRAM linker placement), [./hardware-validation.md](./hardware-validation.md).

## Load-bearing gotchas

These are the things that bite you. Read them before touching anything.

- **ROM disappears during EZ-Flash I/O.** While the EZ-Flash performs an SD op it remaps the entire `0x08000000` ROM window to the bootloader/OS page (`rompage = 0x8000`). Any code or data the CPU touches during the op — including a normally-mapped IRQ vector that reads ROM — will crash. Every function on the hot path is `EWRAM_CODE` (placed in EWRAM, `long_call`) precisely so it survives the remap (`lib/sys.h:66`, `lib/ezflashomega/io_ezfo.c:11`). The Everdrive only disables its *last 16 MB* of ROM, so it is less hostile, but this codebase still brackets it with IRQs off.
- **`flashcartio_is_reading` is the IRQ kill-switch.** It is set `true` around BOTH reads and writes (the write path reuses the read guard), and any IRQ/VBlank handler must consult it and refuse to `SoftReset` or touch the cart ROM while it is true (`lib/flashcartio.h:10`, `lib/flashcartio.c:69,73,84,86`, `lib/flashcartio_write.c:16-18`).
- **Detection order is Everdrive-FIRST, EZ-Flash-second** (`lib/flashcartio.c:17-58`). Preserve this. EZ-Flash detection is invasive — it unmaps ROM and compares header checksums — so the cheaper, safer Everdrive register probe runs first.
- **Everdrive WRITE is intentionally NOT wired.** `flashcartio_write_sector` only implements the `EZ_FLASH_OMEGA` case; the `default` (which includes `EVERDRIVE_GBA_X5`) returns `false` (`lib/flashcartio_write.c:10-26`). The Everdrive driver *has* a working `diskWrite` (`lib/everdrivegbax5/disk.c:381`), it is just not exposed through the dispatch in this codebase. Do not assume writes work on Everdrive here.
- **READ retries once; WRITE has NO retry.** The EZ-Flash read burst retries up to twice on timeout (`times=2`, `delay(5000)`); the write burst returns failure immediately on the first timeout (`lib/ezflashomega/io_ezfo.c:68,84-90` vs `120-121`). The caller MUST verify writes itself (read-back / checksum) — see [./safety-pipeline.md](./safety-pipeline.md).
- **Transfers happen in bursts of at most 4 sectors per hardware command** on the EZ-Flash (`for i ... i+=4; blocks = min(4, count-i)`). There is no single-shot arbitrary-`count` command (`lib/ezflashomega/io_ezfo.c:69-70,106-107`).
- **`dmaCopy` is 32-bit-word DMA (`size>>2` words).** Sector size (512) is a multiple of 4 so this is fine, but buffers must be 4-byte aligned or the tail bytes are dropped (`lib/sys.h:111-124`). If your app uses DMA for audio, route SD reads to **DMA1** (`FLASHCARTIO_USE_DMA1`) because DMA1/DMA2 have higher priority and will corrupt a DMA3 SD transfer; or disable DMA entirely (`FLASHCARTIO_DISABLE_DMA`) for slow-but-safe CPU copies (`lib/sys.h:11-19,46-57`).
- **`u8`/`u16`/`u32` are `#define` macros, not typedefs** (`lib/sys.h:59-64`). Including `sys.h` pollutes those identifiers globally — be careful mixing with libgba/tonc types.
- **`0xEEE1` polarity.** The EZ-Flash response register at `0x9E00000` returns `0xEEE1` while the SD is *busy*; `Wait_SD_Response` returns `0` (success) only when it reads anything *else*, and `1` (timeout) after `>0x100000` spins (`lib/ezflashomega/io_ezfo.c:44-60`). Do not invert this.
- **Everdrive: match the emulated save type.** `ed_set_save_type` defaults to `ED_SAVE_TYPE_SRM`; a wrong save type can corrupt OS-mode behavior (`lib/sys.h:6-9,27-29`).

## Public dispatch API

`lib/flashcartio.h` (read), `lib/flashcartio_write.h` (write, additive in a separate translation unit).

| Symbol | Decl | Impl | Notes |
|---|---|---|---|
| `ActiveFlashcart` enum `{ NO_FLASHCART, EVERDRIVE_GBA_X5, EZ_FLASH_OMEGA }` | `lib/flashcartio.h:7` | — | The dispatch key for every read/write switch |
| `extern ActiveFlashcart active_flashcart;` | `lib/flashcartio.h:9` | `lib/flashcartio.c:14` (default `NO_FLASHCART`) | Set by `flashcartio_activate` |
| `extern volatile bool flashcartio_is_reading;` | `lib/flashcartio.h:10` | `lib/flashcartio.c:15` (default `false`) | IRQ guard, see gotchas |
| `bool flashcartio_activate(void)` | `lib/flashcartio.h:12` | `lib/flashcartio.c:17` | Detect + init; call once at startup |
| `bool flashcartio_read_sector(unsigned int sector, unsigned char* destination, unsigned short count)` | `lib/flashcartio.h:13-15` | `lib/flashcartio.c:60` | Impl signature is `(u32, u8*, u16)` |
| `bool flashcartio_write_sector(u32 sector, const u8* source, u16 count)` | `lib/flashcartio_write.h:20` | `lib/flashcartio_write.c:10` | EZ-Flash only; returns `false` otherwise |

Sector size is fixed at **512 bytes** throughout; `count` is the number of sectors, so `destination`/`source` must be `>= count*512` bytes (`lib/ezflashomega/io_ezfo.c:92,109`). Buffers used during an op should live in EWRAM (IWRAM is only 32 KiB).

Only two call sites of the public API exist in the reference record-mixer app (reference impl: `/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`): `flashcartio_activate()` at `source/main.c:371` (halts with "No flashcart detected!" on `false`) and a `switch (active_flashcart)` at `source/main.c:53` purely to print a human-readable cart name. Sector read/write are driven indirectly via the FatFs diskio glue, not directly from `main` — see [./fatfs-fileops.md](./fatfs-fileops.md).

### Detection order and why (`lib/flashcartio.c:17-58`)

1. **Everdrive GBA X5 first** (`lib/flashcartio.c:18-47`, gated by `FLASHCARTIO_ED_ENABLE`): optionally save+clear `REG_IME`, then `ed_init_sd_only()` → `ed_init()` → `ed_set_save_type(FLASHCARTIO_ED_SAVE_TYPE)` → `diskInit() == 0` → `ed_lock_regs()`. On success sets `active_flashcart = EVERDRIVE_GBA_X5` and returns `true`.
2. **EZ-Flash Omega second** (`lib/flashcartio.c:49-55`, gated by `FLASHCARTIO_EZFO_ENABLE`): `_EZFO_startUp()` → sets `EZ_FLASH_OMEGA`, returns `true`.
3. Neither responds → returns `false`.

The Everdrive probe is a cheap register-writability test; the EZ-Flash probe unmaps ROM and compares the header checksum (more invasive). Trying the cheap/safe path first avoids touching the ROM map on Everdrive hardware.

### Compile gates (`lib/sys.h:11-44`)

| Macro | Default | Effect |
|---|---|---|
| `FLASHCARTIO_ED_ENABLE` | 1 | Compile the Everdrive backend + cases |
| `FLASHCARTIO_EZFO_ENABLE` | 1 | Compile the EZ-Flash backend + cases |
| `FLASHCARTIO_ED_DISABLE_IRQ` | 1 | Save/clear/restore `REG_IME` around Everdrive ops |
| `FLASHCARTIO_EZFO_DISABLE_IRQ` | 1 | Save/clear/restore `REG_IME` inside the EZ-Flash driver |
| `FLASHCARTIO_ED_SAVE_TYPE` | `ED_SAVE_TYPE_SRM` | Emulated save type for Everdrive OS mode |
| `FLASHCARTIO_USE_DMA1` | 0 | Use DMA1 instead of DMA3 (audio conflict) |
| `FLASHCARTIO_DISABLE_DMA` | 0 | Plain `u32` copy loop instead of hardware DMA |

## EZ-Flash Omega DE backend (`lib/ezflashomega/io_ezfo.{c,h}`)

Adapted from the official EZ-Flash `omega-de-kernel` `Ezcard_OP.c` (`lib/ezflashomega/io_ezfo.c:8-9`). 223 lines; every hardware-touching function is `EWRAM_CODE`.

### The "ROM disappears" rule

`SetRompage(page)` writes `page` to `0x9880000` wrapped in the magic prologue/epilogue (`lib/ezflashomega/io_ezfo.c:128-135`). Page meanings:

| `rompage` value | Meaning |
|---|---|
| `0x8000` (`ROMPAGE_BOOTLOADER`) | Bootloader / OS mode — SD access enabled, **all game ROM gone** |
| `0x200` (`ROMPAGE_PSRAM`) | PSRAM page (where a PSRAM-loaded ROM lives) |
| `0..0x1FF` | NOR-flash 1 MiB pages (512 total, chip `S98WS512PE0`) |

Both `_EZFO_readSectors` and `_EZFO_writeSectors` do the same dance (`lib/ezflashomega/io_ezfo.c:196-222`): optionally save+clear `REG_IME` → `SetRompage(ROMPAGE_BOOTLOADER)` to enter OS/SD mode → transfer → `SetRompage(ROMPAGE_ROM)` to restore the running game's page → restore `REG_IME`. `ROMPAGE_ROM` is discovered once at startup and stored in EWRAM `.sbss` so it survives across calls (`lib/ezflashomega/io_ezfo.c:143`).

### Register map (all 16-bit volatile)

| Address | Role | Citation |
|---|---|---|
| `0x9FE0000 = 0xD200`, `0x8000000 = 0x1500`, `0x8020000 = 0xD200`, `0x8040000 = 0x1500` | Magic unlock prologue (before every command) | `lib/ezflashomega/io_ezfo.c:20-23` |
| `0x9FC0000 = 0x1500` | Commit/suffix (after every command) | `lib/ezflashomega/io_ezfo.c:25` |
| `0x9400000` | SD control: `1`=enable, `3`=read-state, `0`=disable | `lib/ezflashomega/io_ezfo.c:19,24,28-38` |
| `0x9600000` | LBA low 16 (`address & 0xFFFF`) | `lib/ezflashomega/io_ezfo.c:77` |
| `0x9620000` | LBA high 16 (`(address>>16) & 0xFFFF`) | `lib/ezflashomega/io_ezfo.c:78` |
| `0x9640000` | Op/block-count: read = `blocks`; write = `0x8000 + blocks` (**bit `0x8000` = write**) | `lib/ezflashomega/io_ezfo.c:79,116` |
| `0x9880000` | `rompage` select | `lib/ezflashomega/io_ezfo.c:133` |
| `0x9E00000` | SD response status (read) AND DMA data window (src for reads, dst for writes); `0xEEE1` = busy | `lib/ezflashomega/io_ezfo.c:41,92,109` |

### Read burst (`lib/ezflashomega/io_ezfo.c:62-96`)

Per 4-sector burst: `SD_Enable()` → magic + LBA + `blocks` to the count reg → `SD_Read_state()` → `Wait_SD_Response()`. On timeout (`res == 1`) decrement `times` and `goto read_again` after `delay(5000)` (retries once, `times` starts at 2). Then `dmaCopy(0x9E00000 → buffer + i*512, blocks*512)`. `SD_Disable()` after the loop.

### Write burst (`lib/ezflashomega/io_ezfo.c:98-126`)

Ordering is **opposite to reads** — data is DMA'd into the window *before* the command is issued. Per burst: `dmaCopy(buffer + i*512 → 0x9E00000, blocks*512)` FIRST, THEN magic + LBA + `0x8000 + blocks` to the count reg → `Wait_SD_Response()`. On timeout `return 1` immediately — **no retry**. After the loop: `delay(3000)` then `SD_Disable()`. (`SD_Enable()` + `SD_Read_state()` are issued once up front.) Preserve the DMA-then-command ordering if reimplementing.

### Detection (`_EZFO_startUp`, `lib/ezflashomega/io_ezfo.c:155-194`)

Read the ROM header checksum complement at `0x8000000 + 188` (offset `0xBC`). Map `ROMPAGE_BOOTLOADER`; **if the checksum STILL matches, this is NOT an EZ-Flash** → return `false` (detection is inverted by intent). Otherwise locate where the running ROM is remapped: try PSRAM page `0x200` first, then scan all 512 NOR pages `0..0x1FF`; store the match in `ROMPAGE_ROM`. A cart whose checksum coincidentally matches the bootloader page could be mis-detected.

### Low-level API (`lib/ezflashomega/io_ezfo.h:8-10`)

- `bool _EZFO_startUp(void)` — detect + locate ROM page (impl `io_ezfo.c:155`).
- `bool _EZFO_readSectors(u32 address, u32 count, void* buffer)` — impl `io_ezfo.c:196`.
- `bool _EZFO_writeSectors(u32 address, u32 count, const void* buffer)` — impl `io_ezfo.c:210`. Directly reusable for any Omega DE project.

## Everdrive GBA X5 backend (`lib/everdrivegbax5/`)

Unlike the EZ-Flash's fixed vendor MMIO, the Everdrive implements the **real native SD command protocol** through an FPGA register window. The two backends share only the LBA + 512-byte-sector contract; no low-level code is shared.

### Register window (`lib/everdrivegbax5/everdrive.c`)

- `REG_BASE = 0x9FC0000`; each register at `REG_BASE + reg*2` (16-bit, word-strided) via `ed_reg_rd`/`ed_reg_wr` (`everdrive.c:16,77-83`).
- Register IDs: `REG_CFG=0x00`, `REG_STATUS=0x01`, `REG_SD_CMD=0x08`, `REG_SD_DAT=0x09`, `REG_SD_CFG=0x0A`, `REG_SD_RAM=0x0B`, `REG_KEY=0x5A` (`everdrive.c:16-26`).
- **Unlock** by writing `0xA5` to write-only `REG_KEY` (`everdrive.c:54`). `ed_init_sd_only()` detects an Everdrive by checking that `REG_SD_CFG` is *unwritable before* the `0xA5` key and *writable after* it (`everdrive.c:48-62`).
- `STAT_SD_BUSY=1` (poll-until-clear), `STAT_SDC_TOUT=2` (data timeout); all cmd/dat helpers busy-wait on `STAT_SD_BUSY`.

### Native SD protocol summary (`lib/everdrivegbax5/disk.c`)

- `diskCmdSD(cmd, arg)` builds a 6-byte command (`cmd | arg32 | CRC7`), software CRC7 via `diskCrc7`, parses typed R1/R2/R3/R6/R7 responses with validation (`disk.c:109,74`).
- `diskInit()` runs the full power-on sequence: SPD_LO, MODE8, 40× `0xff` clocks, CMD0, CMD8 (`0x1aa` voltage check → SD_V2), CMD55+ACMD41 (`0x40300000`, HCS) until ready, OCR → SD_HC, CMD2 (CID), CMD3 (RCA), CMD9 (CSD), CMD7 (select), CMD55+ACMD6 (4-bit bus), then SPD_HI. Returns 0 or `DISK_ERR_INIT+n` (`disk.c:174`).
- `diskRead(sd_addr, dst, slen)` opens a CMD18 multi-block stream (`diskOpenRead`) and keeps it open across sequential calls via static `disk_addr`; `ed_sd_dma_rd` streams 512-byte sectors (`disk.c:281,269`). DMA transfers one sector = 256 16-bit words (`DMA_LEN = 256`) from `REG_BASE + REG_SD_DAT*2`; if `dst` is ROM space (`(dst & 0xE000000)==0x8000000`) it auto-routes into PSRAM with `CFG_AUTO_WE` (`everdrive.c:134-135,185-213`).
- `diskWrite(sd_addr, src, slen)` **exists and works** — CMD25 multi-block, per-sector start token, DMA + hardware CRC16, data-response token check (`0x02`=accepted, `0x05`=CRC error) (`disk.c:381`). It is simply **not exposed** through `flashcartio_write_sector`.
- **SDSC (non-HC) cards use BYTE addressing**: both `diskOpenRead` and `diskWrite` multiply the LBA by 512 when `(card_type & SD_HC)==0` (`disk.c:271-272`).

### Everdrive control API (`lib/everdrivegbax5/everdrive.h:48-70`, `disk.h:28-31`)

- `bool ed_init_sd_only(void)` (`everdrive.h:48`) — SD-presence probe (detection).
- `u8 ed_init(void)` (`everdrive.h:50`) — full init (regs on, PSRAM mapped, ROM write enable, big ROM).
- `void ed_unlock_regs(void)` / `void ed_lock_regs(void)` (`everdrive.h:51-52`) — every read in the dispatch is bracketed by unlock/lock (`flashcartio.c:70-72`).
- `void ed_set_save_type(u8 save_type)` (`everdrive.h:70`) — set emulated save type; must match the game.
- `u8 diskInit(void)` / `u8 diskRead(u32, u8*, u16)` (0 == success) (`disk.h:28-29`).

Everdrive save-type constants: `ED_SAVE_TYPE_EEP=16`, `ED_SAVE_TYPE_SRM=32`, `ED_SAVE_TYPE_FLA64=64`, `ED_SAVE_TYPE_FLA128=80` (`lib/sys.h:6-9`).

## EWRAM / `long_call` / `REG_IME` discipline

- `EWRAM_CODE = __attribute__((section(".ewram"), long_call))` and `EWRAM_BSS = __attribute__((section(".sbss")))` (`lib/sys.h:66-67`). `long_call` is required because the code runs from EWRAM while the cart ROM window is remapped out from under the CPU. The `.ewram`/`.sbss` sections come from the devkitPro GBA default linker script (no custom `.ld` in the repo). See [./build-and-toolchain.md](./build-and-toolchain.md).
- `REG_IME` is `*(vu16*)0x04000208` (`lib/sys.h:109`). It is saved/cleared/restored **inside** the EZ-Flash driver (`io_ezfo.c:196-222`, gated by `FLASHCARTIO_EZFO_DISABLE_IRQ`) and **also** around the Everdrive case and `activate` in the dispatch. Disabling IRQs in both the driver and the caller is intentional belt-and-suspenders — keep both.
- Combined with `flashcartio_is_reading`, the system relies on any IRQ handler that *could* fire (e.g. `SoftReset`) checking the flag and never touching the cart ROM mid-transfer.

### DMA helper (`lib/sys.h:111-124`)

`dmaCopy(src, dest, size)` is an always-inline 32-bit (`DMA32`) copy of `size>>2` words. By default it uses **DMA3** (`REG_DMA3SAD=0x40000D4` etc.); `FLASHCARTIO_USE_DMA1` switches to **DMA1** (`0x40000BC` etc.); `FLASHCARTIO_DISABLE_DMA` falls back to a plain `u32` loop. Because it moves whole words, sector sizes/counts must be 4-byte aligned (512 is). **DMA1-vs-DMA3 audio conflict:** apps that drive audio via DMA1/DMA2 (higher priority than DMA3) must move SD reads to DMA1 or disable DMA, or the audio DMA will corrupt the SD transfer.

## Sources

External references (from research):

- afska/gba-flashcartio — the upstream library this code is built on (autodetect, FatFs read-only): <https://github.com/afska/gba-flashcartio>
- EZ-Flash Omega DE kernel (source of `Ezcard_OP.c`): <https://github.com/ez-flash/omega-de-kernel> ; original Omega kernel: <https://github.com/ez-flash/omega-kernel>
- felixjones/ezfo-disc_io — OS-mode "ROM disappears", code-must-run-from-RAM, NOR-vs-PSRAM notes: <https://github.com/felixjones/ezfo-disc_io>
- GBAtemp Omega disc_io thread (rompage bit 15 = OS mode): <https://gbatemp.net/threads/ez-flash-omega-disc_io-library-project.511490/>

Key source files (vendored under `lib/`):

- `lib/flashcartio.h`, `lib/flashcartio.c` — dispatch + detection
- `lib/flashcartio_write.h`, `lib/flashcartio_write.c` — write dispatch (EZ-Flash only)
- `lib/sys.h` — typedef macros, EWRAM attrs, DMA helper, compile gates
- `lib/ezflashomega/io_ezfo.c`, `lib/ezflashomega/io_ezfo.h` — EZ-Flash Omega DE driver
- `lib/everdrivegbax5/everdrive.c`, `lib/everdrivegbax5/everdrive.h`, `lib/everdrivegbax5/disk.c`, `lib/everdrivegbax5/disk.h` — Everdrive GBA X5 native SD driver
