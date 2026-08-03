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
	$(NM) -Sn $@ > $(basename $(notdir $@)).map

%.a :
	@echo $(notdir $@)
	@rm -f $@
	$(AR) -crs $@ $^

# === OBJECTIFY =======================================================

%.iwram.o : %.iwram.cpp
	@echo $(notdir $<)
	$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) $(IARCH) -c $< -o $@

%.iwram.o : %.iwram.c
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) $(IARCH) -c $< -o $@

%.o : %.cpp
	@echo $(notdir $<)
	$(CXX) -MMD -MP -MF $(DEPSDIR)/$*.d $(CXXFLAGS) $(RARCH) -c $< -o $@

%.o : %.c
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d $(CFLAGS) $(RARCH) -c $< -o $@

%.o : %.s
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

%.o : %.S
	@echo $(notdir $<)
	$(CC) -MMD -MP -MF $(DEPSDIR)/$*.d -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

export PATH := $(DEVKITARM)/bin:$(PATH)

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

LIBS        := -ltonc

# PER-VARIANT build directory. This MUST NOT be shared.
#
# Object files carry no record of the -D flags that produced them, so with one shared
# `build/` a plain `make` after a `make delta` sees up-to-date .o files, skips compiling,
# and links the DELTA objects into PokeDNA.gba — a hardware build that boots as the
# emulator build and asks for a flash save. gbafix still stamps the right title, so the
# only symptom is at runtime on the cart. That shipped; it is why this comment is long.
BUILD       := build$(if $(PDNA_TARGET),-$(PDNA_TARGET),)
SRCDIRS     := source lib lib/fatfs lib/ezflashomega lib/everdrivegbax5
# Build target: 'nor' (default) embeds every sprite (~6.25 MB, run from NOR); 'sd' streams the
# shiny+back blobs from /PokeDNA/sprites.pak so the ROM fits the EZ-Flash SD-mode budget (~<=4 MB).
PDNA_TARGET ?= nor
ifneq ($(PDNA_TARGET),sd)
SRCDIRS     += source/embed          # NOR: embed the shiny-front + back blobs
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
ifeq ($(PDNA_TARGET),sd)
CFLAGS += -DPDNA_STREAM_SPRITES      # SD build: mon_front/mon_back stream shiny+back from the card
endif
ifeq ($(PDNA_TARGET),delta)
CFLAGS += -DPDNA_DELTA               # emulator build: save is our own 128 KiB flash, no SD
endif

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

CFILES   := $(foreach dir, $(SRCDIRS) , $(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir, $(SRCDIRS) , $(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir, $(SRCDIRS) , $(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir, $(DATADIRS), $(notdir $(wildcard $(dir)/*.*)))

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

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

all : $(BUILD)

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
.PHONY: $(BUILD) all clean rebuild sd delta
rebuild: clean $(BUILD)

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

# EOF
