#!/bin/bash
#
# ami2ha -- one-shot cross-toolchain setup for macOS (Intel and Apple Silicon).
#
# Builds vbcc/vasm/vlink for m68k-amigaos and assembles the SDKs the compiler
# does not ship: the AmigaOS NDK headers, the MUI 3.8 developer includes and
# (optionally) the AmiSSL SDK.
#
# Everything lands under $PREFIX (default ~/opt/amiga) and nothing needs root.
# Safe to re-run: completed steps are skipped.
#
# Linux should work with minor changes (replace the brew step with your
# package manager); it is untested.

set -uo pipefail

PREFIX="${PREFIX:-$HOME/opt/amiga}"
WORK="${WORK:-$HOME/src/ami2ha-toolchain}"
INC="$PREFIX/targets/m68k-amigaos/include"

VBCC_SRC="http://phoenix.owl.de/tags/vbcc0_9hP2.tar.gz"
VASM_SRC="http://sun.hasenbraten.de/vasm/release/vasm.tar.gz"
VLINK_SRC="http://sun.hasenbraten.de/vlink/release/vlink.tar.gz"
TARGET_LHA="http://phoenix.owl.de/vbcc/2022-05-22/vbcc_target_m68k-amigaos.lha"
UNIX_CFG="http://phoenix.owl.de/vbcc/2022-05-22/vbcc_unix_config.tar.gz"
MUI_DEV="https://aminet.net/dev/mui/mui38dev.lha"
NDK_REPO="https://github.com/sacredbanana/AmigaSDK-gcc.git"
NDK_PATH="amigaos3/sdk/m68k-amigaos/ndk-include"

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die()  { printf '\033[31mFATAL: %s\033[0m\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

mkdir -p "$WORK" "$PREFIX/bin" "$PREFIX/config" || die "cannot create $PREFIX"
cd "$WORK" || die "cannot enter $WORK"

fetch() { # url outfile
  [ -s "$2" ] && return 0
  echo "  downloading $(basename "$2")"
  curl -sSL --max-time 300 -o "$2" "$1" || return 1
  [ -s "$2" ]
}

# --------------------------------------------------------------------------
say "1/6  Build dependencies"
# --------------------------------------------------------------------------
if have brew; then
  brew install -q lhasa >/dev/null 2>&1 || true
else
  echo "  Homebrew not found; ensure a C compiler and 'lha' are installed."
fi
have lha || die "lha not found (brew install lhasa)"
have curl || die "curl not found"
have git  || die "git not found"

# --------------------------------------------------------------------------
say "2/6  vasm (m68k, Motorola syntax)"
# --------------------------------------------------------------------------
if [ ! -x "$PREFIX/bin/vasmm68k_mot" ]; then
  fetch "$VASM_SRC" vasm.tar.gz || die "vasm download failed"
  rm -rf vasm && mkdir vasm && tar xzf vasm.tar.gz --strip-components=1 -C vasm
  # -std=gnu89 -w: these are 1990s C sources; modern clang is far stricter.
  ( cd vasm && make CPU=m68k SYNTAX=mot CC="cc -std=gnu89 -w -O2" ) >vasm.log 2>&1
  [ -x vasm/vasmm68k_mot ] || { tail -20 vasm.log; die "vasm build failed"; }
  cp vasm/vasmm68k_mot vasm/vobjdump "$PREFIX/bin/" 2>/dev/null
fi
echo "  ok: $PREFIX/bin/vasmm68k_mot"

# --------------------------------------------------------------------------
say "3/6  vlink"
# --------------------------------------------------------------------------
if [ ! -x "$PREFIX/bin/vlink" ]; then
  fetch "$VLINK_SRC" vlink.tar.gz || die "vlink download failed"
  rm -rf vlink && mkdir vlink && tar xzf vlink.tar.gz --strip-components=1 -C vlink
  ( cd vlink && make CC="cc -std=gnu89 -w -O2" ) >vlink.log 2>&1
  [ -x vlink/vlink ] || { tail -20 vlink.log; die "vlink build failed"; }
  cp vlink/vlink "$PREFIX/bin/"
fi
echo "  ok: $PREFIX/bin/vlink"

# --------------------------------------------------------------------------
say "4/6  vbcc"
# --------------------------------------------------------------------------
if [ ! -x "$PREFIX/bin/vbccm68k" ]; then
  fetch "$VBCC_SRC" vbcc.tar.gz || die "vbcc download failed"
  rm -rf vbcc && mkdir vbcc && tar xzf vbcc.tar.gz --strip-components=1 -C vbcc
  mkdir -p vbcc/bin
  # Strict -std=c99 matters: in GNU modes clang reserves `asm`, which vbcc's
  # supp.h uses as an ordinary parameter name.
  # dtgen asks about the host's integer types; empty lines accept its
  # defaults, which are correct for every modern 64-bit host.
  ( cd vbcc && yes '' | make TARGET=m68k CC="cc -std=c99 -w -O2" ) >vbcc.log 2>&1
  [ -x vbcc/bin/vbccm68k ] || { tail -30 vbcc.log; die "vbcc build failed"; }
  cp vbcc/bin/vbccm68k vbcc/bin/vc vbcc/bin/vprof "$PREFIX/bin/" 2>/dev/null
fi
echo "  ok: $PREFIX/bin/vbccm68k"

# --------------------------------------------------------------------------
say "5/6  AmigaOS target (C library, startup code, vbcc config)"
# --------------------------------------------------------------------------
if [ ! -d "$PREFIX/targets/m68k-amigaos/lib" ]; then
  fetch "$TARGET_LHA" vbcc_target.lha || die "target download failed"
  rm -rf tgt && mkdir tgt && ( cd tgt && lha -xq ../vbcc_target.lha ) 2>/dev/null
  TDIR="$(find tgt -maxdepth 4 -type d -name 'm68k-amigaos' | head -1)"
  [ -n "$TDIR" ] || die "m68k-amigaos target not found in archive"
  mkdir -p "$PREFIX/targets"
  cp -R "$TDIR" "$PREFIX/targets/m68k-amigaos"
fi
if [ ! -f "$PREFIX/config/aos68k" ]; then
  fetch "$UNIX_CFG" vbcc_unix_config.tar.gz || die "config download failed"
  rm -rf cfg && mkdir cfg && tar xzf vbcc_unix_config.tar.gz -C cfg
  find cfg -type f -exec cp {} "$PREFIX/config/" \; 2>/dev/null
fi
echo "  ok: $PREFIX/targets/m68k-amigaos"

# --------------------------------------------------------------------------
say "6/6  SDKs: NDK, MUI, AmiSSL"
# --------------------------------------------------------------------------

# NDK -- vbcc ships a C library but no AmigaOS structure headers.
# -n (no-clobber) matters: vbcc's own proto/ and inline/ headers use its
# register-argument syntax, while the NDK carries the GCC flavour. Existing
# vbcc files must win; the NDK only fills in what is missing.
if [ ! -f "$INC/exec/types.h" ]; then
  if [ ! -d AmigaSDK-gcc/.git ]; then
    git clone --filter=blob:none --sparse --depth 1 "$NDK_REPO" AmigaSDK-gcc >/dev/null 2>&1 \
      || die "NDK clone failed"
  fi
  ( cd AmigaSDK-gcc && git sparse-checkout set "$NDK_PATH" >/dev/null 2>&1 && git checkout -q ) \
    || die "NDK sparse checkout failed"
  [ -f "AmigaSDK-gcc/$NDK_PATH/exec/types.h" ] || die "NDK headers missing"
  cp -Rn "AmigaSDK-gcc/$NDK_PATH"/* "$INC"/ 2>/dev/null
fi
echo "  ok: NDK headers"

# The Roadshow network headers include <sys/errno.h>; vbcc keeps errno.h at
# the top level, so provide the alias they expect.
if [ ! -f "$INC/sys/errno.h" ]; then
  mkdir -p "$INC/sys"
  printf '#ifndef _SYS_ERRNO_H\n#define _SYS_ERRNO_H\n#include <errno.h>\n#endif\n' \
    > "$INC/sys/errno.h"
fi
echo "  ok: sys/errno.h shim"

# MUI 3.8 developer includes.
if [ ! -f "$INC/libraries/mui.h" ]; then
  fetch "$MUI_DEV" mui38dev.lha || die "MUI SDK download failed"
  rm -rf mui && mkdir mui && ( cd mui && lha -xq ../mui38dev.lha ) 2>/dev/null
  MUIINC="$(find mui -type f -name mui.h -path '*libraries*' | head -1)"
  [ -n "$MUIINC" ] || die "MUI includes not found in archive"
  MUIROOT="$(dirname "$(dirname "$MUIINC")")"
  cp -Rn "$MUIROOT"/* "$INC"/ 2>/dev/null
fi
echo "  ok: MUI includes"

# AmiSSL SDK -- optional, only needed for USE_AMISSL=1 (https:// endpoints).
if [ ! -d "$WORK/amissl" ]; then
  URL="$(curl -sS --max-time 60 \
        https://api.github.com/repos/jens-maus/amissl/releases/latest \
        | grep -oE 'https://[^"]*SDK\.lha' | head -1)"
  if [ -n "$URL" ] && fetch "$URL" amissl-sdk.lha; then
    mkdir -p amissl && ( cd amissl && lha -xq ../amissl-sdk.lha ) 2>/dev/null
  fi
fi
if [ -d "$WORK/amissl" ]; then
  echo "  ok: AmiSSL SDK at $WORK/amissl"
else
  echo "  skipped: AmiSSL SDK (optional; plain HTTP still works)"
fi

# --------------------------------------------------------------------------
say "Done"
# --------------------------------------------------------------------------
cat <<EOF
Toolchain prefix : $PREFIX
Verify with      : make && make test

If you installed somewhere other than the default, either export it:
    export VBCC=$PREFIX
or pass it to make:
    make VBCC=$PREFIX
EOF
