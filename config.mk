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
#
# MUST NOT be raised to 2 with vbcc 0.9hP2. At -O=2 its m68k backend emits
# library calls without ever loading the argument registers: it computes the
# library base, the name pointer and the version into stack slots and then
# issues `jsr -552(a6)` with a6, a1 and d0 untouched. Every OpenLibrary and
# every library function call in the program is affected, which shows up as
# unexplainable Gurus rather than as anything resembling a compiler fault.
#
# Reproduced by compiling src/net/socket.c at each level and calling
# net_lib_open on real hardware: -O=0 and -O=1 return a valid library base,
# -O=2 returns NULL.
OPT         ?= 1

# Build AmiSSL support for https:// endpoints. Requires the AmiSSL SDK.
# Set to 1 and point AMISSL_SDK at the extracted Developer/ directory.
USE_AMISSL  ?= 0
AMISSL_SDK  ?= $(HOME)/src/amiga-sdks/amissl/AmiSSL/Developer

# Host compiler used for the portable unit tests in tests/.
HOSTCC      ?= cc
