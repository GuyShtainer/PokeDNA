# Build & toolchain (devkitARM + libtonc + Docker)

How this repo turns C into a `.gba` cartridge image: a one-line Docker build over the
official `devkitpro/devkitarm` image (no host toolchain), an auto-globbing devkitPro
Makefile, and a small set of section/attribute conventions that exist purely because the
GBA has 32 KiB of fast RAM. Read this before adding a new tool or a new `.c` file.

See also: [../CAPABILITIES.md](../CAPABILITIES.md), [../ROADMAP.md](../ROADMAP.md),
[./flashcart-io.md](./flashcart-io.md), [./fatfs-fileops.md](./fatfs-fileops.md),
[./ui-and-logging.md](./ui-and-logging.md), [./licensing.md](./licensing.md).

## Load-bearing gotchas

These are the things that bite. All are grounded in source below.

| Gotcha | Why it bites | Where |
| --- | --- | --- |
| **IWRAM is 32 KiB and holds the stack.** | Any buffer over a few hundred bytes on the stack overflows it silently and corrupts execution. Put big/static buffers in EWRAM with `EWRAM_BSS`. | `lib/sys.h:67`, `source/log.c:12` |
| **`u8`/`u16`/`u32` are `#define` macros, not typedefs.** | You cannot `typedef`, forward-declare, or take a struct member named `u8`; any header that re-`#define`s them will fight `sys.h`. Token-pasting and `unsigned`-keyword interactions can surprise you. | `lib/sys.h:59-64` |
| **Plain `.c` does NOT go to fast IWRAM/ARM.** | A normal `.c` compiles Thumb into ROM (slow). To get ARM-in-IWRAM you must literally name the file `*.iwram.c`. | `Makefile:52-54`, `Makefile:97` |
| **Flashcart driver choice is compile-time, not runtime.** | `-DFLASHCARTIO_*_ENABLE` selects which drivers link in; there is no runtime config switch. | `Makefile:99`, `lib/sys.h:22-39` |
| **Build needs `DEVKITPRO`/`DEVKITARM` env vars.** | They are pre-set inside the Docker image. On a bare host you must install devkitARM + libtonc and export both, or the Makefile cannot find `arm-none-eabi-gcc` or libtonc. | `Makefile:11-13`, `build.sh:7` |
| **`-ffast-math` + `-fno-strict-aliasing` are on globally.** | Save-format code that type-puns through pointers relies on `-fno-strict-aliasing`; do not drop it. `-ffast-math` is fine here (no FPU) but means no IEEE NaN guarantees. | `Makefile:102` |
| **`ALIGNED` is a local macro, not from `sys.h`.** | DMA needs 4-byte-aligned buffers; the alignment attribute is redefined per-file, so copy it when you add a new DMA buffer. | `lib/fatfs/diskio_write.c:20` |

## Docker one-liner build

`build.sh` is the whole build. It requires only Docker — the image already bundles
devkitARM + libtonc with `DEVKITPRO`/`DEVKITARM` set.

```bash
# build.sh:7-16 (verbatim shape)
IMG="devkitpro/devkitarm:20241104"
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -v "$(pwd)":/project \
  "$IMG" \
  bash -c 'cd /project && make rebuild'
# -> ./record_mixer.gba
```

- `--user $(id -u):$(id -g)` keeps build artifacts owned by you, not root (`build.sh:11`).
- `-v $(pwd):/project` bind-mounts the repo; the container is otherwise disposable (`--rm`) (`build.sh:10,12`).
- The container runs `make rebuild`, which is `clean` + build (`build.sh:14`, `Makefile:184-185`).
- Image is pinned to a date tag (`devkitpro/devkitarm:20241104`) for reproducibility (`build.sh:7`).

## Makefile structure

The Makefile is the devkitPro tonc template, adapted from afska's gba-flashcartio
(`Makefile:1-4`; see [./licensing.md](./licensing.md)). Highlights:

| Concern | Setting | Where |
| --- | --- | --- |
| Toolchain prefix | `PREFIX ?= arm-none-eabi-`, then `CC/CXX/AS/AR/NM/OBJCOPY` | `Makefile:15-22` |
| libtonc link | `LIBS := -ltonc`; `TONCLIB := ${DEVKITPRO}/libtonc`; `LIBDIRS := $(TONCLIB)` | `Makefile:11,79,85` |
| Output name | `PROJ := record_mixer` → `record_mixer.gba` | `Makefile:76` |
| Cart header | `gbafix $@ -t$(TITLE)` with `TITLE := RECMIX` | `Makefile:29,77` |
| Source roots | `SRCDIRS := source lib lib/fatfs lib/ezflashomega lib/everdrivegbax5` | `Makefile:82` |

**Auto-globbing — new `.c` files need NO Makefile edit.** Every `*.c`/`*.cpp`/`*.s` under
`SRCDIRS` (and `*.*` under `DATADIRS`) is discovered by `wildcard` and turned into an
object file automatically (`Makefile:142-145`, `Makefile:153-155`). Drop a file into a
listed dir and rebuild — done. (Adding a *new directory* still requires editing `SRCDIRS`
and `INCDIRS`.)

**ELF → GBA pipeline.** Link produces `record_mixer.elf`; an implicit rule then runs
`objcopy -O binary` to strip it to a raw `.gba`, and `gbafix` patches the cartridge header
(logo checksum, complement, title):

```make
# Makefile:26-29
%.gba : %.elf
	@$(OBJCOPY) -O binary $< $@
	@echo built ... $(notdir $@)
	@gbafix $@ -t$(TITLE)
```

The link step also writes a symbol map (`$(NM) -Sn`) per target (`Makefile:34,39`).

## Compiler & link flags

```
CFLAGS := -mcpu=arm7tdmi -mtune=arm7tdmi -O2 \
          -DFLASHCARTIO_ED_ENABLE=1 -DFLASHCARTIO_EZFO_ENABLE=1 \
          -Wall -ffast-math -fno-strict-aliasing      # Makefile:99-102
```

- `-mcpu=arm7tdmi -mtune=arm7tdmi`: the GBA CPU (`Makefile:99`).
- `-O2`: optimization level (`Makefile:99`).
- `-DFLASHCARTIO_ED_ENABLE=1` / `-DFLASHCARTIO_EZFO_ENABLE=1`: compile in the EverDrive GBA
  X5 and EZ-Flash Omega disk drivers; these are the `#ifndef`-guarded knobs in
  `lib/sys.h:22-39` (`Makefile:99`). See [./flashcart-io.md](./flashcart-io.md).
- **Default codegen is Thumb**: `RARCH := -mthumb-interwork -mthumb` applies to all normal
  `.o` rules (`Makefile:96`, `Makefile:60-62`).
- `CXXFLAGS` adds `-fno-rtti -fno-exceptions` (`Makefile:104`).

Build switches (`Makefile:89-91`): `bMB` (multiboot), `bTEMPS` (`-save-temps`), `bDEBUG`
(`-DDEBUG -g` vs `-DNDEBUG`, `Makefile:120-129`).

## The `*.iwram.c` ARM fast-path rule

Plain `.c` is Thumb in ROM. To place hot code in 32 KiB IWRAM compiled as ARM, name the
file `*.iwram.c` (or `.iwram.cpp`). Dedicated pattern rules build those with `IARCH`:

```make
# Makefile:97
IARCH := -mthumb-interwork -marm -mlong-calls
# Makefile:52-54
%.iwram.o : %.iwram.c
	$(CC) ... $(CFLAGS) $(IARCH) -c $< -o $@
```

`-marm` forces ARM instructions; `-mlong-calls` lets IWRAM code reach ROM addresses. The
linker placement into the IWRAM section is handled by the gba specs/linker script. The
EZ-Flash driver instead uses the `EWRAM_CODE` attribute (below) to relocate hot functions.

## Specs: cartridge vs multiboot, and memory reporting

| Build | Specs | Linked by | Output |
| --- | --- | --- | --- |
| Cartridge (default) | `-specs=gba.specs` | `%.elf` rule, `Makefile:36-38` | `record_mixer.gba` |
| Multiboot (`bMB=1`) | `-specs=gba_mb.specs` | `%.mb.elf` rule, `Makefile:31-33` | `record_mixer.mb.gba` |

`gba.specs` lays code/data for a real cartridge (ROM at 0x08000000, EWRAM/IWRAM data);
`gba_mb.specs` builds a multiboot image that runs entirely from EWRAM (no cart ROM).

`LDFLAGS` always passes `-Wl,--print-memory-usage`, so every build prints IWRAM / EWRAM /
ROM region usage — watch this to catch IWRAM creep before it overflows the stack
(`Makefile:107`).

## Section attributes & the `u8`-is-a-macro pitfall

`lib/sys.h` is the vendored mini-runtime header (the flashcartio author's `sys.h`, not
libtonc's). It defines:

```c
// lib/sys.h:66-67
#define EWRAM_CODE __attribute__((section(".ewram"), long_call))
#define EWRAM_BSS  __attribute__((section(".sbss")))
```

- **`EWRAM_BSS`** → static/global variable lands in EWRAM (256 KiB) instead of IWRAM. Used
  for every large buffer, e.g. `source/log.c:12` (`static char EWRAM_BSS s_buf[8192]`).
- **`EWRAM_CODE`** → function body relocated into EWRAM and called via long-call; used
  throughout the EZ-Flash driver (`lib/ezflashomega/io_ezfo.c:11` onward).
- **`ALIGNED`** → 4-byte alignment for DMA buffers. NOT in `sys.h`; it is redefined locally
  per file, e.g. `lib/fatfs/diskio_write.c:20` and `lib/fatfs/diskio.c:13`. Copy it when you
  add a DMA-fed buffer.

**Pitfall — `u8`/`vu8`/`u16`/`vu16`/`u32`/`vu32` are object-like macros, not typedefs:**

```c
// lib/sys.h:59-64
#define u8 unsigned char
#define u16 unsigned short
#define u32 unsigned int
// (+ vu8/vu16/vu32 volatile variants)
```

Consequences: you cannot use `u8` as a struct/field/variable name; a header that includes a
real `<stdint.h>`-style `typedef u8 ...;` will clash; and any other header re-`#define`-ing
these will silently win or break depending on include order. Prefer including `sys.h` once
and treating these names as reserved keywords.

## Memory map (why all the EWRAM fuss)

| Region | Size | Speed | Holds | Rule |
| --- | --- | --- | --- | --- |
| IWRAM | 32 KiB | fast (32-bit, 0-wait) | **the stack**, `*.iwram.c` code | Keep tiny. No big stack buffers. |
| EWRAM | 256 KiB | slower (16-bit bus, wait states) | all big static buffers (`EWRAM_BSS`), `EWRAM_CODE` | Default home for working memory. |
| ROM | up to 32 MiB | slow | default code (Thumb) + rodata | Where plain `.c` ends up. |

The reference Pokemon record-mixer keeps a 128 KiB save buffer, the 8 KiB log buffer
(`source/log.c:12`), a 2 KiB screen scratch, and the name table all in `EWRAM_BSS` for
exactly this reason. **Never declare a multi-KiB array as a local (stack) variable.**

## Pure-C dual-compile for host tests

Format/logic modules (Gen 3 save parsing, checksums, record-mix logic) are written as
plain, freestanding C with no libtonc or GBA-register dependency, so the *same* `.c` files
can be compiled by the host compiler (clang/gcc) into a unit-test binary and run on the
dev machine — no emulator needed. The hardware-only pieces (`sys.h` registers, DMA, tonc
TTE) stay out of those modules. The host test harness itself
(`tests/host_test.c`) and the format modules it drives (`source/gen3_save.*`,
`source/savefile.*`, `source/record_mix.*`) live in the reference impl
(`/Users/guyshtainer/VSCodeProjects/Pokemon mix records tool for GBA`), not in this toolkit.
When you vendor such a module here, preserve the convention: keep it free of `sys.h`/tonc so
it stays host-compilable. See [./gen3-save-format.md](./gen3-save-format.md) and
[./safety-pipeline.md](./safety-pipeline.md).

## Starting a NEW tool from this repo

1. Add a `source/main.c` (vendor the UI/logging template from
   [./ui-and-logging.md](./ui-and-logging.md): `render`, `vsync`, `wait_keys`, `halt_msg`,
   `log_init`/`log_line`/`log_flush_to_sd`).
2. Set `TITLE := YOURTAG` (≤12 chars, the gbafix cart title) and optionally `PROJ :=
   your_tool` for the output filename (`Makefile:76-77`).
3. Drop additional `.c` files into any directory already in `SRCDIRS` — no Makefile edit
   (`Makefile:82,142-145`). Add a brand-new directory? Append it to both `SRCDIRS` and
   `INCDIRS` (`Makefile:82,84`).
4. Use `EWRAM_BSS` for every buffer over a few hundred bytes; name speed-critical files
   `*.iwram.c`.
5. Build with `./build.sh` (Docker) and watch `--print-memory-usage` for IWRAM headroom.

## Sources

External:
- devkitPro devkitARM Docker images — `devkitpro/devkitarm` (image pinned to `:20241104`).
- afska/gba-flashcartio (Makefile + `sys.h` + flashcart drivers origin):
  https://github.com/afska/gba-flashcartio
- felixjones/ezfo-disc_io (EZ-Flash Omega driver, Apache-2.0):
  https://github.com/felixjones/ezfo-disc_io
- ELM-ChaN FatFs (the `lib/fatfs` filesystem layer): https://elm-chan.org/fsw/ff/

Key source files (verified):
- `build.sh` — Docker one-liner build.
- `Makefile` — toolchain, flags, auto-glob, objcopy/gbafix pipeline, iwram rules, specs.
- `lib/sys.h` — `EWRAM_CODE`/`EWRAM_BSS` attributes and the `u8`/`u16`/`u32` macros.
- `lib/fatfs/diskio_write.c`, `lib/fatfs/diskio.c` — local `ALIGNED` macro + EWRAM DMA buffers.
- `source/log.c` — example `EWRAM_BSS` buffer usage.
