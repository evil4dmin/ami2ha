/*
 * ami2ha -- turning a snapshot file into something MUI can show
 *
 * Internal to src/ui. Home Assistant hands us a JPEG, which nothing on
 * AmigaOS can draw directly; datatypes decode and remap it to the screen,
 * and MUI's Bitmap class displays the result.
 *
 * Dtpic.mui would have been less code -- it takes a filename and does all
 * of this itself -- but MUI 3.8's headers declare the class name with an
 * empty attribute block, so MUIA_Dtpic_Name has no definition to compile
 * against. Guessing the value was not worth it when Bitmap.mui and the
 * picture datatype are both fully declared.
 */
#ifndef AMI2HA_UI_CAMPIC_H
#define AMI2HA_UI_CAMPIC_H

#include "ami2ha/compat.h"

#include <stddef.h>

struct Screen;
struct BitMap;

typedef struct {
    APTR            dt;     /* the DTObject, kept while its bitmap is shown */
    struct BitMap  *bm;
    int             width;
    int             height;
} campic;

void campic_init(campic *p);

/*
 * Decode `path` for `scr`. Returns 1 on success, leaving the previous
 * picture untouched on failure so a bad frame does not blank the tile.
 * `err` receives a short reason.
 */
int  campic_load(campic *p, const char *path, struct Screen *scr,
                 char *err, size_t errsz);

/* Release the decoded picture. Safe on a zeroed or already-freed campic. */
void campic_free(campic *p);

#endif /* AMI2HA_UI_CAMPIC_H */
