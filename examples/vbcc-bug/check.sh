#!/bin/sh
# Show the difference at a glance. Needs vbcc on PATH and $VBCC set.
for O in 1 2; do
    echo "===== -O=$O ====="
    vc +aos68k -c99 -cpu=68020 -O=$O -S -o /tmp/vbccbug.$O.asm bug.c 2>/dev/null
    sed -n '/^_test/,/rts/p' /tmp/vbccbug.$O.asm
    echo
done
echo "At -O=1 the call is preceded by 'move.l _DOSBase,a6'."
echo "At -O=2 it is not, and the register save list is empty."
