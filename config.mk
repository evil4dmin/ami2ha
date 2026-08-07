# ami2ha -- local toolchain configuration.
#
# Everything here can be overridden on the command line or in the
# environment, e.g.:  make VBCC=/opt/amiga CPU=68020
#
# Run tools/setup-toolchain.sh to produce a tree matching these defaults.

# Root of the vbcc installation (contains bin/, config/, targets/).
VBCC        ?= $(HOME)/opt/amiga

# vbcc target configuration. aos68k = AmigaOS 3.x, 68k, standard C library.
VC_CONFIG   ?= aos68k

# Minimum CPU. 68000 keeps stock A500/A600 compatibility; 68020 produces
# noticeably better code if you only care about accelerated machines.
CPU         ?= 68000

# Optimisation level passed to vbcc.
OPT         ?= 2

# Build AmiSSL support for https:// endpoints. Requires the AmiSSL SDK.
# Set to 1 and point AMISSL_SDK at the extracted Developer/ directory.
USE_AMISSL  ?= 0
AMISSL_SDK  ?= $(HOME)/src/amiga-sdks/amissl/AmiSSL/Developer

# Host compiler used for the portable unit tests in tests/.
HOSTCC      ?= cc
