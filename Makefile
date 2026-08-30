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
# The vbcc toolchain ships its own, older copy of the AmiSSL headers under
# targets/m68k-amigaos/include. Whichever include path comes first wins, and
# if that is the toolchain's, the SDK's OpenSSL headers end up paired with
# the toolchain's inline stubs. The two disagree about which types are still
# public, and the build dies far away in inline/amissl_protos.h complaining
# about a declaration that is perfectly valid -- so the SDK has to come ahead
# of the toolchain path, not after it.
ifeq ($(USE_AMISSL),1)
AMISSL_INC  := -I$(AMISSL_SDK)/include
endif

CFLAGS      := +$(VC_CONFIG) -c99 -O=$(OPT) -cpu=$(CPU) \
               -Iinclude $(AMISSL_INC) -I$(VBCC)/targets/m68k-amigaos/include

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

# No link library goes with this. The SDK's libamisslstubs.a is an ELF
# archive for gcc; under vbcc proto/amissl.h expands to inline stubs that
# jsr straight through the library base, so there is nothing left to link.
ifeq ($(USE_AMISSL),1)
CFLAGS      += -DA2H_USE_AMISSL=1
endif

# vbcc only discards the unused static inline helpers the OpenSSL headers
# declare when this optimiser bit is set. -O=17375 is plain -O=1 with that
# one bit added (AmiSSL's README-SDK, section 11) -- which matters here,
# because -O=2 miscompiles library calls in this compiler. Without it this
# object alone carries 46 KB of dead functions.
$(BUILD)/src/net/tls.o: CFLAGS := $(subst -O=$(OPT),-O=17375,$(CFLAGS))

# --- release ---------------------------------------------------------
#
# Read from the one place that names a version, so the archive cannot
# disagree with the About box.
# awk rather than sed: a '#' anywhere in a make expression starts a
# comment, which would swallow the rest of the line.
VERSION     := $(shell awk '$$2 == "A2H_VERSION" { gsub(/"/, "", $$3); print $$3 }' \
                       include/ami2ha/version.h)
VERSION_DATE := $(shell awk '$$2 == "A2H_VERSION_DATE" { gsub(/"/, "", $$3); print $$3 }' \
                       include/ami2ha/version.h)
DISTNAME    := ami2ha-$(VERSION)
DISTDIR     := dist/$(DISTNAME)
DISTLHA     := dist/$(DISTNAME).lha

.PHONY: all clean test dirs dist version-check

all: dirs $(TARGET)

# Nothing outside version.h should name a version. 0.1.1 shipped with a
# guide that still said 0.1, because three files carried it by hand; the
# templates fixed those, but a stray archive name in the README would go
# stale just as quietly and nothing would say so.
#
# The Aminet Replaces: line is exempt: it names the release this one
# supersedes, which is by definition not $(VERSION).
version-check:
	@test -n "$(VERSION)" || { echo "cannot read version"; exit 1; }
	@bad=`grep -rn 'ami2ha-[0-9][0-9.]*\.lha' README.md docs install 2>/dev/null \
	      | grep -v 'Replaces:' | grep -v 'ami2ha-$(VERSION)\.lha'`; \
	 if [ -n "$$bad" ]; then \
	   echo "version-check: these still name another version, expected $(VERSION):"; \
	   echo "$$bad"; \
	   exit 1; \
	 fi
	@echo "version-check: everything agrees on $(VERSION)"


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
dist: version-check
	@test -n "$(VERSION)" || { echo "cannot read version"; exit 1; }
	@rm -rf $(DISTDIR) $(DISTLHA)
	@mkdir -p $(DISTDIR)/ami2ha/icons
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory CPU=68000 >/dev/null
	@cp $(TARGET) $(DISTDIR)/ami2ha/ami2ha.000
	@$(MAKE) --no-print-directory clean >/dev/null
	@$(MAKE) --no-print-directory CPU=68020 >/dev/null
	@cp $(TARGET) $(DISTDIR)/ami2ha/ami2ha.020
	@sed -e 's/@VERSION@/$(VERSION)/g' install/Install.tmpl \
	     > $(DISTDIR)/ami2ha/Install
	@sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@VERSION_DATE@/$(VERSION_DATE)/g' \
	     install/ami2ha.guide.tmpl > $(DISTDIR)/ami2ha/ami2ha.guide
	@# Icons. The drawer icon sits beside the drawer, not in it, or
	@# Workbench has nothing to show once the archive is unpacked. The
	@# program's own icon waits in icons/ until the installer places it;
	@# left loose it would show as an icon with no program behind it.
	@python3 install/icon/mkinfo.py $(DISTDIR)/ami2ha/icons >/dev/null
	@mv $(DISTDIR)/ami2ha/icons/Install_project.info $(DISTDIR)/ami2ha/Install.info
	@mv $(DISTDIR)/ami2ha/icons/ami2ha_drawer.info   $(DISTDIR)/ami2ha.info
	@mv $(DISTDIR)/ami2ha/icons/ami2ha_tool.info     $(DISTDIR)/ami2ha/icons/ami2ha.info
	@# The manual and the ReadMe get theirs beside the files themselves
	@# rather than waiting in icons/: both are worth reading before
	@# deciding to install anything, and without an icon neither can be
	@# opened from Workbench at all.
	@mv $(DISTDIR)/ami2ha/icons/ami2ha_guide.info    $(DISTDIR)/ami2ha/ami2ha.guide.info
	@mv $(DISTDIR)/ami2ha/icons/ReadMe_project.info  $(DISTDIR)/ami2ha/ReadMe.info
	@cp LICENSE               $(DISTDIR)/ami2ha/LICENSE
	@cp examples/dashboard.cfg $(DISTDIR)/ami2ha/dashboard.cfg.example
	@sed -e 's/@VERSION@/$(VERSION)/g' install/ReadMe.tmpl \
	     > $(DISTDIR)/ami2ha/ReadMe
	@# Aminet wants the readme uploaded beside the archive, not inside
	@# it, and named to match. Keeping it out of the archive also keeps
	@# the archive byte-identical wherever it is published, so one
	@# checksum covers every mirror.
	@sed -e 's/@VERSION@/$(VERSION)/g' install/aminet.readme.tmpl \
	     > dist/$(DISTNAME).readme
	@if command -v lha >/dev/null 2>&1 && \
	    (cd $(DISTDIR) && lha -aq2 ../$(DISTNAME).lha ami2ha ami2ha.info >/dev/null 2>&1) && \
	    test -f $(DISTLHA); then \
	  echo "built $(DISTLHA)"; \
	else \
	  echo "staged $(DISTDIR)/ami2ha"; \
	  echo ""; \
	  echo "No archiver that can *create* LhA archives was found."; \
	  echo "The lha in Homebrew is Lhasa, which only extracts."; \
	  echo "Pack it on the Amiga instead:"; \
	  echo "    LhA -r a ami2ha-$(VERSION).lha ami2ha ami2ha.info"; \
	fi

clean:
	rm -rf $(BUILD)
	@$(MAKE) -C tests clean
