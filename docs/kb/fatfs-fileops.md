# FatFs file & directory operations

How elm-chan FatFs is mounted over the flashcartio block device on the GBA, which `f_*` operations are compiled in (and which are deliberately *not*), and the recipes for listing / copying / deleting / moving files in this exact config. Pairs with [./flashcart-io.md](./flashcart-io.md) (the SD block layer beneath) and [./safety-pipeline.md](./safety-pipeline.md) (the verified-write pattern built on top).

> Files cited as `source/...` other than `source/gba_rtc.*` and `source/log.*` live in the **reference impl: /Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA** (the Pokemon record-mixer), not in this toolkit. Everything under `lib/` plus `source/gba_rtc.*` / `source/log.*` is vendored here.

## Load-bearing gotchas

- **No `f_copy` exists.** A file copy is a hand-rolled `f_read`/`f_write` loop. EOF = `br < btr`; disk-full = `bw < btw`. See the recipe below.
- **The DMA path needs a word-aligned buffer, but the glue only tests the low bit.** Both `disk_read` and `disk_write` check `((u32)buff & 0x1)` — odd → stage through a static EWRAM scratch buffer; even → pass straight through (`lib/fatfs/diskio.c:38`, `lib/fatfs/diskio_write.c:32`). A merely 2-byte-aligned buffer slips through the "direct" path. This matches afska's original read code; keep the same test if you copy it.
- **`GET_SECTOR_COUNT` is intentionally unimplemented** (`disk_ioctl` default → `RES_PARERR`, `lib/fatfs/diskio_write.c:68-70`). This is safe **only** because `FF_USE_MKFS 0` (`lib/fatfs/ffconf.h:30`). If you ever enable `f_mkfs`, you must add `GET_SECTOR_COUNT`.
- **`FF_FS_LOCK 0`** (`lib/fatfs/ffconf.h:247`): there is **no** duplicate-open / open-object protection. You must `f_close` before `f_unlink`/`f_rename` on the same object or you can corrupt the volume. The verified-write flow does exactly this (close → unlink → rename).
- **LFN uses a static BSS working buffer, so FatFs is NOT thread-safe** (`FF_USE_LFN 1`, `lib/fatfs/ffconf.h:107`; `FF_FS_REENTRANT 0`, `:259`). Single-threaded GBA use only.
- **`f_mount` must be forced** (third arg `1`) and the `FATFS` object must outlive all FatFs use. Reference impl mounts a stack-local `fs` in `main` that lives for the whole program (`source/main.c:375`).
- **Directory-listing end is signalled by `fno.fname[0] == 0`, NOT an error code.** Forgetting this loops forever or misreads the last entry.
- **`get_fattime` returns `0` (unset) on RTC failure** — no fabricated fallback date (`lib/fatfs/diskio_write.c:91`). `FF_FS_NORTC 0` (`:222`) so the `FF_NORTC_*` fixed-date constants are inert.

## How FatFs is mounted over flashcartio

The media glue is split across two files so the vendored read-only `diskio.c` stays untouched and the write half activates only when `FF_FS_READONLY == 0`:

| Callback | File:line | Behaviour |
|---|---|---|
| `disk_status` | `lib/fatfs/diskio.c:21-23` | `0` for drive 0, else `STA_NOINIT`. |
| `disk_initialize` | `lib/fatfs/diskio.c:29-31` | `0` if `active_flashcart != NO_FLASHCART`, else `STA_NOINIT`. No per-drive init beyond global flashcart detection. |
| `disk_read` | `lib/fatfs/diskio.c:37-51` | Odd-aligned `buff` → stage through scratch in ≤4-sector chunks + `memcpy`; aligned → `flashcartio_read_sector` direct. |
| `disk_write` | `lib/fatfs/diskio_write.c:29-46` | Mirror of read in reverse: odd-aligned source `memcpy`'d INTO scratch 4 sectors at a time then written; aligned source passed straight to `flashcartio_write_sector`. `RES_ERROR` on any failed sector. |
| `disk_ioctl` | `lib/fatfs/diskio_write.c:52-72` | Exactly 3 cmds (below). |
| `get_fattime` | `lib/fatfs/diskio_write.c:81-92` | Cartridge RTC → DOS datetime DWORD; `0` on RTC failure. |

**Odd-buffer staging through EWRAM scratch.** Two separate `static u8 EWRAM_BSS ... [512 * 4]` buffers (2 KiB / 4 sectors each), 4-byte aligned: `aligned_buff` for reads (`lib/fatfs/diskio.c:15`) and `w_aligned_buff` for writes (`lib/fatfs/diskio_write.c:23`). `EWRAM_BSS` = `__attribute__((section(".sbss")))` so they live in the 256 KiB EWRAM, not the tiny IWRAM stack. The staging loop processes ≤4 sectors per iteration.

**`disk_ioctl` — the three commands** (`lib/fatfs/diskio_write.c:55-71`):

| cmd | Returns | Why |
|---|---|---|
| `CTRL_SYNC` | `RES_OK` | Writes are synchronous/blocking; nothing to flush. |
| `GET_SECTOR_SIZE` | `*(WORD*)buff = 512` | Fixed sector size. |
| `GET_BLOCK_SIZE` | `*(DWORD*)buff = 1` | Erase-block unit. |
| `GET_SECTOR_COUNT` | *(default)* `RES_PARERR` | **Deliberately unimplemented** — only `f_mkfs` needs it and it is disabled (`FF_USE_MKFS 0`). |

**`get_fattime`** reads `gba_rtc_get()` and packs `((year-1980)<<25)|(month<<21)|(day<<16)|(hour<<11)|(minute<<5)|(second/2)` (`lib/fatfs/diskio_write.c:84-89`). On RTC failure returns `0`. See [./rtc.md](./rtc.md).

## Which ffconf option gates which `f_*` op

| ffconf option | Gates | Effect of the value used here |
|---|---|---|
| `FF_FS_READONLY` | all write/create/delete/rename + `f_write`/`f_sync`/`f_unlink`/`f_mkdir`/`f_rename`/`f_truncate`/`f_getfree` | `0` → write API enabled. |
| `FF_FS_MINIMIZE` (0–3) | master gate, see levels below | `0` → all basic functions present. |
| `FF_USE_FIND` | `f_findfirst` / `f_findnext` (needs MINIMIZE ≤ 1) | `0` → **not** available. |
| `FF_FS_RPATH` (0–2) | `f_chdir`/`f_chdrive` (≥1), `f_getcwd` (=2) | `2` → all relative-path APIs available. |
| `FF_USE_LFN` (0–3) | long filenames in `FILINFO.fname` | `1` → LFN via static BSS buffer. |
| `FF_USE_MKFS` | `f_mkfs` | `0` → **not** available (and lets `GET_SECTOR_COUNT` stay unimplemented). |

**`FF_FS_MINIMIZE` master-gate levels** (`lib/fatfs/ffconf.h:17-24`):

| Level | Removes (cumulative) |
|---|---|
| 0 | nothing — everything below available |
| 1 | `f_stat`, `f_getfree`, `f_unlink`, `f_mkdir`, `f_truncate`, `f_rename` |
| 2 | + `f_opendir`, `f_readdir`, `f_closedir` |
| 3 | + `f_lseek` |

A file manager needing delete/create/move/stat **must** use `FF_FS_MINIMIZE == 0`; directory listing needs ≤ 1.

## Current ffconf.h state

| Option | Value | Line | Consequence |
|---|---|---|---|
| `FF_FS_READONLY` | `0` | `ffconf.h:11` | Write API enabled. |
| `FF_FS_MINIMIZE` | `0` | `ffconf.h:17` | All basic functions present. |
| `FF_USE_FIND` | `0` | `ffconf.h:26` | No `f_findfirst`/`f_findnext`. |
| `FF_USE_MKFS` | `0` | `ffconf.h:30` | No `f_mkfs` → `GET_SECTOR_COUNT` skippable. |
| `FF_USE_FASTSEEK` | `0` | `ffconf.h:33` | — |
| `FF_USE_EXPAND` | `0` | `ffconf.h:36` | No `f_expand`. |
| `FF_USE_CHMOD` | `0` | `ffconf.h:39` | No `f_chmod`/`f_utime`. |
| `FF_USE_LABEL` | `0` | `ffconf.h:44` | No `f_getlabel`/`f_setlabel`. |
| `FF_USE_FORWARD` | `0` | `ffconf.h:48` | No `f_forward`. |
| `FF_USE_STRFUNC` | `1` | `ffconf.h:51` | `f_gets`/`f_putc`/`f_puts`/`f_printf` (LLI+FLOAT, UTF-8). |
| `FF_CODE_PAGE` | `437` | `ffconf.h:79` | U.S. OEM. |
| `FF_USE_LFN` | `1` | `ffconf.h:107` | LFN via static BSS buffer (NOT thread-safe). |
| `FF_MAX_LFN` | `255` | `ffconf.h:108` | — |
| `FF_LFN_UNICODE` | `2` | `ffconf.h:126` | UTF-8; `TCHAR = char`; all path/name strings UTF-8. |
| `FF_LFN_BUF` / `FF_SFN_BUF` | `255` / `12` | `ffconf.h:137-138` | `FILINFO.fname` budget. |
| `FF_FS_RPATH` | `2` | `ffconf.h:145` | `f_chdir`/`f_chdrive`/`f_getcwd` available. |
| `FF_VOLUMES` | `1` | `ffconf.h:157` | Single logical volume. |
| `FF_MIN_SS` / `FF_MAX_SS` | `512` / `512` | `ffconf.h:182-183` | Fixed 512-byte sectors. |
| `FF_LBA64` | `0` | `ffconf.h:191` | 32-bit LBAs. |
| `FF_FS_TINY` | `0` | `ffconf.h:210` | Each `FIL` has its own sector buffer. |
| `FF_FS_EXFAT` | `1` | `ffconf.h:217` | exFAT enabled (requires LFN; discards C89 compat). |
| `FF_FS_NORTC` | `0` | `ffconf.h:222` | Timestamps from `get_fattime`; `FF_NORTC_*` inert. |
| `FF_FS_LOCK` | `0` | `ffconf.h:247` | No duplicate-open protection. |
| `FF_FS_REENTRANT` | `0` | `ffconf.h:259` | Not thread-safe. |

## What IS / IS NOT available in this config

**Available** (`FF_FS_READONLY 0` + `FF_FS_MINIMIZE 0` + `FF_FS_RPATH 2` + `FF_USE_STRFUNC 1`): `f_mount`, `f_open`, `f_close`, `f_read`, `f_write`, `f_sync`, `f_lseek`, `f_truncate`, `f_stat`, `f_getfree`, `f_unlink`, `f_mkdir`, `f_rename`, `f_opendir`, `f_readdir`, `f_closedir`, `f_rewinddir` (via `f_readdir(dp, NULL)`), `f_chdir`, `f_chdrive`, `f_getcwd`, `f_gets`/`f_putc`/`f_puts`/`f_printf`.

**NOT available:** `f_findfirst`/`f_findnext` (`FF_USE_FIND 0`), `f_mkfs`/`f_fdisk` (`FF_USE_MKFS 0`), `f_expand` (`FF_USE_EXPAND 0`), `f_chmod`/`f_utime` (`FF_USE_CHMOD 0`), `f_getlabel`/`f_setlabel` (`FF_USE_LABEL 0`), `f_forward` (`FF_USE_FORWARD 0`), `f_setcp` (single fixed code page 437). **No `f_copy` ever exists in FatFs.**

## Recipes

### Directory listing — `f_opendir` + `f_readdir` loop

End-of-directory is a **null `fname[0]` returned WITHOUT an error**, not an error code. Use `fattrib & AM_DIR` to skip folders. Pattern from the reference impl (`source/main.c:108-120`):

```c
DIR dir; FILINFO fno;
FRESULT fr = f_opendir(&dir, SAVER_DIR);          /* main.c:108 */
if (fr != FR_OK) return -1;
while (count < MAX && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {  /* main.c:113 */
    if (fno.fattrib & AM_DIR) continue;           /* skip directories */
    /* fno.fname is a UTF-8 long name (FF_LFN_UNICODE 2) */
    ...
}
f_closedir(&dir);                                 /* main.c:120 */
```

`f_readdir(dp, NULL)` rewinds the directory. To filter (e.g. only `*.sav`) do it in C — `FF_USE_FIND` wildcards are disabled here.

### File copy — manual loop (no `f_copy`)

Open source `FA_READ`, dest `FA_WRITE | FA_CREATE_ALWAYS` (or `FA_CREATE_NEW` to refuse clobbering), loop `f_read`/`f_write`. **EOF = `br < btr`; disk-full = `bw < btw`.** `f_close` both to flush. The reference impl's `copy_file` does this in 4 KiB chunks through an EWRAM buffer, treating a short write (`bw != br`) as an error and promoting a failing `f_close(dst)` to a write error (`source/savefile.c:84`):

```c
f_open(&fd, dst, FA_WRITE | FA_CREATE_ALWAYS);     /* savefile.c:87 */
for (;;) {
    UINT br, bw;
    f_read(&fs, buf, CHUNK, &br);
    if (br == 0) break;                            /* EOF: br < CHUNK (or 0) */
    f_write(&fd, buf, br, &bw);
    if (bw != br) { err = SF_ERR_WRITE; break; }   /* short write => disk full */
}
/* close errors on the write handle are NOT silently ignored */
```

### Delete — `f_unlink`

`f_unlink(path)` removes a file or sub-directory. Caveats:
- A read-only object (`AM_RDO`) → `FR_DENIED` (no `f_chmod` here to clear it).
- A non-empty sub-directory, or the current directory → `FR_DENIED`. For "delete folder" UX, list-and-remove children first.
- Object must not be open. With `FF_FS_LOCK 0` this is **not** rejected for you — close first or risk corruption.

### Move / rename — `f_rename` (same volume only)

`f_rename(old, new)` renames and/or moves within the **same** volume; it cannot cross drives, cannot rename an open object, and `new` must not already exist (`FR_EXIST`). For cross-card moves, fall back to copy-then-`f_unlink`. With one volume here (`FF_VOLUMES 1`), every rename is same-volume.

The verified-write swap in the reference impl is the canonical safe sequence — close, then unlink the original, then rename the temp (`source/savefile.c:140-172`):

```c
/* ... write buf -> path.tmp, f_close, re-read & byte-compare ... */
f_unlink(path);                       /* savefile.c:171 — original gone only after temp verified */
if (f_rename(tmp, path) != FR_OK)     /* savefile.c:172 */
    return SF_ERR_RENAME;             /* leaves verified .tmp for manual recovery */
```

See [./safety-pipeline.md](./safety-pipeline.md) for the full backup / verify / atomic-swap pattern this is part of.

## LFN & locking caveats

- **`FF_USE_LFN 1` → static BSS working buffer → single-thread only.** Combined with `FF_FS_REENTRANT 0` (`ffconf.h:259`), FatFs here is strictly single-threaded — fine for the GBA's cooperative main loop.
- **`FF_LFN_UNICODE 2` → all path/filename strings are UTF-8** (`TCHAR = char`). `FILINFO.fname` is a UTF-8 long name; budget `FF_LFN_BUF = 255` bytes.
- **`FF_FS_EXFAT 1` requires LFN** and discards ANSI C89 compatibility (vendor note, `ffconf.h:217-220`). Do not disable LFN without also disabling exFAT.
- **`FF_FS_LOCK 0` → no open-object guard.** The application is responsible for not opening / removing / renaming objects that are already open. This is exactly why the safe-write flow `f_close`es before `f_unlink`/`f_rename`.

See [../CAPABILITIES.md](../CAPABILITIES.md) for the toolkit-wide feature matrix and [./build-and-toolchain.md](./build-and-toolchain.md) for the `ffunicode.c` link requirement that LFN imposes.

## Sources

External (elm-chan FatFs reference, stable per-function URLs):
- http://elm-chan.org/fsw/ff/doc/config.html — ffconf options (`FF_FS_MINIMIZE`, `FF_USE_FIND`, `FF_FS_RPATH`, `FF_USE_LFN`, `FF_USE_MKFS`)
- http://elm-chan.org/fsw/ff/doc/opendir.html, readdir.html, sfileinfo.html — directory listing + `FILINFO`
- http://elm-chan.org/fsw/ff/doc/open.html, read.html, write.html, close.html — file copy primitives (EOF/disk-full detection)
- http://elm-chan.org/fsw/ff/doc/unlink.html, rename.html, mkdir.html, stat.html, chdir.html — management ops
- https://elm-chan.org/fsw/ff/ — FatFs project root

Key source files:
- `lib/fatfs/diskio.c` — read-only glue (`disk_status`/`disk_initialize`/`disk_read`)
- `lib/fatfs/diskio_write.c` — `disk_write` / `disk_ioctl` / `get_fattime`
- `lib/fatfs/ffconf.h` — the compiled feature set
- `source/main.c` (reference impl) — `f_mount`, dir-listing loop
- `source/savefile.c` (reference impl) — copy / verified-write / rename pattern
