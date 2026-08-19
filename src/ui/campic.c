/* ami2ha -- turning a snapshot file into something MUI can show */
#include "ami2ha/compat.h"

#include "campic.h"

#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <intuition/screens.h>

#include <proto/datatypes.h>
#include <proto/intuition.h>
#include <proto/utility.h>

#include <stdio.h>
#include <string.h>

struct Library *DataTypesBase = NULL;

void campic_init(campic *p)
{
    p->dt     = NULL;
    p->bm     = NULL;
    p->width  = 0;
    p->height = 0;
}

void campic_free(campic *p)
{
    if (p->dt) {
        /* The bitmap belongs to the DTObject, so it must not be touched
         * after this and MUI must have been pointed elsewhere first. */
        DisposeDTObject((Object *)p->dt);
        p->dt = NULL;
    }
    p->bm     = NULL;
    p->width  = 0;
    p->height = 0;
}

int campic_load(campic *p, const char *path, struct Screen *scr,
                char *err, size_t errsz)
{
    Object              *dt;
    struct BitMapHeader *bmhd = NULL;
    struct BitMap       *bm   = NULL;
    campic               fresh;

    if (err && errsz)
        err[0] = '\0';

    if (!DataTypesBase) {
        DataTypesBase = OpenLibrary("datatypes.library", 39);
        if (!DataTypesBase) {
            if (err) snprintf(err, errsz, "datatypes.library not available");
            return 0;
        }
    }

    /*
     * PDTA_Remap with the screen is what makes this usable on an 8-bit
     * Workbench: the decoder renders into the screen's palette instead of
     * handing back a truecolour bitmap nothing here could blit.
     *
     * A missing JPEG datatype shows up here as a plain failure to create
     * the object, which is the common case on a stock install -- Home
     * Assistant only serves JPEG, so there is nothing to fall back to.
     */
    dt = NewDTObject((APTR)path,
                     DTA_GroupID,      GID_PICTURE,
                     PDTA_Remap,       TRUE,
                     PDTA_Screen,      (IPTR)scr,
                     PDTA_DestMode,    PMODE_V43,
                     TAG_DONE);
    if (!dt) {
        if (err)
            snprintf(err, errsz,
                     "cannot decode the picture (no JPEG datatype?)");
        return 0;
    }

    /* Ask the decoder to lay the picture out for that screen, which is
     * where the remapping actually happens. */
    DoMethod(dt, DTM_PROCLAYOUT, (IPTR)NULL, 1);

    GetDTAttrs(dt,
               PDTA_DestBitMap,    (IPTR)&bm,
               PDTA_BitMapHeader,  (IPTR)&bmhd,
               TAG_DONE);

    /* Older datatypes fill only the source bitmap. */
    if (!bm)
        GetDTAttrs(dt, PDTA_BitMap, (IPTR)&bm, TAG_DONE);

    if (!bm || !bmhd || bmhd->bmh_Width <= 0 || bmhd->bmh_Height <= 0) {
        DisposeDTObject(dt);
        if (err) snprintf(err, errsz, "the picture decoded to nothing");
        return 0;
    }

    fresh.dt     = (APTR)dt;
    fresh.bm     = bm;
    fresh.width  = bmhd->bmh_Width;
    fresh.height = bmhd->bmh_Height;

    /* Only now let go of the old one: on failure above the tile keeps
     * showing the last good frame rather than going blank. */
    campic_free(p);
    *p = fresh;
    return 1;
}
