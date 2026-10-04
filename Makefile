.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := 14CFW
BUILD    := build
SOURCES  := .
DATA     :=
INCLUDES := .

APP_TITLE   := 14CFW
APP_AUTHOR  := Amir
APP_VERSION := 1.1.0

ifeq ($(strip $(ICON)),)
icons := $(wildcard *.jpg)
ifneq (,$(findstring $(TARGET).jpg,$(icons)))
export APP_ICON := $(TOPDIR)/$(TARGET).jpg
else
ifneq (,$(findstring icon.jpg,$(icons)))
export APP_ICON := $(TOPDIR)/icon.jpg
endif
endif
else
export APP_ICON := $(TOPDIR)/$(ICON)
endif

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS   += $(INCLUDE) -D__SWITCH__ `$(PORTLIBS_PATH)/switch/bin/sdl2-config --cflags` `$(PKG_CONFIG) --cflags libcurl SDL2_ttf`
CXXFLAGS := $(CFLAGS) -fno-rtti -fexceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

PKG_CONFIG := $(PORTLIBS_PATH)/switch/bin/aarch64-none-elf-pkg-config
LIBS := `$(PKG_CONFIG) --libs --static SDL2_ttf libcurl` `$(PORTLIBS_PATH)/switch/bin/sdl2-config --libs` -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export BUILD_EXEFS_SRC := $(TOPDIR)/$(EXEFS_SRC)
.PHONY: $(BUILD) clean all
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
.PHONY: all
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
