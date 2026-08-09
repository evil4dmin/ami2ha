/*
 * vbcc 0.9h, m68k-amigaos target: register parameters of inline-assembly
 * functions are not placed in their declared registers at -O=2.
 *
 *   vc +aos68k -c99 -cpu=68020 -O=1 -S bug.c   # correct
 *   vc +aos68k -c99 -cpu=68020 -O=2 -S bug.c   # a6 never loaded
 */
#include <proto/dos.h>

long test(void)
{
    return (long)Output();
}
