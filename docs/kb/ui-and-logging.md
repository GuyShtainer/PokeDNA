# UI scaffolding & the triple logger (tonc)

Purpose: a copy-paste-able recipe for a tonc tile-text UI (init, frame tick, edge-detected
input, modal menus, bounded screen assembly) plus the **triple logger** (`source/log.*`,
vendored in this toolkit) that mirrors output to screen, mGBA's debug console, and an SD file
so you can debug GBA homebrew wherever it runs.

The logger (`source/log.{c,h}`) is **vendored here** and meant to be used as-is. The UI
primitives are a **reference implementation** that lives in the record-mixer app, not in this
toolkit — copy them into your `main.c` (reference impl:
`/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`, the Pokemon record-mixer).

## Load-bearing gotchas

| Gotcha | Why it bites | Cite |
|---|---|---|
| `key_poll()` must run **exactly once per VBlank** (inside `vsync`) | `key_hit`/`key_held` edge detection compares this frame's poll to last frame's; polling 0× or 2× per frame breaks rising-edge logic | `source/main.c:74-77` |
| `key_hit(mask)` is **rising-edge only — no auto-repeat** | Holding a key does **not** repeat; menus advance one step per press by design. If you want repeat you must add it yourself | `source/main.c:79-86`, `source/main.c:202-203` |
| Use `siprintf`, **never `sprintf`** | `sprintf` pulls in float-formatting code and a bigger stack frame; the codebase is integer-only on purpose | `source/main.c:131,153` |
| Big buffers **must** be `EWRAM_BSS` | IWRAM is only 32 KiB and holds the stack; a 128 KiB save image, the 8 KiB log buffer, or the 2 KiB screen scratch on the stack overflows it | `source/main.c:41-48`, `source/log.c:10-12` |
| Screen assembly via `strcat` is **manually bounds-checked** into a fixed 2 KiB buffer | There is no `snprintf`-style guard on `strcat`; every append checks `strlen(buf)+strlen(line) < sizeof(buf)-1` first or you silently smash EWRAM | `source/main.c:127-135` |
| The in-RAM log buffer **drops its oldest half** on overflow (8 KiB cap) | Chatty sessions lose the *earliest* on-screen lines — but the SD flush still holds the current buffer | `source/log.c:60-69` |
| Each `log_flush_to_sd` **OVERWRITES** the file (`FA_CREATE_ALWAYS`) | It is not append. The file always reflects the *current* in-RAM buffer (i.e. the live ring), so an overflowed early line is gone from the file too | `source/log.c:76-86` |
| mGBA debug regs respond **only under mGBA** | On real hardware `log_under_mgba()==0` and `mgba_emit` is a no-op; SD + screen are your only sinks | `source/log.c:24-27,39-48` |

## tonc init sequence

`init_system()` brings up a BG-mode-0 tile-text UI on BG0, VBlank-paced
(reference impl: `source/main.c:357-362`):

```c
static void init_system(void) {
  irq_init(NULL);                          // install tonc's IRQ dispatcher
  irq_add(II_VBLANK, NULL);                // enable VBlank IRQ (frame pacing only)
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;     // mode 0, BG0 on
  tte_init_se_default(0, BG_CBB(0) | BG_SBB(31)); // TTE on BG0, char-base 0, screen-base 31
}
```

- `tte_init_se_default(layer, ctrl)` sets up the tonc Text Engine in **se** (regular tilemap)
  mode. `BG_CBB(0)` = char-base block 0 (tile graphics), `BG_SBB(31)` = screen-base block 31
  (the tilemap) — the standard non-overlapping placement (`source/main.c:361`).
- The VBlank IRQ handler is `NULL`; the only thing it does is wake `VBlankIntrWait()`. See
  the build-and-toolchain doc for the libtonc/devkitARM setup that supplies these symbols
  (`../docs/kb/build-and-toolchain.md` if present).

## Reusable UI primitives

All static helpers in the reference `main.c`. Copy them verbatim into a new tool.

| Signature | What it does | Cite |
|---|---|---|
| `static void render(const char* text)` | Full-screen redraw: `tte_erase_screen()` then `tte_write("#{P:0,0}")` (TTE cursor-home escape) then `tte_write(text)` | `source/main.c:68-72` |
| `static void vsync(void)` | One-frame tick: `VBlankIntrWait()` then `key_poll()`. The only correct place to poll keys | `source/main.c:74-77` |
| `static u16 wait_keys(u16 mask)` | Blocks (calling `vsync` each frame) until a key in `mask` is newly pressed; returns the `key_hit` bitmask | `source/main.c:79-86` |
| `static void halt_msg(const char* msg)` | Fatal panic: `log_line("HALT: ...")`, flush log to SD, `render(log_text())`, then `while(1) vsync()` | `source/main.c:88-93` |
| `browse_scan` / `render_browser` / `browse_two` | The scrolling list-picker (replaces the older `pick_save`): `browse_scan` scans the dir into entries, `render_browser` draws the list with the `ui_text_sel` selection bar, `browse_two` runs the cursor/key loop | `source/main.c:239` / `:315` / `:379` |

### Frame tick & input

```c
static void vsync(void) { VBlankIntrWait(); key_poll(); }   // main.c:74-77

static u16 wait_keys(u16 mask) {                            // main.c:79-86
  u16 hit;
  do { vsync(); hit = key_hit(mask); } while (!hit);
  return hit;
}
```

`key_hit` returns only keys that went from up→down **this** frame, which is why
`wait_keys` never auto-repeats. tonc key constants: `KEY_UP/DOWN/LEFT/RIGHT/A/B/L/R/START/SELECT`.
The main list uses mask `KEY_UP|KEY_DOWN|KEY_A|KEY_START|KEY_SELECT` (`source/main.c:388`);
action/toggle screens use `KEY_L|KEY_R|KEY_A|KEY_B` (`source/main.c:199`).

### Menu navigation idiom

Wrap-around modular arithmetic with a `>> ` prefix marking the selected row
(`source/main.c:202-203`, prefix at `source/main.c:131`):

```c
if (k & KEY_DOWN)      sel = (sel + 1) % n;
else if (k & KEY_UP)   sel = (sel == 0) ? n - 1 : sel - 1;
// row text: siprintf(line, "%s%s\n", (i == sel ? ">> " : "   "), names[i]);
```

### Screen assembly: bounded `strcat` into a fixed EWRAM buffer

The screen text is built up line by line in a single `static char EWRAM_BSS g_screen[2048]`
(`source/main.c:48`), then handed to `render()`. Every append is guarded
(`source/main.c:127-135`):

```c
char line[80];
g_screen[0] = 0;
strcat(g_screen, "Record Mixer\n");
for (int i = 0; i < g_count; i++) {
  siprintf(line, "%s%s\n", (i == sel ? ">> " : "   "), g_names[i]); // siprintf, not sprintf
  if (strlen(g_screen) + strlen(line) < sizeof(g_screen) - 1)        // manual overflow guard
    strcat(g_screen, line);
}
render(g_screen);
```

Large static buffers all carry `EWRAM_BSS` (from tonc `sys.h`) because IWRAM (32 KiB) holds
the stack: `g_save[128 KiB]`, `g_screen[2048]`, `g_names[32][64]` (`source/main.c:41-48`).

## The triple logger (`source/log.*`, vendored)

Three sinks so you can see output wherever the ROM runs (`source/log.h:4-9`):

1. **In-RAM text buffer** the UI prints on screen — the only sink that works on real hardware.
2. **mGBA debug console** — only when running under the mGBA emulator.
3. **A file on the SD card** — the persistent artifact for hardware debugging.

### API

| Signature | What it does | Cite |
|---|---|---|
| `void log_init(void)` | Probe the mGBA debug channel (write `0xC0DE`, check `0x1DEA`), then `log_clear()`. Call once at startup before any `log_line` | `source/log.h:11`, `source/log.c:24-28` |
| `int log_under_mgba(void)` | `1` if running under mGBA (channel live), else `0` | `source/log.h:12`, `source/log.c:30` |
| `void log_line(const char* fmt, ...)` | printf-style (`vsnprintf` into a 256-byte stack tmp, ≤255 chars). Emits to mGBA first, then appends a line + `\n` to the RAM buffer. Safe before SD mount | `source/log.h:14`, `source/log.c:50-74` |
| `void log_clear(void)` | Reset the in-RAM buffer to empty | `source/log.h:15`, `source/log.c:32-35` |
| `const char* log_text(void)` | The accumulated NUL-terminated buffer, for on-screen rendering (e.g. `render(log_text())`) | `source/log.h:17`, `source/log.c:37` |
| `int log_flush_to_sd(const char* path)` | Write the whole buffer to `path` with `FA_WRITE\|FA_CREATE_ALWAYS` (**overwrite**). Returns `0` on success, the FatFs `FRESULT` on FS error, or `-1` on short write. Call after the SD is mounted | `source/log.h:19-21`, `source/log.c:76-86` |

### mGBA detection & emit

The mGBA debug interface is three fixed registers (`source/log.c:19-22`); they respond only
under the emulator. Detection writes the enable magic and checks the readback
(`source/log.c:24-27`):

```c
#define MGBA_REG_ENABLE (*(volatile unsigned short*)0x4FFF780)
#define MGBA_REG_FLAGS  (*(volatile unsigned short*)0x4FFF700)
#define MGBA_LOG_BUF    ((volatile char*)0x4FFF600)
#define MGBA_LEVEL_INFO 3

MGBA_REG_ENABLE = 0xC0DE;                          // enable
s_mgba = (MGBA_REG_ENABLE == 0x1DEA) ? 1 : 0;      // readback == 0x1DEA → under mGBA
```

To emit a line: copy up to 255 bytes of the string to `MGBA_LOG_BUF` (0x4FFF600), NUL-terminate,
then write `MGBA_REG_FLAGS = 0x100 | level` (level 3 = INFO) to trigger the log
(`source/log.c:39-48`). On real hardware `s_mgba==0` and `mgba_emit` returns immediately.

### In-RAM buffer: keep-newest ring

The buffer is `static char EWRAM_BSS s_buf[LOG_CAP]` with `LOG_CAP = 8192`
(`source/log.c:10-12`). On overflow it **drops the oldest half** rather than truncating new
output — a coarse keep-newest policy (`source/log.c:60-69`):

```c
if (s_len + n + 2 >= LOG_CAP) {       // would overflow
  unsigned keep = LOG_CAP / 2;
  if (s_len > keep) { memmove(s_buf, s_buf + (s_len - keep), keep); s_len = keep; }
  else              { s_len = 0; }
}
```

Consequence: very chatty sessions lose their *earliest* lines on screen **and** in the SD file
(the file is just a dump of the current buffer). Single log lines are capped at ~255 chars by
the `vsnprintf` tmp and the mGBA 255-byte copy (`source/log.c:51-54,42`).

### SD flush OVERWRITES, never appends

```c
FRESULT fr = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);  // truncate-or-create
fr = f_write(&f, s_buf, s_len, &bw);
```

`FA_CREATE_ALWAYS` truncates any existing file (`source/log.c:76-86`), so each flush replaces
the whole file with the current buffer. The record-mixer deliberately uses the **first**
`log_flush_to_sd` right after `f_mount` as proof that the SD write path works
(reference impl: `source/main.c:381-383`). See `../docs/kb/fatfs-fileops.md` (if present) for
the FatFs file-mode semantics and `../docs/kb/safety-pipeline.md` for how writes are verified.

## Startup wiring (reference)

The record-mixer's `main()` shows the canonical bring-up order
(reference impl: `source/main.c:364-383`):

```c
init_system();                              // tonc UI up
log_init();                                 // probe mGBA, clear buffer
log_line("=== ... ===");
if (!flashcartio_activate()) halt_msg("No flashcart detected!");  // ../docs/kb/flashcart-io.md
FRESULT fr = f_mount(&fs, "", 1);
if (fr != FR_OK) halt_msg("SD mount failed!");
... scan dir ...
int wr = log_flush_to_sd(LOG_PATH);         // first flush = SD-write proof
```

`halt_msg()` is the panic path: it logs, flushes to SD, renders the full log on screen, and
spins — so a crash leaves both an on-screen dump and an SD artifact (`source/main.c:88-93`).

## Sources

External references (from research):
- tonc Text Engine + key handling are part of libtonc, bundled in the devkitPro GBA toolchain
  image `devkitpro/devkitarm` (see `../docs/kb/build-and-toolchain.md` if present).

Key source files:
- `source/log.h`, `source/log.c` — the triple logger (vendored in this toolkit).
- `source/main.c` — UI primitives `vsync`/`wait_keys`/`halt_msg` + the list-picker `browse_scan`/`render_browser`/`browse_two` (Mode-3 `ui.c` layer),
  `init_system`, and the startup wiring (reference impl:
  `/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`).
