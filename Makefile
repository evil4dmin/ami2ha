# ami2ha -- Home Assistant client for AmigaOS
#
#   make            build the Amiga binary (build/ami2ha)
#   make test       build and run the portable unit tests on this host
#   make dist       build both CPU variants and pack a release .lha
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

# vbcc has no separate math library: the float helpers live in vc.lib and
# are selected by the target config's -amiga-softfloat.
LDFLAGS     := -lamiga

ifeq ($(USE_AMISSL),1)
CFLAGS      += -DA2H_USE_AMISSL=1 -I$(AMISSL_SDK)/include
endif

# --- release ---------------------------------------------------------
#
# Read from the one place that names a version, so the archive cannot
# disagree with the About box.
# awk rather than sed: a '#' anywhere in a make expression starts a
# comment, which would swallow the rest of the line.
VERSION     := $(shell awk '$$2 == "A2H_VERSION" { gsub(/"/, "", $$3); print $$3 }' \
                       include/ami2ha/version.h)
DISTNAME    := ami2ha-$(VERSION)
DISTDIR     := dist/$(DISTNAME)
DISTLHA     := dist/$(DISTNAME).lha

.PHONY: all clean test dirs dist

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

# Both builds are shipped and the installer picks between them: a 68020
# binary silently fails to run on a 68000, and asking a newcomer which
# processor they have is a poor first impression.
dist:
	@test -n "$(VERSION)" || { echo "cannot read version"; exit 1; }
	@rm -rf $(DISTDIR) $(DISTLHA)
	@mkdir -p $(DISTDIR)/ami2ha
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory CPU=68000 >/dev/null
	@cp $(TARGET) $(DISTDIR)/ami2ha/ami2ha.000
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory CPU=68020 >/dev/null
	@cp $(TARGET) $(DISTDIR)/ami2ha/ami2ha.020
	@cp install/Install       $(DISTDIR)/ami2ha/
	@cp install/ami2ha.guide  $(DISTDIR)/ami2ha/
	@cp LICENSE               $(DISTDIR)/ami2ha/LICENSE
	@cp examples/dashboard.cfg $(DISTDIR)/ami2ha/dashboard.cfg.example
	@sed -e 's/@VERSION@/$(VERSION)/g' install/ReadMe.tmpl \
	     > $(DISTDIR)/ami2ha/ReadMe
	@if command -v lha >/dev/null 2>&1 && \
	    (cd $(DISTDIR) && lha -aq2 ../$(DISTNAME).lha ami2ha >/dev/null 2>&1) && \
	    test -f $(DISTLHA); then \
	  echo "built $(DISTLHA)"; \
	else \
	  echo "staged $(DISTDIR)/ami2ha"; \
	  echo ""; \
	  echo "No archiver that can *create* LhA archives was found."; \
	  echo "The lha in Homebrew is Lhasa, which only extracts."; \
	  echo "Pack it on the Amiga instead:"; \
	  echo "    LhA -r a ami2ha-$(VERSION).lha ami2ha"; \
	fi

clean:
	rm -rf $(BUILD)
	@$(MAKE) -C tests clean
