# vbcc 0.9h / m68k-amigaos: register parameters ignored at `-O=2`

A report, ready to send to Volker Barthelmann (`vb@compilers.de`).

The reproducer is in [`examples/vbcc-bug/`](../examples/vbcc-bug/).

---

## Summary

At `-O=2`, vbcc's m68k backend does not place the register-declared
parameters of inline-assembly functions into their declared registers. It
computes them into stack slots and then executes the inline assembly with
those registers untouched.

On AmigaOS this breaks **every operating system call**, because the whole
OS interface is declared this way — the library base in `a6`, arguments in
`a0`/`a1`/`d0`… Calls execute with whatever happened to be in those
registers.

Nothing warns. The compiler reports no diagnostic, the linker is happy, and
the program crashes somewhere unrelated much later.

## Environment

| | |
|---|---|
| vbcc | V0.9h (source `vbcc0_9hP2.tar.gz`) |
| target | `m68k-amigaos`, config `aos68k`, `vbcc_target_m68k-amigaos.lha` (2022-05-22) |
| includes | AmigaOS NDK 3.2 `proto/` + vbcc's own `inline/` |
| host | macOS 15 (Darwin 25.6.0) arm64, Apple clang 21 |
| tested on | AmigaOS 3.2 (Kickstart 47.13), 68060, under FS-UAE |

## Reproducer

```c
#include <proto/dos.h>

long test(void)
{
    return (long)Output();
}
```

```
vc +aos68k -c99 -cpu=68020 -O=1 -S bug.c    # correct
vc +aos68k -c99 -cpu=68020 -O=2 -S bug.c    # wrong
```

`Output()` is declared in `inline/dos_protos.h` as an inline-assembly
function whose first parameter is bound to `a6`:

```c
BPTR __Output(__reg("a6") void *)="\tjsr\t-60(a6)";
#define Output() __Output(DOSBase)
```

### Correct, `-O=1`

```
_test
        movem.l l3,-(a7)
        move.l  _DOSBase,a6      ; a6 = library base
        jsr     -60(a6)
l3      reg     a6
        movem.l (a7)+,a6
        rts
```

### Wrong, `-O=2`

```
_test
        sub.w   #12,a7
        movem.l l3,-(a7)
        move.l  _DOSBase,(0+l5,a7)     ; base -> stack slot
        move.l  (0+l5,a7),(4+l5,a7)    ; ...copied to another slot
        jsr     -60(a6)                ; a6 never loaded
        move.l  d0,(8+l5,a7)
        move.l  (8+l5,a7),d0
l3      reg                            ; note: empty save list
l5      equ     0
        add.w   #12,a7
        rts
```

`a6` is never written, and the register save list `l3` is empty — the
backend does not consider `a6` used at all.

With arguments the same thing happens to them. `OpenLibrary`, declared

```c
struct Library *__OpenLibrary(__reg("a6") void *, __reg("a1") CONST_STRPTR,
                              __reg("d0") ULONG)="\tjsr\t-552(a6)";
```

compiles at `-O=2` to six stack stores followed by `jsr -552(a6)` with
`a6`, `a1` and `d0` all untouched, so it returns NULL for a library that is
present and open elsewhere in the same program.

## Which `-O` values are affected

`-O` is a bitmask, and only some combinations misbehave:

```
-O=0  ok      -O=4   ok      -O=64   ok
-O=1  ok      -O=5   ok      -O=128  ok
-O=2  BROKEN  -O=6   BROKEN  -O=256  ok
-O=3  ok      -O=7   ok      -O=512  ok
```

The pattern is exact: **bit 1 set while bit 0 is clear**. Setting bit 0 as
well (`-O=3`, `-O=7`) produces correct code again.

That matches what the manual says about bit 1:

> After intermediate code for the whole function has been generated, simple
> register allocation may be done in non-optimizing compilation if bit 1 has
> been set in the `-O` option.

So the fault appears to be in the simple register allocator used on the
non-optimizing path, which does not honour `__reg` parameter bindings.
Enabling the optimizing path as well hides it.

This matters because `-O=2` is a natural thing to write, and is what several
build systems and tutorials use.

## Workaround

Build with `-O=1`. Any value with bit 0 set also works.

## How it turned up

Writing a Home Assistant client for AmigaOS. `OpenLibrary("bsdsocket.library", 4)`
returned NULL while the library was demonstrably present — a probe in the
same binary opened it successfully seconds earlier and printed
`UAE bsdsocket.library 4.1`. Elsewhere the program produced Gurus with no
output at all, which is what happens when a program calls into an OS library
with a garbage base pointer.

Isolated by compiling one source file at each optimization level and running
it on real hardware:

```
socket.c at -O=0 : net_lib_open -> 0,  SocketBase=0x40643354, ver=4
socket.c at -O=1 : net_lib_open -> 0,  SocketBase=0x40643354, ver=4
socket.c at -O=2 : net_lib_open -> -1, SocketBase=0x0
```

The generated assembly then showed why.
