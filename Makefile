#
# Makefile for gba-record-mixer
# Adapted from afska/gba-flashcartio's tonc template (itself from devkitPro).
# Builds PokeLinkSim.gba with devkitARM + libtonc.
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
	@gbafix $@ -t$(TITLE)

%.mb.elf :
	@echo Linking multiboot
	$(LD) -specs=gba_mb.specs $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@
	$(NM) -Sn $@ > $(basename $(notdir $@)).map

%.elf :
	@echo Linking cartridge
	$(LD) -specs=gba.specs $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@
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

export PROJ := PokeLinkSim
TITLE       := PokeLinkSim

LIBS        := -ltonc

BUILD       := build
SRCDIRS     := source lib lib/fatfs lib/ezflashomega lib/everdrivegbax5
DATADIRS    := data
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

.PHONY: clean rebuild
rebuild: clean $(BUILD)

# EOF
