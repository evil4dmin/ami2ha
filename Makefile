# ami2ha -- Home Assistant client for AmigaOS
#
#   make            build the Amiga binary (build/ami2ha)
#   make test       build and run the portable unit tests on this host
#   make clean      remove build artefacts
#
# Toolchain paths live in config.mk.

include config.mk

# vc locates its target configuration through $VBCC, and shells out to
# vbccm68k / vasmm68k_mot / vlink by bare name, so both must be in the
# environment of every recipe rather than just make variables.
export VBCC
export PATH := $(VBCC)/bin:$(PATH)

VC          := $(VBCC)/bin/vc
BUILD       := build
TARGET      := $(BUILD)/ami2ha

# --- source groups ---------------------------------------------------
#
# core/ is strictly portable C99: no Amiga headers, no OS calls. It is
# compiled both into the Amiga binary and into the host test runner, which
# is what makes the protocol and parsing code testable without an Amiga.
#
# Everything else is Amiga-only and always includes ami2ha/compat.h first.

CORE_SRC    := $(wildcard src/core/*.c)
AMIGA_SRC   := $(wildcard src/net/*.c)   \
               $(wildcard src/ha/*.c)    \
               $(wildcard src/config/*.c) \
               $(wildcard src/rexx/*.c)  \
               $(wildcard src/ui/*.c)    \
               $(wildcard src/*.c)

SRC         := $(CORE_SRC) $(AMIGA_SRC)
OBJ         := $(patsubst %.c,$(BUILD)/%.o,$(SRC))

# --- flags -----------------------------------------------------------
#
CFLAGS      := +$(VC_CONFIG) -c99 -O=$(OPT) -cpu=$(CPU) \
               -Iinclude -I$(VBCC)/targets/m68k-amigaos/include

# vbcc's -dontwarn refuses several message numbers, including the two the
# NDK headers trigger in bulk (53: anonymous union members in
# devices/timer.h, 226: typedef redeclarations against vbcc's C library).
# Neither comes from our code, so they are filtered out of the log here to
# keep real diagnostics visible.
NDK_NOISE   := grep -v -e 'warning 53 in' -e 'warning 226 in' \
                       -e 'included from file' -e '^>' || true

LDFLAGS     := -lamiga -lm

ifeq ($(USE_AMISSL),1)
CFLAGS      += -DA2H_USE_AMISSL=1 -I$(AMISSL_SDK)/include
endif

.PHONY: all clean test dirs

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BUILD)/src/core $(BUILD)/src/net $(BUILD)/src/ha \
	          $(BUILD)/src/config $(BUILD)/src/rexx $(BUILD)/src/ui

$(TARGET): $(OBJ)
	@$(VC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) 2>&1 | $(NDK_NOISE)
	@test -f $@ && echo "built $@"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC $<"
	@$(VC) $(CFLAGS) -c -o $@ $< 2>&1 | $(NDK_NOISE)
	@test -f $@

test:
	@$(MAKE) -C tests run HOSTCC=$(HOSTCC)

clean:
	rm -rf $(BUILD)
	@$(MAKE) -C tests clean
