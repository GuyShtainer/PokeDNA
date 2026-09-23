#
# Makefile for gba-record-mixer
# Adapted from afska/gba-flashcartio's tonc template (itself from devkitPro).
# Builds record_mixer.gba with devkitARM + libtonc.
#

# === SETUP ===========================================================

.SUFFIXES:

export TONCLIB := ${DEVKITPRO}/libtonc

export PATH := $(DEVKITARM)/bin:$(PATH)

PREFIX ?= arm-none-eabi-

export CC      := $(PREFIX)gcc
export CXX     := $(PREFIX)g++
export AS      := $(PREFIX)as
export AR      := $(PREFIX)ar
export NM      := $(PREFIX)nm
export OBJCOPY := $(PREFIX)objcopy

# === LINK / TRANSLATE ================================================

%.gba : %.elf
	@$(OBJCOPY) -O binary $< $@
	@echo built ... $(notdir $@)
	@gbafix $@ -t$(TITLE)             # NOTE: do NOT -p pad — EZ-Flash loads the ROM into PSRAM; keep the image small/unpadded
# BELT AND BRACES. Per-variant BUILD dirs already make cross-contamination impossible, but
# this exact failure shipped once: a hardware image linked from emulator objects, wearing
# the correct title, only detectable by running it on the cart. gbafix stamps the title
# from the Makefile, so the title proves NOTHING about the code. Assert on the code.
	@if [ "$(PDNA_TARGET)" != "delta" ] && grep -qa "PokeDNA (emulator build)" $@; then 		echo "*** FATAL: $(notdir $@) is a HARDWARE build but contains emulator-build code."; 		echo "***        Stale objects were linked in. rm -rf build* and rebuild."; 		rm -f $@; exit 1; 	fi
# ROM SELF-CHECK STAMP. This image is ~12.5 MB and this card has a documented history of
# incomplete 12 MB SD loads (projects/rom-load-lab). Post-link, CRC32 the sampled windows
# declared in source/pdna_romver_data.c -- located BY SYMBOL through nm, never by scanning
# for a magic (the verifier's own literal pool is a byte-identical false hit) -- and patch
# them into the descriptor, so the ROM can say "re-copy me" instead of hanging. Anchors
# absent from a build (artless, `make sd`) stamp as "no window": a runtime no-op, NOT a
# false alarm. Runs AFTER gbafix (which only touches 0xA0..0xBD) and AFTER the guard above,
# so a rejected binary is never stamped. Missing python3 is a WARNING, not a fatal, so
# ./build.sh's Docker path can never be broken by an absent interpreter -- the cost is
# visible at boot as "rom self-check: UNSTAMPED", not silent.
	@if command -v python3 >/dev/null 2>&1; then \
	   python3 $(dir $(OUTPUT))tools/stamp_rom_windows.py --elf $< --gba $@ --nm "$(NM)" \
	     || { rm -f $@; exit 1; }; \
	 else \
	   echo "*** WARNING: python3 not found -- $(notdir $@) is UNSTAMPED."; \
	   echo "***          It cannot self-check its own load and will say so at boot."; \
	 fi

%.mb.elf :
	@echo Linking multiboot
	$(LD) -specs=gba_mb.specs $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@
	$(NM) -Sn $@ > $(basename $(notdir $@)).map

%.elf :
	@echo Linking cartridge
	$(LD) -specs=gba.specs $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@
# EWRAM OVERFLOW GUARD. devkitARM's gba_cart.ld declares .sbss with "AT>ewram", which
# constrains only the LOAD address — ld never range-checks the run address, so .sbss
# silently runs off the end of the 256 KiB EWRAM and NOTHING warns. EWRAM then MIRRORS
# every 256 KiB, so the overhang lands on 0x02000000: the EZ-Flash driver's own
# EWRAM-resident code. That shipped once (2026-08) and cost a day: every SD write failed
# because the write bounce buffer was overwriting _EZFO_readSectors and the head of
# _EZFO_writeSectors. Reads still worked, so it looked like a save bug.
# There is no linker-script or _Static_assert way to catch this; it has to be post-link.
	@e=`$(NM) $@ | awk '/ A __eheap_start$$/{print $$1}'`; 	 if [ -z "$$e" ]; then echo "*** FATAL: no __eheap_start symbol"; rm -f $@; exit 1; fi; 	 v=`printf '%d' 0x$$e`; lim=`printf '%d' 0x02040000`; 	 if [ $$v -gt $$lim ]; then 	   echo "*** FATAL: EWRAM OVERFLOW — .sbss/heap starts at 0x$$e, $$(($$v-$$lim)) bytes past"; 	   echo "***        the end of EWRAM (0x02040000). EWRAM mirrors every 256 KiB, so this"; 	   echo "***        corrupts the flashcart driver at 0x02000000 and breaks every SD write."; 	   echo "***        Shrink a static buffer. Largest EWRAM symbols:"; 	   $(NM) -Sn $@ | awk '$$3=="b"||$$3=="B"||$$3=="d"||$$3=="D"' | sort -k2 -r | head -8; 	   rm -f $@; exit 1; 	 fi; 	 echo "  EWRAM ok: $$(($$lim-$$v)) bytes free below 0x02040000"
# STACK BUDGET GUARD. There is one user stack (__sp_usr down to __iheap_start) and no MMU
# guard page: an overrun corrupts the newlib heap and .bss silently. tools/stack_budget.py
# walks the .su + objdump call graph from main() (handling Thumb-1's intra-function long
# `bl`, bx-rN indirect thunks, linker veneers, and .constprop/.isra .su name mismatches --
# see its own header) and fails the build if the deepest chain + 64 B of ISR reentry would
# leave less than 1024 B of headroom. PDNA_STACK_CHECK=0 skips it (e.g. a throwaway host
# experiment with no devkitARM objdump on PATH). --builddir is $(CURDIR), not a bare "."
# (BACKLOG #84b D3): this recipe runs inside the $(BUILD)-dir submake, so "." happens to
# be correct today, but an absolute path can never be silently wrong if that ever changes
# -- the guard itself now also refuses a --builddir with zero .su files instead of quietly
# falling back to the prologue estimator for every function.
	@if [ "$(PDNA_STACK_CHECK)" = "0" ]; then \
	   echo "  STACK skip (PDNA_STACK_CHECK=0)"; \
	 else \
	   python3 $(dir $(OUTPUT))tools/stack_budget.py --elf $@ --builddir $(CURDIR) --root main --variant $(PDNA_TARGET) $(if $(filter 1,$(PDNA_ARTLESS)),--artless,) \
	     || { rm -f $@; exit 1; }; \
	   for v in `awk '/^gated /{print $$6}' $(dir $(OUTPUT))tools/stack_edges.txt | sed 's/^via=//' | tr ',' '\n' | sort -u`; do \
	     python3 $(dir $(OUTPUT))tools/stack_budget.py --elf $@ --builddir $(CURDIR) --root $$v --top 1 --variant $(PDNA_TARGET) $(if $(filter 1,$(PDNA_ARTLESS)),--artless,) >/dev/null \
	       || { echo "*** FATAL: a gated subtree reached from $$v exceeds its declared need (re-run --root $$v)"; rm -f $@; exit 1; }; \
	   done; \
	   echo "  GATED ok: `awk '/^gated /{n++}END{print n+0}' $(dir $(OUTPUT))tools/stack_edges.txt` gated subtree(s) re-measured from their via= roots"; \
	 fi
	$(NM) -Sn $@ > $(basename $(notdir $@)).map

%.a :
	@echo $(notdir $@)
	@rm -f $@
	$(AR) -crs $@ $^

# === OBJECTIFY =======================================================

# BACKLOG #110: every object below now carries $(DEPSDIR)/cflags.stamp as a real
# (non-order-only) prerequisite, so a plain `make CFLAGS+=...`-style flag change forces
# a full recompile instead of make's normal incremental logic trusting untouched .c/.o
# mtimes and linking objects built under DIFFERENT flags into one .elf (the exact
# stale-build-dir shape #110's stack-guard fix above refuses to link over). The stamp
# is written once per $(BUILD) invocation, see the $(BUILD) rule further down, and only
# actually touched (new mtime) when its CONTENT changed -- same cmp -s trick as
# pdna_git.h just above it -- so an unrelated touch of one .c file does not force every
# OTHER object to "look" stale too.
%.iwram.o : %.iwram.cpp $(DEPSDIR)/cflags.stamp
	@echo $(notdir $<)
	$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) $(IARCH) -c $< -o $@

%.iwram.o : %.iwram.c $(DEPSDIR)/cflags.stamp
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) $(IARCH) -c $< -o $@

%.o : %.cpp $(DEPSDIR)/cflags.stamp
	@echo $(notdir $<)
	$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) $(RARCH) -c $< -o $@

%.o : %.c $(DEPSDIR)/cflags.stamp
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) $(RARCH) -c $< -o $@

%.o : %.s $(DEPSDIR)/cflags.stamp
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

%.o : %.S $(DEPSDIR)/cflags.stamp
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

export PATH := $(DEVKITARM)/bin:$(PATH)

# === ART FILE INVENTORY / PRESENCE GUARD =============================
#
# The generated, git-ignored art modules (tools/gen_*.py output) that PDNA_ARTLESS=1
# excludes from the build below -- listed by basename (they all live directly in
# source/, except PDNA_ART_EMBED_SFILES which live in source/embed/). This is the
# same 21-file + source/embed/ inventory the old move-aside ritual used
# (docs/analysis-2026-08-18/report-artless-speed.md's now-superseded PART 1), kept
# here as the single source of truth for both the artless filter and check-art below.
PDNA_ART_CFILES := mon_front.c mon_back.c mon_icons.c mon_icons_oam.c \
                    item_icons.c type_icons.c hand_cursor.c hand_oam.c \
                    bag_bg.c card_bg.c pokeblock_bg.c wallpapers.c
PDNA_ART_SFILES := mon_front_data.s mon_icons_data.s mon_icons_oam_data.s \
                    item_icons_data.s bag_bg_data.s card_bg_data.s pokeblock_bg_data.s
PDNA_ART_HFILES  := hand_cursor.h                 # daycare_bg_data.h is listed separately:
PDNA_ART_HEADER_ONLY := daycare_bg_data.h         # it gates via PDNA_NO_DAYCARE_BG, not a filter
PDNA_ART_EMBED_SFILES := mon_back_data.s mon_front_shiny_data.s

# BACKLOG #19's last item (docs/AUDIT-2026-09-05-backlog-3-19.md section B): the 741
# verbatim Game Freak item/move/ability description strings, split out of
# data_tables.c into their own generated/git-ignored file so PDNA_ARTLESS=1 can drop
# them the same way it drops the 21 art files above -- see desc_gate.h /
# data_desc_shim.c. Not merged into PDNA_ART_CFILES: it isn't ripped art, it's text,
# and keeping its own name makes `grep PDNA_DESC` find every piece of this gate.
PDNA_DESC_CFILES := data_desc.c

PDNA_ART_FILES := $(addprefix source/,$(PDNA_ART_CFILES) $(PDNA_ART_SFILES) \
                     $(PDNA_ART_HFILES) $(PDNA_ART_HEADER_ONLY) $(PDNA_DESC_CFILES)) \
                   $(addprefix source/embed/,$(PDNA_ART_EMBED_SFILES))
# check-art (the presence-guard target that uses this list) is defined further down, right
# after `all`/$(BUILD) -- NOT here. This file's default goal is "whichever concrete,
# non-pattern target is defined FIRST", and $(BUILD) must stay that target (it always was);
# putting check-art's own rule ahead of it here would silently make a bare `make` run only
# the guard and build nothing.

# === PROJECT DETAILS =================================================

# 'delta' is the EMULATOR build (Delta / RetroArch on a phone): it has no flashcart
# and no microSD, so it reads and writes its OWN 128 KiB flash save instead of a .sav
# on a card. Different output name so the two never overwrite each other's save.
ifeq ($(PDNA_TARGET),delta)
export PROJ := pokedna-delta
TITLE       := PokeDNADLT
else ifeq ($(PDNA_TARGET),sd)
# Its own name so it can never overwrite the full NOR build — you want both on the card.
export PROJ := PokeDNA-SD
TITLE       := PokeDNASD
else
export PROJ := PokeDNA
TITLE       := PokeDNA
endif

# --- artless variant: same PROJ/TITLE scheme as above, "-artless"/short-suffixed. Composes
# with every PDNA_TARGET above -- see the `artless`/`sd-artless`/`delta-artless` targets near
# EOF. GBA header titles are a fixed 12-byte field, so the suffix stays short. PDNA_ARTLESS is
# the flag that makes this a first-class build (see SRCDIRS/CFILES/SFILES/CFLAGS below) instead
# of the old ritual of physically moving the ~21 generated art files out of source/ and back.
ifeq ($(strip $(PDNA_ARTLESS)),1)
ifeq ($(PDNA_TARGET),delta)
export PROJ := pokedna-delta-artless
TITLE       := PokeDNADltA
else ifeq ($(PDNA_TARGET),sd)
export PROJ := PokeDNA-SD-artless
TITLE       := PokeDNASdA
else
export PROJ := PokeDNA-artless
TITLE       := PokeDNAArt
endif
endif

# --- telemetry-free variant: its own OUTPUT NAME, not just its own object directory.
# PDNA_PERF=0 already gets a separate BUILD dir (below), but PROJ/TITLE above are functions
# of PDNA_TARGET and PDNA_ARTLESS only -- so `make artless PDNA_PERF=0` used to overwrite
# PokeDNA-artless.gba/.elf/.map with an UNINSTRUMENTED image wearing an identical ROM
# header title. Flash it, run it, and the log simply has no `perf ...` lines in it; the
# obvious conclusion is "the telemetry is broken", not "you flashed the wrong artifact".
# That is exactly the class of failure the BUILD-dir comment below documents ("a hardware
# image linked from emulator objects, wearing the correct title, only detectable by
# running it on the cart"), so it gets the same treatment. GBA header titles are a fixed
# 12-byte field and PokeDNADltA is already 11 chars, so the suffix is ONE character.
ifeq ($(strip $(PDNA_PERF)),0)
export PROJ := $(PROJ)-noperf
TITLE       := $(TITLE)N
endif

LIBS        := -ltonc

# PER-VARIANT build directory. This MUST NOT be shared.
#
# Object files carry no record of the -D flags that produced them, so with one shared
# `build/` a plain `make` after a `make delta` sees up-to-date .o files, skips compiling,
# and links the DELTA objects into PokeDNA.gba — a hardware build that boots as the
# emulator build and asks for a flash save. gbafix still stamps the right title, so the
# only symptom is at runtime on the cart. That shipped; it is why this comment is long.
# Same hazard, same fix, for PDNA_ARTLESS: an artless build's objects must never be reused
# for a full-art link (or vice versa), hence the "-artless" suffix below too.
# ...and PDNA_PERF gets its own suffix for the same reason every other flag does: an
# object built with the counters in and one built without carry no record of which, so a
# shared directory would silently link a half-instrumented binary.
BUILD       := build$(if $(PDNA_TARGET),-$(PDNA_TARGET),)$(if $(filter 1,$(PDNA_ARTLESS)),-artless,)$(if $(filter 0,$(PDNA_PERF)),-noperf,)
SRCDIRS     := source lib lib/fatfs lib/ezflashomega lib/everdrivegbax5
# Build target: 'nor' (default) embeds every sprite (~6.25 MB, run from NOR); 'sd' streams the
# shiny+back blobs from /PokeDNA/sprites.pak so the ROM fits the EZ-Flash SD-mode budget (~<=4 MB).
PDNA_TARGET  ?= nor
# Artless flag: 1 excludes the ~21 generated/git-ignored art modules from THIS BUILD (they stay
# exactly where they are in source/ -- see PDNA_ART_CFILES/PDNA_ART_SFILES below, and the
# check-art guard, which fails loudly if they are ever actually missing from disk instead).
PDNA_ARTLESS ?= 0
ifneq ($(PDNA_TARGET),sd)
SRCDIRS     += source/embed          # NOR: embed the shiny-front + back blobs
endif
ifeq ($(strip $(PDNA_ARTLESS)),1)
SRCDIRS     := $(filter-out source/embed,$(SRCDIRS))   # source/embed/ is 100% art (see below)
endif
DATADIRS    :=          # front-sprite blobs are embedded via .incbin (source/mon_front_data.s), not bin2o
INCDIRS     := source lib lib/fatfs lib/ezflashomega lib/everdrivegbax5
LIBDIRS     := $(TONCLIB)

# --- switches ---

bMB    := 0
bTEMPS := 0
bDEBUG := 0

# === BUILD FLAGS =====================================================

ARCH  := -mthumb-interwork -mthumb
RARCH := -mthumb-interwork -mthumb
IARCH := -mthumb-interwork -marm -mlong-calls

CFLAGS := -mcpu=arm7tdmi -mtune=arm7tdmi -O2 -DFLASHCARTIO_ED_ENABLE=1 -DFLASHCARTIO_EZFO_ENABLE=1
CFLAGS += -Wall
CFLAGS += $(INCLUDE)
CFLAGS += -ffast-math -fno-strict-aliasing
CFLAGS += -fstack-usage         # emits .o-adjacent .su files; tools/stack_budget.py's post-link guard reads them
ifeq ($(PDNA_TARGET),sd)
CFLAGS += -DPDNA_STREAM_SPRITES      # SD build: mon_front/mon_back stream shiny+back from the card
endif
ifeq ($(PDNA_TARGET),delta)
CFLAGS += -DPDNA_DELTA               # emulator build: save is our own 128 KiB flash, no SD
endif
# Artless: the ~21 generated art files stay ON DISK (never moved), so their own
# __has_include probes (mon_icons_gate.h/hand_gate.h/rom_chrome_gate.h) would otherwise find
# them present and wrongly report "art compiled" even though PDNA_ART_CFILES/PDNA_ART_SFILES
# below excluded them from THIS build's OFILES -- undefined references at link time. Every
# one of those gates already documents a `-D` escape hatch for exactly this (host tests use
# it too, see tests/host_romhand_test.c / host_romchrome_test.c); PDNA_NO_DAYCARE_BG is the
# same idea added for pdna_main.c's daycare_bg_data.h probe, which had no such escape before.
ifeq ($(strip $(PDNA_ARTLESS)),1)
CFLAGS += -DPDNA_HAND_ART_COMPILED=0 -DPDNA_MON_ICONS_ART_COMPILED=0 \
          -DPDNA_CARD_ART_COMPILED=0 -DPDNA_POKEBLOCK_ART_COMPILED=0 \
          -DPDNA_BAG_ART_COMPILED=0 -DPDNA_DESC_TEXT_COMPILED=0
# Daycare BG is procedurally generated (tools/gen_daycare_bg.py) from user-supplied images,
# not ROM-derived, so the artless build includes it. Consumer gates via __has_include probe.
# ...and tell the CODE which variant it is, not just which art gates are off, so the
# log's boot line can name the build the user is actually running (source/perf.c).
CFLAGS += -DPDNA_ARTLESS=1
endif

# TELEMETRY SWITCH. 1 (the default) compiles source/perf.c's SD/icon counters, spans and
# rollups in; `make artless PDNA_PERF=0` compiles every one of them away to nothing --
# the call sites stay put, the macros become ((void)0) and the functions become empty
# static inlines, so a release build pays no cycles, no bytes and no log lines. The
# session CLOCK is deliberately outside this switch (perf.h says why): two on-screen
# readouts depend on it, so removing it would not remove telemetry, it would zero them.
PDNA_PERF ?= 1
CFLAGS += -DPDNA_PERF=$(PDNA_PERF)

# BACKLOG #235: pin the build timestamp for PARITY builds. source/pdna_main.c embeds
# `__DATE__ __TIME__` at three sites (the emulator-boot footer + the log's own "build"
# line), so two builds of byte-identical source compiled minutes apart cannot be
# byte-exact -- a real parity sweep (tools/parity_sweep_a4.py) then has to hand-wave
# the resulting diff as "probably the timestamp", which is exactly how a genuine
# regression gets excused. Off by default (`PDNA_PIN_BUILD_STAMP` unset/0): every
# ordinary build keeps the compiler's real __DATE__/__TIME__, which is useful
# diagnostic information nobody wants to lose day to day. A parity run builds BOTH
# sides with `PDNA_PIN_BUILD_STAMP=1` (same fixed literal every time -- the exact
# text doesn't matter, only that it's identical on both sides), which redefines the
# two builtin macros via -D so the ROM's embedded bytes are the same regardless of
# wall-clock build time -- PINNED, never a masked pixel region (#235's own point:
# masking a screen area to make a diff go away can hide a REAL change behind it, the
# opposite of what this checker exists to catch). `make does not track CFLAGS`
# (BUILD IDENTITY comment above, same caveat): a parity build must be a CLEAN build
# on both sides (`make clean` first, or a fresh worktree/objdir) so an incremental
# .o left over from an unpinned build never carries a stale timestamp forward.
PDNA_PIN_BUILD_STAMP ?= 0
ifeq ($(strip $(PDNA_PIN_BUILD_STAMP)),1)
CFLAGS += -D__DATE__='"Jan  1 2026"' -D__TIME__='"00:00:00"'
endif

# BUILD IDENTITY. __DATE__/__TIME__ alone cannot tell two builds of the same afternoon
# apart, and "is that log from the old binary?" has already cost this project a
# debugging session. The short hash + a '+' when the tree is dirty goes into the ROM and
# into every log's `perf: git ...` line (source/perf.c). Everything is best-effort: no
# git, a tarball, or Docker without the .git directory all yield an EMPTY string, which
# the code prints as "?" -- a build must never fail for want of a version stamp.
PDNA_GIT       := $(shell git -C "$(CURDIR)" rev-parse --short=8 HEAD 2>/dev/null)
PDNA_GIT_DIRTY := $(shell git -C "$(CURDIR)" diff --quiet HEAD 2>/dev/null || echo +)
# ...delivered as a GENERATED HEADER, not a -D on CFLAGS. make does not track CFLAGS
# changes, and perf.o depends on perf.c/perf.h alone (via -MMD), so a -D went stale on any
# INCREMENTAL build: touch one unrelated .c, and the compile line for THAT file carried the
# new hash while perf.o kept the old one -- the linked ROM then reported a clean tree for a
# binary built from a dirty one, or named the previous commit after a commit. `make artless`
# recurses through `rebuild: clean $(BUILD)` and was safe; the default goal, i.e. the plain
# `make` that builds the full-art comparison binary, was not. A header makes the dependency
# real: $(BUILD)/pdna_git.h is rewritten only when its CONTENT changes (see the $(BUILD)
# rule), so perf.o rebuilds exactly when the stamp actually moves and never otherwise.

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS := $(ARCH) $(INCLUDE)
LDFLAGS := $(ARCH) -Wl,--print-memory-usage,-Map,$(PROJ).map

ifeq ($(strip $(bMB)), 1)
	TARGET := $(PROJ).mb
else
	TARGET := $(PROJ)
endif

ifeq ($(strip $(bTEMPS)), 1)
	CFLAGS   += -save-temps
	CXXFLAGS += -save-temps
endif

ifeq ($(strip $(bDEBUG)), 1)
	CFLAGS   += -DDEBUG -g
	CXXFLAGS += -DDEBUG -g
	ASFLAGS  += -DDEBUG -g
	LDFLAGS  += -g
else
	CFLAGS   += -DNDEBUG
	CXXFLAGS += -DNDEBUG
	ASFLAGS  += -DNDEBUG
endif

# === BUILD PROC ======================================================

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH  := \
	$(foreach dir, $(SRCDIRS) , $(CURDIR)/$(dir)) \
	$(foreach dir, $(DATADIRS), $(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

# BACKLOG #109: source/gb_fields.c + gb_flags.c are GENERATED (tools/gen_gbfields.py) and
# git-ignored, but had no Makefile rule -- a checkout could silently build against a
# stale copy (it bit the gbdata merge: 22/170 fresh objects, three core tests failed
# until the generator was re-run by hand). CFILES below is a $(wildcard) evaluated at
# PARSE time, so a target-prerequisite rule (like check-art/check-data-tables further
# down) is too late for a file that doesn't exist on disk yet -- this has to run BEFORE
# the wildcard, hence a $(shell ...) at parse time, not a prerequisite. Runs once per
# top-level `make` invocation (this branch is the OUTER one; the recursive submake in
# $(BUILD) below takes the other branch of this ifneq and never re-triggers it).
# tools/ensure_gbfields.sh is idempotent (a handful of stat(2) calls when the table is
# already fresh), and NEVER fatal for a missing decomp reference (assets/upstream/*/
# symbols/*.sym is git-ignored, local-only material absent on a fresh clone -- it falls
# back to the COMMITTED weak symbols in source/gb_fields_fallback.c / gb_flags_fallback.c
# instead, loudly). It IS fatal when regeneration was actually needed and the generator
# itself failed, so a stale table is never silently kept.
ifeq (,$(filter clean,$(MAKECMDGOALS)))
GBFIELDS_RC := $(strip $(shell ./tools/ensure_gbfields.sh >&2; echo $$?))
ifneq ($(GBFIELDS_RC),0)
$(error tools/ensure_gbfields.sh failed (exit $(GBFIELDS_RC)) -- see the FATAL text above)
endif
endif

CFILES   := $(foreach dir, $(SRCDIRS) , $(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir, $(SRCDIRS) , $(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir, $(SRCDIRS) , $(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir, $(DATADIRS), $(notdir $(wildcard $(dir)/*.*)))

# ARTLESS FILTER. The files themselves are never touched -- this only keeps them out of
# THIS variant's glob result (source/embed/ was already dropped from SRCDIRS above). A
# plain full-art `make` never reaches this branch, so PokeDNA.gba's OFILES list is
# unchanged by any of this.
ifeq ($(strip $(PDNA_ARTLESS)),1)
CFILES   := $(filter-out $(PDNA_ART_CFILES) $(PDNA_DESC_CFILES),$(CFILES))
SFILES   := $(filter-out $(PDNA_ART_SFILES),$(SFILES))
endif

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES := $(addsuffix .o, $(BINFILES)) \
	$(CFILES:.c=.o) $(CPPFILES:.cpp=.o) \
	$(SFILES:.s=.o)

export INCLUDE := $(foreach dir,$(INCDIRS),-I$(CURDIR)/$(dir)) \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	-I$(CURDIR)/$(BUILD)

export LIBPATHS := -L$(CURDIR) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# check-art only guards the FULL-ART path. A fresh public clone never has the ~21
# gitignored generated art files (they are never committed -- see the "Graphics assets"
# section of README.md), and that is the NORMAL, IP-clean state of the tree, not a defect
# -- so PDNA_ARTLESS=1 ('artless'/'sd-artless'/'delta-artless') must build with none of
# them present, without ever consulting this guard.
ifneq ($(strip $(PDNA_ARTLESS)),1)
$(BUILD): check-art
endif
$(BUILD): check-data-tables
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@printf '%s\n' '/* GENERATED by the Makefile (see PDNA_GIT). Do not edit, do not commit. */' \
	                '#define PDNA_GIT_HASH "$(PDNA_GIT)$(PDNA_GIT_DIRTY)"' \
	                '#define PDNA_BUILD_DIR "$(BUILD)"' > $@/pdna_git.h.tmp
	@cmp -s $@/pdna_git.h.tmp $@/pdna_git.h 2>/dev/null && rm -f $@/pdna_git.h.tmp \
	  || mv -f $@/pdna_git.h.tmp $@/pdna_git.h
# BACKLOG #110: cflags.stamp is the FLAGS side of the same staleness class pdna_git.h
# guards on the identity side. Written once per $(BUILD) invocation, content-compared
# (cmp -s, same trick as pdna_git.h above) so its mtime only moves when CFLAGS/CXXFLAGS/
# ASFLAGS actually changed -- every %.o/%.iwram.o pattern rule up top now carries
# $(DEPSDIR)/cflags.stamp as a real prerequisite, so a moved stamp forces every object
# to recompile, and an untouched one forces nothing extra.
	@printf '%s\n' "$(CFLAGS)" "$(CXXFLAGS)" "$(ASFLAGS)" > $@/cflags.stamp.tmp
	@cmp -s $@/cflags.stamp.tmp $@/cflags.stamp 2>/dev/null && rm -f $@/cflags.stamp.tmp \
	  || mv -f $@/cflags.stamp.tmp $@/cflags.stamp
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

all : $(BUILD)

# FULL-ART builds only. A full-art `make`/`make sd`/`make delta` links the ~21 generated
# art modules by name (PDNA_ART_FILES above) and produces a silently artless binary under
# the full-art name if even one is missing -- caught only by the "N/17 windows stamped"
# self-check line at boot, which is far too late. This target exists to catch exactly one
# incident: an agent moved the art files aside to build artless the old way, died
# mid-run, and left them missing for every subsequent full-art build. It is NOT the state
# of a normal clone, which never has these files at all -- that tree wants `make artless`
# (below), not this guard, which is why PDNA_ARTLESS=1 skips it entirely above. Defined
# AFTER `all`/$(BUILD) on purpose -- see the comment where PDNA_ART_FILES is built.
.PHONY: check-art
check-art:
	@missing=""; \
	for f in $(PDNA_ART_FILES); do [ -e "$$f" ] || missing="$$missing $$f"; done; \
	if [ -n "$$missing" ]; then \
	  echo "*** FATAL: this is a full-art/full-text build ($(PROJ)) but generated"; \
	  echo "***        sources are missing from source/ (art AND/OR data_desc.c --"; \
	  echo "***        the latter is the 741 verbatim description STRINGS, not art,"; \
	  echo "***        but it fails the SAME way for the SAME reason: PDNA_ARTLESS=1"; \
	  echo "***        would have excluded it on purpose; a full build silently"; \
	  echo "***        missing it is the failure this guard exists to catch):"; \
	  for f in $$missing; do echo "***          $$f"; done; \
	  echo "*** Two ways forward:"; \
	  echo "***   1) Generate it: see README.md's 'Graphics assets' section --"; \
	  echo "***      each tools/gen_*.py regenerates one file (data_desc.c comes from"; \
	  echo "***      tools/gen_data.py, same as data_tables.c); see its own --help."; \
	  echo "***   2) Skip it entirely: 'make artless' (or 'make sd-artless' /"; \
	  echo "***      'make delta-artless') builds without needing any of these files,"; \
	  echo "***      including on a fresh clone that never had them."; \
	  exit 1; \
	fi

.PHONY: check-data-tables
check-data-tables:
	@[ -e source/data_tables.c ] || { echo "*** source/data_tables.c is missing: run  tools/gen_data.py  (it is generated + gitignored; needed by EVERY build variant)"; exit 1; }

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).gba $(TARGET).sav

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).gba : $(OUTPUT).elf
$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

endif

# $(BUILD) must be PHONY (the devkitPro default). Without it make sees an up-to-date
# DIRECTORY and never recurses, so a plain `make` after an edit does nothing and every
# build has to be a full `make rebuild`. With it, the inner make does normal incremental
# compilation and only the touched objects are rebuilt.
.PHONY: $(BUILD) all clean rebuild sd delta artless sd-artless delta-artless retail-gate loc-layout delta-gb stack-check
rebuild:
	@$(MAKE) clean
	@$(MAKE) $(BUILD)

sd:                    # trimmed build that streams shiny/back sprites from /PokeDNA/sprites.pak
	@$(MAKE) PDNA_TARGET=sd rebuild
	@echo ""
	@echo "  PokeDNA-SD.gba built — small enough to load from the SD menu reliably."
	@echo "  (Fragmentation risk scales with file size; the full build is ~12 MB.)"

delta:                 # emulator build (Delta / RetroArch): edits its OWN 128 KiB flash save.
	@$(MAKE) PDNA_TARGET=delta rebuild
	@echo ""
	@echo "  pokedna-delta.gba built."
	@echo "  Put it in your emulator, then copy your Pokemon .sav over pokedna-delta.sav."
	@echo "  Save type must be Flash 1Mbit (128K). NO BACKUPS in this build."

# BACKLOG #62: the delta build, fused with a Gen-3 save AND a whole Game Boy corpus
# (ROMs + saves) so the GB half of PokeDNA has real data to read on an emulator with no
# SD card. GB_ROMS/EMERALD_SAV point at Guy's own local dumps by default (never in this
# repo -- see docs/kb/pokemon/, roms.sh) and can be overridden: `make delta-gb
# GB_ROMS=/path/to/gb EMERALD_SAV=/path/to/Emerald.sav`. LOCAL-USE ONLY: the output
# embeds a personal save and commercial ROMs -- never commit, publish, or share it
# (fuse_rom.py/fuse_sav.py/fuse_gb.py all say this too; it bears repeating here since
# this is the one target that runs all three back to back).
GB_ROMS ?= $(HOME)/VSCodeProjects/gba-toolkit/roms
EMERALD_SAV ?= $(HOME)/VSCodeProjects/gba-toolkit/roms/Emerald.sav

# BACKLOG #68a: this is now the ONLY GB-corpus recipe -- pdna_main.c's boot-time
# gb_delta_boot_pick() offers a picker over BOTH the fused Emerald.sav and the fused
# GB corpus in one image, so the old blank-flash `delta-gb-only` target (which existed
# only to reach the full read/write standalone GB mount with no Gen-3 save in the way)
# is retired: picking a GB row off this same image's boot picker reaches that exact
# mount now. Verified by review to boot to the picker, then either continue into the
# Emerald session or PICK A SAVE -> VIEW/EDIT/LEGALITY/MOVE TO BOX/COPY/RELEASE/CREATE,
# backing out (B) reloading Emerald from flash and re-showing the same picker.
# #68b review D3: the ARM-side half of tests/host_gbloc_layout_test.c's layout proof
# (RomGbSpriteLoc/RomGbIconLoc/RomGbUiLoc offsetof/sizeof _Static_assert lines) --
# -fsyntax-only, no link, no output file. The host half already runs in
# tests/run_host_tests.py; this catches the same struct drifting on the OTHER
# compiler that actually reads the fused LOC bytes on real hardware/an emulator.
# Standalone target so it can be run on its own; also gated INTO delta-gb below so a
# layout drift fails that build instead of silently mis-shaping every LOC payload it
# fuses.
loc-layout:
	@"$(DEVKITARM)/bin/$(CC)" -mcpu=arm7tdmi -mthumb -mthumb-interwork -I source \
	  -fsyntax-only tests/host_gbloc_layout_test.c
	@echo "  loc-layout ok: RomGbSpriteLoc/RomGbIconLoc/RomGbUiLoc match on ARM"

delta-gb: loc-layout    # 'delta' fused with Red/Gold/Crystal (ROM+save) + Emerald.sav
	@$(MAKE) PDNA_TARGET=delta rebuild
	@python3 tools/fuse_sav.py pokedna-delta.gba $(EMERALD_SAV) -o pokedna-delta-gb.gba --force
	@python3 tools/fuse_gb.py pokedna-delta-gb.gba \
	  $(GB_ROMS)/gb/Red.gb $(GB_ROMS)/gb/Red.sav \
	  $(GB_ROMS)/gb/Gold.gbc $(GB_ROMS)/gb/Gold.sav \
	  $(GB_ROMS)/gb/Crystal.gbc $(GB_ROMS)/gb/Crystal.sav \
	  -o pokedna-delta-gb.gba --force
	@python3 tools/fuse_gb.py --check pokedna-delta-gb.gba
	@echo ""
	@echo "  pokedna-delta-gb.gba built -- Emerald.sav seeds the flash save; Red/Gold/"
	@echo "  Crystal (ROM+save each) are readable by the GB half via the boot picker."
	@echo "  Personal-use only: never commit, publish, or share this image."

# --- artless variants -------------------------------------------------------
# Same code, same source/ tree — the ~21 generated art files are excluded from the BUILD
# (PDNA_ARTLESS=1: filtered out of CFILES/SFILES, source/embed/ dropped from SRCDIRS, and
# the PDNA_*_ART_COMPILED / PDNA_NO_DAYCARE_BG gates forced off), never moved, copied, or
# deleted. If a target dies halfway through any of these, the working tree is exactly as
# it was before — that is the entire point. Composes with 'delta'/'sd' above; this is the
# cleanest spelling: bare 'artless' for the hardware/NOR case (the one Guy actually asked
# for), '<target>-artless' for the others.
# NAMING NOTE (review finding, 2026-09-05): PROJ itself is wrong here -- the TOP-level
# `make artless` invocation never sets PDNA_ARTLESS (only the recursive `$(MAKE)
# PDNA_ARTLESS=1 rebuild` below does), so THIS make's own $(PROJ) resolves to the
# full-art name ("PokeDNA"), not the artless one actually produced by the recursive
# build. PDNA_PERF, by contrast, IS visible here (it's a command-line variable that
# propagates to every recursive $(MAKE) automatically, and `make artless
# PDNA_PERF=0` is real usage -- 'noperf' variant docs above), so the basename is
# rebuilt by hand from the same pieces PROJ's own definition uses, mirroring
# PDNA_ARTLESS=1 explicitly instead of trusting the (wrong, here) $(PROJ).
artless:               # hardware build WITHOUT the generated/git-ignored art -> PokeDNA-artless.gba
	@$(MAKE) PDNA_ARTLESS=1 rebuild
	@./tools/check_no_desc_text.sh PokeDNA-artless$(if $(filter 0,$(PDNA_PERF)),-noperf,)
	@echo ""
	@echo "  PokeDNA-artless.gba built — weak fallbacks in source/art_fallbacks.c (+ the"
	@echo "  PDNA_*_ART_COMPILED / PDNA_NO_DAYCARE_BG gates) stand in for every generated"
	@echo "  art module. Not one file in source/ was moved, copied, or deleted to get here."
	@echo "  No Game Freak description text compiled in either (desc_gate.h /"
	@echo "  data_desc_shim.c) -- verified by tools/check_no_desc_text.sh above."

delta-artless:         # emulator build, no compiled art (composes 'delta' + 'artless')
	@$(MAKE) PDNA_TARGET=delta PDNA_ARTLESS=1 rebuild
	@./tools/check_no_desc_text.sh pokedna-delta-artless$(if $(filter 0,$(PDNA_PERF)),-noperf,)
	@echo ""
	@echo "  pokedna-delta-artless.gba built. Same save rules as 'delta' above."

sd-artless:            # SD-streaming build, no compiled art (composes 'sd' + 'artless')
	@$(MAKE) PDNA_TARGET=sd PDNA_ARTLESS=1 rebuild
	@./tools/check_no_desc_text.sh PokeDNA-SD-artless$(if $(filter 0,$(PDNA_PERF)),-noperf,)
	@echo ""
	@echo "  PokeDNA-SD-artless.gba built."

# --- host-side gates (pure C / Python; no ROM build, no GBA hardware) -------
# S4: boot EDITED Gen-1/2 saves in the real ROM and assert on the screen the game drew,
# not on this tree's own parser (tools/gb_retail_gate.py). Not part of
# tests/run_host_tests.py's default loop -- that script's tests are near-instant pure C;
# this one is ~30 emulator boots.
# The interpreter and --mgba-vendor path below are MACHINE-SPECIFIC (Guy's Mac, where
# libmgba-py is vendored under rec2mp4/ and /usr/local/bin/python3 is the interpreter
# that can import it -- the system python3 has neither mgba nor markdown). On another
# machine, run `python3 tools/gb_retail_gate.py --mgba-vendor <path>` directly instead.
retail-gate:   # per-worktree scratch: the shared default raced between parallel lanes (BACKLOG #101)
	@/usr/local/bin/python3 tools/gb_retail_gate.py \
		--mgba-vendor /Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/rec2mp4/vendor \
		--scratch /tmp/gb_retail_gate-$(notdir $(CURDIR))

# STACK-BUDGET GUARD SELF-TEST (BACKLOG #84b). Unit tests for tools/stack_budget.py
# itself (tests/host_stack_budget_test.py -- not picked up by run_host_tests.py, which
# only builds/runs the C tests), then a re-run of the real guard against every ELF this
# tree already has built, so "did I break the guard" and "does today's build still pass
# it" are one command.
stack-check:
	@python3 tests/host_stack_budget_test.py
	@for e in PokeDNA-artless.elf:build-artless:nor:--artless PokeDNA.elf:build:nor: pokedna-delta.elf:build-delta:delta:; do \
		elf=$${e%%:*}; rest=$${e#*:}; bd=$${rest%%:*}; rest=$${rest#*:}; va=$${rest%%:*}; al=$${rest##*:}; \
		if [ -f "$$elf" ] && [ -d "$$bd" ]; then \
			echo "-- $$elf --"; \
			python3 tools/stack_budget.py --elf "$$elf" --builddir "$(CURDIR)/$$bd" --root main --variant "$$va" $$al || exit 1; \
			for v in `awk '/^gated /{print $$6}' tools/stack_edges.txt | sed 's/^via=//' | tr ',' '\n' | sort -u`; do \
				python3 tools/stack_budget.py --elf "$$elf" --builddir "$(CURDIR)/$$bd" --root "$$v" --top 1 --variant "$$va" $$al >/dev/null \
					|| { echo "*** FATAL: a gated subtree reached from $$v exceeds its declared need (re-run --root $$v)"; exit 1; }; \
			done; \
	   echo "  GATED ok: `awk '/^gated /{n++}END{print n+0}' $(dir $(OUTPUT))tools/stack_edges.txt` gated subtree(s) re-measured from their via= roots"; \
		else \
			echo "-- $$elf -- skipped (not built)"; \
		fi; \
	done

# EOF
