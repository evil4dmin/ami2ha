/*
 * ami2ha -- platform and compiler compatibility layer
 *
 * Include this FIRST in any file that touches AmigaOS APIs. It must come
 * before <sys/socket.h> and friends, because it supplies typedefs those
 * headers expect the C library to have provided.
 *
 * The project targets AmigaOS 3.x/68k first, but everything here is written
 * so the same sources build for AmigaOS 4, MorphOS and AROS. Register and
 * hook conventions come from the SDI headers (public domain, adtools/SDI)
 * rather than being hand-rolled per compiler.
 *
 * Files under src/core/ do NOT include this -- they are plain C99 and are
 * compiled by the host compiler for the unit tests.
 */
#ifndef AMI2HA_COMPAT_H
#define AMI2HA_COMPAT_H

/* ------------------------------------------------------------------ *
 * Platform identification
 * ------------------------------------------------------------------ */

#if defined(__amigaos4__)
#  define A2H_OS4     1
#  define A2H_PLATFORM "AmigaOS 4"
#elif defined(__MORPHOS__)
#  define A2H_MORPHOS 1
#  define A2H_PLATFORM "MorphOS"
#elif defined(__AROS__)
#  define A2H_AROS    1
#  define A2H_PLATFORM "AROS"
#else
#  define A2H_OS3     1
#  define A2H_PLATFORM "AmigaOS 3"
#endif

/* ------------------------------------------------------------------ *
 * Types the Roadshow/AmiTCP network headers expect but vbcc's C library
 * does not define. Must be declared before <sys/socket.h> is pulled in.
 * ------------------------------------------------------------------ */

/*
 * sa_family_t and socklen_t come from <sys/socket.h> itself; only these two
 * are genuinely absent. __uint32_t is what <netinet/in.h> builds in_addr_t
 * from, so without it `struct in_addr` silently stays incomplete.
 */
#if defined(A2H_OS3)
#  ifndef A2H_NET_TYPES_DEFINED
#    define A2H_NET_TYPES_DEFINED
typedef unsigned long __uint32_t;
typedef long          ssize_t;
#  endif
#endif

/* ------------------------------------------------------------------ *
 * OS headers
 * ------------------------------------------------------------------ */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "SDI/SDI_compiler.h"
#include "SDI/SDI_hook.h"

/* ------------------------------------------------------------------ *
 * Integer-sized pointer. AROS and OS4 define IPTR themselves; on classic
 * 68k a plain ULONG is exactly pointer-sized.
 * ------------------------------------------------------------------ */

#if defined(A2H_OS3) && !defined(IPTR)
typedef unsigned long IPTR;
typedef signed long   SIPTR;
#  define IPTR IPTR
#endif

/* ------------------------------------------------------------------ *
 * Small helpers used throughout
 * ------------------------------------------------------------------ */

#ifndef MIN
#  define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#  define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define A2H_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Silence "unused parameter" without depending on compiler attributes. */
#define A2H_UNUSED(x) ((void)(x))

#endif /* AMI2HA_COMPAT_H */
