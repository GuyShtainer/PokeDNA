# The verified-write safety pipeline (never corrupt the original)

A reusable "never corrupt the original" file-IO pattern for any FatFs-based GBA tool: full-file read, immutable backup rotation, and a verified write that only destroys the old file after the new bytes are confirmed byte-identical on disk. Treat this as the reference implementation any file-writing feature (the file browser's copy/edit, a SAV editor's write-back) should copy — it is **not yet vendored** into the toolkit `lib/`.

> Reference impl lives in the Pokemon record-mixer repo: `/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`. All `source/savefile.*` citations below are relative to that repo (NOT vendored here). The only piece this module depends on that *is* vendored in the toolkit is `lib/sys.h` (the `EWRAM_BSS` macro). See [../CAPABILITIES.md](../CAPABILITIES.md) and [../ROADMAP.md](../ROADMAP.md) for where this lands.

## Load-bearing gotchas

These are the things that bite you. Read this section even if you read nothing else.

| Gotcha | Why it bites | Where |
|---|---|---|
| **The rename gap.** `sf_write_verified` calls `f_unlink(path)` on the *original* **before** `f_rename(tmp, path)`. If the rename then fails you get `SF_ERR_RENAME` with the original gone but the **verified `.tmp` left on disk** for manual recovery. | A failure in this narrow window is the one moment the original is not present. Callers/users must know to recover from `<path>.tmp`. | `source/savefile.c:170` |
| **Only 21 backup slots.** `sf_backup` uses `.bak`, then `.bak1`..`.bak20`, and **never overwrites**. Once all 21 exist it returns `SF_ERR_BACKUP` and the commit aborts. | A long-running install accumulates backups until commits start failing; the user must clear old `.bak*` files. | `source/savefile.c:111` |
| **`files_equal` needs TWO distinct buffers.** It reads file A into `s_cmp` and file B into a separate `bufb[4096]`, then `memcmp`s. Collapsing them to one buffer silently makes every comparison pass. | A "cleanup" that shares the buffer turns the verify into a no-op. | `source/savefile.c:63` |
| **No large stack buffers, ever.** Every big working array is `static ... EWRAM_BSS`. IWRAM stack is tiny (~few KiB); a 128 KiB or 16 KiB stack local overflows it and corrupts memory with no diagnostic. | Easy to "just declare a local buffer" and get silent corruption. | `source/savefile.c:11`, `lib/sys.h:67` |
| **Short reads are failures.** Every byte-compare loop caps each chunk at `sizeof(s_cmp)` (4096) and treats `br != want` as a mismatch. | Without the `br != want` check a truncated re-read would pass verification. | `source/savefile.c:159` |
| **Cross-file atomicity is NOT guaranteed.** The 4-phase multi-file commit (`sf_mix_bidir`) validates *all* outputs before writing *any*, but if file B's write fails after A committed, A is already updated (it has a `.bak`). | Don't assume "both or neither" across files — only per-file safety plus all-or-nothing *validation* gating. | `source/savefile.c:457` |
| **Work buffer must be the full image size.** `sf_read_full` is called with `cap = G3_SAVE_FILE_SIZE` (128 KiB). A smaller `work` buffer overflows. | Caller-owned buffer; the contract is 128 KiB. | `source/savefile.h:44` |

## The error enum

`SfStatus` is the single error type returned by every public entry point; `sf_status_str()` maps each to a short string for logging (`source/savefile.c:28`). Defined at `source/savefile.h:9`.

| Value | Meaning | String (`sf_status_str`) |
|---|---|---|
| `SF_OK` | success | `"OK"` |
| `SF_ERR_OPEN` | could not open a file | `"open failed"` |
| `SF_ERR_READ` | read error / short read | `"read error"` |
| `SF_ERR_WRITE` | write error / short write | `"write error"` |
| `SF_ERR_SIZE` | file too small / wrong size | `"bad size"` |
| `SF_ERR_VERIFY` | written bytes != intended bytes (round-trip) | `"verify mismatch"` |
| `SF_ERR_BACKUP` | backup copy failed or mismatched | `"backup failed"` |
| `SF_ERR_PARSE` | content did not validate | `"parse/validate failed"` |
| `SF_ERR_RENAME` | final rename failed (temp left for recovery) | `"rename failed"` |
| `SF_ERR_LAYOUT` | internal: bad offsets / sector math | `"layout error"` |

`SF_ERR_PARSE` and `SF_ERR_LAYOUT` are the only domain-specific codes; the rest are generic file-IO outcomes any tool can reuse.

## EWRAM-buffer discipline

The first 15 lines of `savefile.c` declare every large buffer as `static ... EWRAM_BSS`, never as a stack local (`source/savefile.c:11`):

```c
static uint8_t EWRAM_BSS s_sb1[G3_SAVEBLOCK1_BYTES];  /* ~15.9 KiB */
static uint8_t EWRAM_BSS s_snap[2 * G3_SECTOR_SIZE];  /* 8 KiB     */
static uint8_t EWRAM_BSS s_cmp[4096];                 /* re-read compare chunk */
```

`EWRAM_BSS` is `__attribute__((section(".sbss")))` — the linker section for EWRAM `.bss` (`lib/sys.h:67`, vendored). This homes large zero-init buffers in the GBA's 256 KiB EWRAM rather than the ~32 KiB IWRAM where the stack lives. The rule for the whole toolkit: **big working buffers are `static EWRAM_BSS`; keep stack frames small.** The full-image work buffer (128 KiB, `G3_SAVE_FILE_SIZE`) is caller-owned and passed in, not declared here.

## `sf_read_full` — whole-file load (close before checking)

```c
SfStatus sf_read_full(const char* path, uint8_t* buf, uint32_t cap, uint32_t* out_size);
```

Opens `FA_READ`, does a single `f_read` of up to `cap` bytes, **`f_close` first, then checks the read result** so the handle is never leaked on error, and reports actual bytes via `*out_size` (`source/savefile.c:44`). The close-before-check ordering is the reusable detail.

## `files_equal` — byte-for-byte file equality

Internal helper (`source/savefile.c:57`). Order of operations:

1. Open both `FA_READ` (close the first if the second open fails).
2. `f_size(a) != f_size(b)` → not equal, short-circuit.
3. Read both in 4 KiB chunks into **two distinct EWRAM buffers** (`s_cmp` for A, static `bufb[4096]` for B), `memcmp` each chunk; a length mismatch or content mismatch → not equal.
4. Always `f_close` both handles.

The two-buffer requirement is the gotcha above — this is the reusable equality primitive behind backup verification.

## `sf_backup` — immutable backup rotation

```c
SfStatus sf_backup(const char* src_path, char* out_bak, unsigned out_bak_cap);
```

Picks the first **non-existent** name among `<src>.bak`, `<src>.bak1` .. `<src>.bak20` (loop `n = 0..20`), so existing backups are **never overwritten**; if all 21 slots are taken it logs and returns `SF_ERR_BACKUP` (`source/savefile.c:108`). It then `copy_file`s src → chosen name and **re-verifies the copy byte-for-byte via `files_equal`** before declaring success; the chosen path is written into `out_bak`. `copy_file` streams in 4 KiB chunks and promotes a failing `f_close` on the *write* handle to `SF_ERR_WRITE` (`source/savefile.c:84`).

## `sf_write_verified` — the core safe write

```c
SfStatus sf_write_verified(const char* path, const uint8_t* buf, uint32_t len);
```

The whole point: **the original is untouched unless the new bytes are verified on disk.** Three phases (`source/savefile.c:140`):

1. **Write temp.** Write `buf(len)` to `<path>.tmp` (`FA_WRITE|FA_CREATE_ALWAYS`), checking `fr == FR_OK`, `bw == len`, **and** `fc (close) == FR_OK`. Any failure → `f_unlink(tmp)`, return `SF_ERR_WRITE`. (`source/savefile.c:150`)
2. **Re-read & byte-compare.** Re-open the temp `FA_READ` and compare it to the intended buffer in `s_cmp`-sized (4096) chunks, treating `br != want` as failure. Mismatch → `f_unlink(tmp)`, return `SF_ERR_VERIFY`. (`source/savefile.c:155`)
3. **Swap into place.** Only now `f_unlink(path)` (ignore error if absent) then `f_rename(tmp, path)`. Rename failure → `SF_ERR_RENAME`, leaving the verified `.tmp` for recovery. (`source/savefile.c:170`)

The original survives every failure mode except the rename gap, where the verified replacement still exists as `<path>.tmp`.

## `verify_image_roundtrip` — the generic dry-run primitive

```c
static SfStatus verify_image_roundtrip(const char* path, const char* suffix,
                                       const uint8_t* work, uint32_t size);
```

Write `work[0..size)` to `<path><suffix>`, re-read, byte-compare to `work`, then `f_unlink` — returns `SF_OK` iff identical (`source/savefile.c:278`). This proves the SD write path works **on the real bytes without touching the original**. It is the leaf operation behind any dry-run/validate-only mode; the same temp-write/re-read/compare/delete scaffolding also appears inside the domain `sf_self_test` (`source/savefile.c:245`). Path-derived names use fixed 160-byte buffers built with `siprintf` and suffixes (`.tmp`, `.bak`/`.bakN`, `.selftest`, `.mixtest`).

## The 4-phase multi-file commit (atomicity caveat)

`sf_mix_bidir` is domain-specific (it splices Pokemon secret bases), but its control flow is the reusable pattern for any "update N files together" tool (`source/savefile.c:414`):

| Phase | Action | Writes? |
|---|---|---|
| **1. Snapshot all** | Read every input into RAM (each input fully captured before anything changes). | none |
| **2. Compute all** | Produce every output buffer in RAM from the snapshots. | none |
| **3. Validate all** | Run each output through `verify_image_roundtrip` (temp round-trip) + re-parse. | temp only, deleted |
| **4. Commit each** | Only if *all* validated: write each file with its own `sf_backup` then `sf_write_verified`, in order. | yes |

`commit == false` stops after Phase 3 — a pure dry run, neither file modified (`source/savefile.c:452`).

**Atomicity caveat:** validation is all-or-nothing, but the *writes* are sequential and per-file. If file B's write fails after A committed, A is already updated and the code logs that A has a `.bak` for recovery (`source/savefile.c:461`). True cross-file atomicity is **not** guaranteed — only per-file safety (verified write + backup) plus the all-or-nothing validation gate. Any future toolkit feature that edits multiple files must accept this limitation or build a stronger journal on top.

## Reuse map

| Primitive | Use it for |
|---|---|
| `sf_write_verified` | Any single-file write-back: the SAV editor saving changes, the browser's in-place edit. |
| `sf_backup` | Make an immutable safety copy before any destructive operation. |
| `verify_image_roundtrip` | Dry-run / "validate without writing" modes. |
| `files_equal` | Confirm a copy/move landed correctly (the file browser's copy). |
| `sf_read_full` | Load a whole file with a clean close-before-check. |
| 4-phase commit | Batch edits across multiple files (with the atomicity caveat). |

See [./fatfs-fileops.md](./fatfs-fileops.md) for the underlying FatFs calls and mount discipline, and [./ui-and-logging.md](./ui-and-logging.md) for the `log_line` layer these functions report through.

## Sources

No external research URLs apply to this module — it is original project code, not derived from a public reference. The relevant research notes (`gen3-save-format`, `fatfs-fileops`) cover the layers this sits on, not the safety pipeline itself.

Key source files (reference impl: `/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`):

- `source/savefile.h` — `SfStatus` enum and public API contracts (the rename-gap and 128 KiB-buffer notes live in the header comments).
- `source/savefile.c` — full implementation: `sf_status_str` (`:28`), `sf_read_full` (`:44`), `files_equal` (`:57`), `copy_file` (`:84`), `sf_backup` (`:108`), `sf_write_verified` (`:140`), `sf_self_test` (`:176`), `verify_image_roundtrip` (`:278`), `sf_mix_bidir` (`:414`).
- `lib/sys.h:67` — `EWRAM_BSS` macro (vendored in the toolkit).
