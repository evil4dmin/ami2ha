/*
 * ami2ha -- ARexx interface
 *
 * Split in two, like the rest of the project:
 *
 *   rexx_execute()  parses and runs one command line. Portable C99, so the
 *                   whole command set is covered by the host test suite.
 *   rexx_open() ..  the AmigaOS message port that carries those lines.
 *                   Amiga-only, in src/rexx/.
 */
#ifndef AMI2HA_REXX_H
#define AMI2HA_REXX_H

#include <stddef.h>

#include "ami2ha/buf.h"
#include "ami2ha/ha.h"

#define REXX_ERR_MAX 96

/* AmigaDOS-style return codes, as ARexx expects in RC. */
#define REXX_RC_OK     0
#define REXX_RC_WARN   5
#define REXX_RC_ERROR 10
#define REXX_RC_FAIL  20

/*
 * Run one command line. Any result is appended to `out`, which becomes the
 * ARexx RESULT variable. On failure a description is written to `err`,
 * which the port publishes as AMI2HA.LASTERROR.
 *
 * Returns a REXX_RC_* code. *quit is set when the script asked us to exit.
 */
int rexx_execute(ha_client *c, const char *line, a2h_buf *out,
                 char *err, size_t errsz, int *quit);

/* ---- the Amiga side ---- */

typedef struct a2h_rexx a2h_rexx;

/*
 * Create the public message port. `base` is the wanted name, e.g. "AMI2HA";
 * if it is taken, AMI2HA.1, AMI2HA.2 ... are tried, as is conventional.
 * Returns NULL if no port could be created.
 */
a2h_rexx *rexx_open(const char *base);
void      rexx_close(a2h_rexx *r);

/* Signal mask to include in the application's Wait(). 0 if not open. */
unsigned long rexx_sigmask(const a2h_rexx *r);

/* The name the port actually got, for display. */
const char *rexx_portname(const a2h_rexx *r);

/*
 * Handle every message waiting on the port. Returns 0 if a script asked the
 * application to quit.
 */
int rexx_poll(a2h_rexx *r, ha_client *c);

#endif /* AMI2HA_REXX_H */
