# Building ami2ha

## Quick start

```sh
./tools/setup-toolchain.sh   # once, ~5-10 minutes
make                         # cross-compile build/ami2ha
make test                    # run the portable core tests on this machine
```

Nothing needs root. Everything lands in `~/opt/amiga` and `~/src/ami2ha-toolchain`.

## What the setup script installs

| Component | Source | Why |
|---|---|---|
| vbcc 0.9hP2 | phoenix.owl.de | The C compiler. Actively maintained by Volker Barthelmann, and the standard choice for AmigaOS. |
| vasm, vlink | sun.hasenbraten.de | Assembler and linker; produce AmigaOS hunk executables directly. |
| vbcc AmigaOS target | phoenix.owl.de | C library, startup code, `amiga.lib`. |
| AmigaOS NDK headers | [sacredbanana/AmigaSDK-gcc](https://github.com/sacredbanana/AmigaSDK-gcc) | `exec/`, `dos/`, `intuition/`, `rexx/`, and the Roadshow network headers. vbcc ships a C library but no OS headers. |
| MUI 3.8 developer includes | [Aminet](https://aminet.net/dev/mui/mui38dev.lha) | `libraries/mui.h` and the muimaster stubs. |
| AmiSSL SDK | [jens-maus/amissl](https://github.com/jens-maus/amissl) | Optional, for `https://` endpoints. |

Note that GCC is *not* used. bebbo's `amiga-gcc`, long the popular choice,
disappeared from GitHub; vbcc is maintained and downloadable, which matters
more for a project meant to be built by other people. The sources avoid
compiler-specific extensions, so adding a GCC path later is a Makefile
change, not a rewrite.

### Two fixes the script applies

Both are worth knowing about if you assemble a toolchain by hand:

- **vbcc must be built with strict `-std=c99`.** In its GNU modes clang
  reserves `asm` as a keyword, and vbcc's `supp.h` uses it as an ordinary
  parameter name.
- **The NDK is merged with `cp -n` (no-clobber).** vbcc's own `proto/` and
  `inline/` headers use its register-argument syntax; the NDK carries the GCC
  flavour. Existing vbcc files must win, with the NDK only filling gaps.
  A plain `cp -R` produces a toolchain that fails to link.

The script also drops a small `sys/errno.h` that includes `<errno.h>`, which
the Roadshow network headers expect but vbcc does not provide.

## Build options

Set these on the command line or edit `config.mk`:

```sh
make CPU=68020        # better code; drops stock A500/A600 support
make OPT=1            # lower optimisation, faster builds
make USE_AMISSL=1     # link AmiSSL for https:// endpoints
make VBCC=/opt/amiga  # toolchain installed somewhere else
```

| Variable | Default | Meaning |
|---|---|---|
| `VBCC` | `~/opt/amiga` | Toolchain root, containing `bin/`, `config/`, `targets/` |
| `VC_CONFIG` | `aos68k` | vbcc target config. `aos68k` = AmigaOS 3.x, standard C library |
| `CPU` | `68000` | Minimum CPU |
| `OPT` | `2` | vbcc optimisation level |
| `USE_AMISSL` | `0` | Build TLS support |
| `HOSTCC` | `cc` | Compiler for the host test runner |

## The portability split

`src/core/` is plain C99 with no Amiga headers and no OS calls. It is
compiled twice: once by the cross-compiler into the Amiga binary, and once by
the host compiler into `tests/run-tests`. Everything else in `src/` is
Amiga-only and includes `ami2ha/compat.h` first.

This is what makes `make test` worth having. Protocol and parsing bugs — the
kind that are miserable to debug through an emulator — surface in about a
second on your development machine, against the same object code semantics
the Amiga will run.

When adding code, ask whether it needs the OS. If it does not, it belongs in
`src/core/` with a test.

## Compiler warnings

vbcc emits two warnings in bulk from third-party headers: 53 (anonymous union
members in the NDK's `devices/timer.h`) and 226 (typedef redeclarations
between the NDK and vbcc's C library). This vbcc build refuses to suppress
either via `-dontwarn`, so the Makefile filters them out of the log. Warnings
from our own code are not filtered.

## Testing on an Amiga

`build/ami2ha` is a standard AmigaOS hunk executable. Copy it to a real
machine, or to an emulator — [vAmiga](https://vamiga.me) (`brew install
vamiga`) and FS-UAE both work. You will need MUI and a TCP/IP stack installed
in the emulated environment.

## Making a release

```
make dist
```

This builds both CPU variants, stages `dist/ami2ha-<version>/ami2ha/`
with the binaries, the AmigaGuide manual, the Installer script, an
example configuration and the licence, and packs it as a `.lha`.

The version comes from `include/ami2ha/version.h`, which is also what
the window title, the About box and the ARexx `VERSION` command read, so
they cannot disagree.

**Packing needs an archiver that can create LhA archives.** The `lha` in
Homebrew is Lhasa, which only extracts; `make dist` notices, leaves the
staged drawer in place, and tells you. The simplest way round it is to
pack on the Amiga itself, which is also what produces the most
Amiga-correct archive:

```
LhA -r a ami2ha-0.1.lha ami2ha
```

Verify the result extracts into a drawer rather than loose files:

```
LhA x ami2ha-0.1.lha
```

### Before publishing one

Worth doing on a real system, because it is what everybody else will do
first:

- run `Installer Install` from inside the unpacked drawer, at the
  default Novice level as well as Average — Novice answers every
  question from its default without showing it
- check the installed drawer gets an `ami2ha.info` it can be started from
- open `ami2ha.guide` in MultiView and follow a couple of links
