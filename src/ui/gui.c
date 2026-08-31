/*
 * ami2ha -- MUI dashboard
 *
 * The window is built once from the configuration; after that the event
 * loop only ever updates gadget contents, never rebuilds layout.
 *
 * The loop is the interesting part. MUI hands us a signal mask to sleep on,
 * and bsdsocket's WaitSelect() accepts an Exec signal mask alongside its
 * descriptor set. Passing MUI's mask straight into WaitSelect means one
 * Wait() covers the GUI, the socket and Ctrl-C together: the application
 * uses no CPU at all while idle, which matters rather more on a 68030 than
 * it would elsewhere.
 */
#include "ami2ha/compat.h"

#include <libraries/mui.h>
#include <proto/muimaster.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <dos/datetime.h>
#include <proto/utility.h>
#include <utility/date.h>

#include "ami2ha/json.h"
#include "ami2ha/ui.h"
#include "ami2ha/camfetch.h"
#include "campic.h"
#include "ami2ha/version.h"
#include "editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Library bases. MUI's set/get macros expand to BOOPSI calls in
 * intuition.library, so both must be open before any object exists.
 */
struct Library      *MUIMasterBase = NULL;
struct IntuitionBase *IntuitionBase = NULL;
/* Amiga2Date lives here, and it is the only locale-proof way to turn the
 * clock into numbers for a file name. */
struct Library      *UtilityBase   = NULL;

/* Neither MAKE_ID nor xget() is supplied by this SDK. */
#define A2H_MAKE_ID(a,b,c,d) \
    (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (ULONG)(d))

static IPTR xget(Object *obj, ULONG attr)
{
    IPTR v = 0;
    GetAttr(attr, obj, (ULONG *)&v);
    return v;
}

static int ui_libs_open(void)
{
    if (!IntuitionBase)
        IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!MUIMasterBase)
        MUIMasterBase = OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN);
    if (!UtilityBase)
        UtilityBase = OpenLibrary("utility.library", 37);
    return IntuitionBase && MUIMasterBase && UtilityBase;
}

/*
 * Say something on screen when there is no console to say it on. A GUI start
 * that cannot reach Home Assistant used to end with a printf and no window,
 * which from a Workbench double-click is indistinguishable from the program
 * not having run at all.
 *
 * EasyRequest rather than MUI_Request: this has to work before any MUI
 * application object exists, which is exactly when the first connection is
 * made.
 */
int ui_ask(const char *title, const char *text, const char *gadgets)
{
    struct EasyStruct es;

    if (!IntuitionBase)
        IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase)
        return 0;

    es.es_StructSize   = sizeof es;
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)title;
    es.es_TextFormat   = (UBYTE *)"%s";
    es.es_GadgetFormat = (UBYTE *)gadgets;

    /*
     * EasyRequest numbers the buttons left to right from 1 and gives the
     * rightmost one 0, so "Retry|Cancel" answers 1 for retry and 0 for
     * cancel -- which is also what a closed window means.
     */
    return (int)EasyRequestArgs(NULL, &es, NULL, (APTR)&text);
}

void ui_alert(const char *title, const char *text)
{
    ui_ask(title, text, "Ok");
}

static void ui_libs_close(void)
{
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
    if (MUIMasterBase) {
        CloseLibrary(MUIMasterBase);
        MUIMasterBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

/* Return IDs. Widget n reports ID_WIDGET_BASE + n. */
#define ID_QUIT        1
#define ID_RECONNECT   2
#define ID_ABOUT       3
#define ID_WIDGET_BASE 1000
/*
 * A media widget has a row of transport buttons rather than one control, so
 * it needs several return IDs per widget: MEDIA_BASE + widget * MEDIA_STRIDE
 * + action. Above the widget range, and tested before it.
 */
/* One per camera widget: the Save item on its context menu. */
#define ID_CAMSAVE_BASE 50000

#define ID_MEDIA_BASE  100000
#define ID_MEDIA_STRIDE 8

/*
 * A level that has stopped moving for this long is what the user meant.
 * Ticks are fiftieths, so this is a third of a second -- long enough to
 * swallow a drag, short enough that letting go feels immediate.
 */
#define SETTLE_TICKS   15

/* Open, Stop, Close. In that order on screen and in the array. */
#define COVER_ACTIONS  3
#define ID_LEVEL_BASE  200000
#define ID_COVER_BASE  300000
#define ID_COVER_STRIDE 4

/* The transport, in the order the buttons appear. */
enum {
    MEDIA_PREV = 0,
    MEDIA_PLAY,
    MEDIA_NEXT,
    MEDIA_VOL_DOWN,
    MEDIA_VOL_UP,
    MEDIA_ACTIONS
};

/*
 * Reconnection. The initial connection is made before the window opens,
 * so the loop starts LINK_UP and only ever moves away from it when
 * something goes wrong.
 */
typedef enum {
    LINK_UP = 0,     /* connected, or at least trying nothing else */
    LINK_RETRYING,   /* down, with an attempt scheduled or running */
    LINK_GAVE_UP     /* down for a reason retrying cannot fix      */
} link_state;

#define RETRY_FIRST_SECS 2
#define RETRY_MAX_SECS   60
/* How long one attempt gets before it is written off, in ticks. Covers a
 * server that accepts the connection and then never speaks. */
#define ATTEMPT_TICKS    (30 * 50)

/*
 * Keepalive, in ticks. A TCP connection can die without either end being
 * told -- a WiFi re-association is enough -- and the socket then stays open
 * and silent forever. Reading it never reports closed or failed, so nothing
 * else in here notices: the dashboard goes on showing hours-old readings
 * under a status line claiming to be connected.
 *
 * So ask. After IDLE_PING_TICKS of silence send a keepalive, and if the
 * answer has not arrived by IDLE_DEAD_TICKS treat the link as gone. Home
 * Assistant sends its own pings well inside the dead window, so on a healthy
 * but quiet connection neither timer is ever reached.
 */
#define IDLE_PING_TICKS  (30 * 50)
#define IDLE_DEAD_TICKS  (90 * 50)

typedef struct {
    Object *value;   /* Text or Gauge showing the state; NULL for buttons */
    Object *control; /* Checkmark or Button; NULL for read-only widgets   */
    char    text[64];/* backing store for MUIA_Text_Contents             */

    /* Camera only. The decoded picture has to outlive the call that made
     * it, because MUI goes on drawing from that bitmap. */
    campic  pic;
    Object *stamp;      /* caption under a camera tile, or NULL */
    char    stamp_text[24];

    /* Media only. The transport buttons, and room for a line that has to
     * hold an artist, a title and a station name at once. */
    Object *media_btn[MEDIA_ACTIONS];
    char    media_text[96];
    /*
     * Station and volume go on their own smaller line. On one line with the
     * title they overflow the cell and MUI clips it mid-word -- "Shaboozey -
     * A Bar Song (Tipsy" is what that looks like, and it reads as a bug.
     */
    char    media_extra[64];
    /* The volume reads out beside the buttons that change it, not on the
     * line above: that line is already full of station names, and this is
     * the number someone is looking for while pressing Vol +. */
    Object *media_vol;
    char    media_vol_text[8];
    long    cam_next;   /* ui_now() tick of the next automatic refresh */
    /*
     * Camera context menu. MUI does not own a menustrip handed to it through
     * MUIA_ContextMenu the way it owns the object tree, so both are kept and
     * disposed here.
     */
    Object *cam_menu;
    Object *cam_save;
    int     cam_slot;   /* which of the two files the picture on screen holds */
    int     cam_want;   /* the file the fetch in flight is writing to        */

    /*
     * Dimmer, colour and cover. A slider notifies on every pixel of a drag,
     * so what the user asked for is parked here and sent once it stops
     * moving: one sweep across a slider is otherwise fifty service calls,
     * which a 68k can generate far faster than Home Assistant enjoys
     * receiving them.
     */
    Object *cover_btn[COVER_ACTIONS];
    /*
     * Colour. Three sliders of our own rather than a Coloradjust: that
     * class draws a colour wheel with a gradient slider beside it whose
     * size is entirely its own business, and at dashboard scale that
     * slider is too small to hit. These are the same sliders a dimmer
     * uses, so a colour light reads like the rest of the window.
     */
    Object *rgb[3];
    Object *swatch;
    long    send_at;    /* ui_now() tick to send at; 0 when nothing is due */
    int     pending;    /* percent for a dimmer, packed 0xRRGGBB for colour */
    int     shown;      /* what the gadget last read as, to spot a change  */
    /*
     * Not armed until the server has told us what this entity currently
     * is. MUI fires a notification while a Coloradjust sets itself up, and
     * without this that counted as the user choosing a colour: opening the
     * window pushed white at full brightness to the lamp before anyone had
     * touched anything. A control must not send a value it invented.
     */
    int     armed;
} ui_widget;

struct a2h_ui {
    Object     *app;
    Object     *win;
    Object     *status;
    char        status_text[96];  /* what the line currently shows       */
    char        status_idle[96];  /* what it returns to                  */
    long        status_until;     /* deadline in ticks, if status_timed  */
    int         status_timed;

    link_state  link;
    long        retry_at;        /* when to make the next attempt      */
    long        attempt_at;      /* when the running attempt started   */
    int         retry_secs;      /* current backoff                    */
    long        last_rx;         /* when the server was last heard     */
    long        last_ping;       /* when a keepalive was last sent     */

    a2h_config *cfg;
    ha_client  *ha;
    a2h_socket *sock;

    ui_widget  *widgets;
    int         nwidgets;

    /*
     * One snapshot fetch at a time, whichever tile asked. Several at once
     * would mean several sockets and several JPEG decodes competing on a
     * machine that has trouble with one.
     */
    camfetch    cam;

    a2h_rexx   *rexx;   /* borrowed; may be NULL */
    Object     *root;   /* holds the group boxes; rebuilt by the editor */
    Object     *menu;
    a2h_editor *editor;
    char        cfgpath[CFG_PATH_MAX];

    int         quit;
};

/* ------------------------------------------------------------------ *
 * Value formatting
 * ------------------------------------------------------------------ */

/* Append what fits and stop, so a long station name cannot run off the end. */
static void append_bounded(char *dst, size_t dstsz, const char *src)
{
    size_t have = strlen(dst);
    size_t room = (have + 1 < dstsz) ? dstsz - have - 1 : 0;

    strncat(dst, src, room);
}

/* "21.4" + "°C" -> "21.4 °C". Home Assistant's own unknown states are
 * shown as a dash, which reads better than the literal "unavailable". */
static void format_value(char *dst, size_t dstsz, const ha_entity *e)
{
    if (!e || e->state[0] == '\0' ||
        strcmp(e->state, "unavailable") == 0 ||
        strcmp(e->state, "unknown") == 0) {
        strncpy(dst, "-", dstsz - 1);
        dst[dstsz - 1] = '\0';
        return;
    }

    if (e->unit[0]) {
        size_t n = (size_t)snprintf(dst, dstsz, "%s %s", e->state, e->unit);
        if (n >= dstsz)
            dst[dstsz - 1] = '\0';
    } else {
        strncpy(dst, e->state, dstsz - 1);
        dst[dstsz - 1] = '\0';
    }
}

static int state_is_on(const ha_entity *e)
{
    if (!e)
        return 0;
    return strcmp(e->state, "on") == 0 ||
           strcmp(e->state, "open") == 0 ||
           strcmp(e->state, "home") == 0 ||
           strcmp(e->state, "playing") == 0;
}

/* Map an entity's numeric state onto a gauge's 0..1000 range. */
static long gauge_position(const a2h_widget *w, const ha_entity *e)
{
    json_token tok;
    long       v = 0;

    if (!e || !e->state[0])
        return 0;

    /* The state arrives as text; reuse the JSON number reader rather than
     * a second decimal parser. */
    tok.type  = JSON_NUMBER;
    tok.start = e->state;
    tok.len   = strlen(e->state);
    if (!json_fixed(&tok, &v, 0))
        return 0;

    if (v <= w->min)
        return 0;
    if (v >= w->max)
        return 1000;
    /* Scale before dividing, and in long arithmetic, so the ratio keeps its
     * precision without any floating point. */
    return ((v - w->min) * 1000L) / (w->max - w->min);
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */

static Object *make_label(const char *text)
{
    return MUI_NewObject(MUIC_Text,
        MUIA_Text_Contents,  (IPTR)text,
        MUIA_Text_PreParse,  (IPTR)"\33l",
        MUIA_Weight,         (IPTR)100,
        TAG_DONE);
}

static int build_widget(a2h_ui *ui, int index, Object *parent)
{
    const a2h_widget *w  = &ui->cfg->widgets[index];
    ui_widget        *uw = &ui->widgets[index];

    uw->value   = NULL;
    uw->control = NULL;
    strcpy(uw->text, "-");

    switch (w->kind) {
    case W_TEXT:
        /* A caption spans the row: the second cell is an empty spacer. */
        DoMethod(parent, OM_ADDMEMBER, (IPTR)MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)w->label,
            MUIA_Text_PreParse, (IPTR)"\33l\33b",
            TAG_DONE));
        DoMethod(parent, OM_ADDMEMBER,
                 (IPTR)MUI_NewObject(MUIC_Rectangle, TAG_DONE));
        return 1;

    case W_SENSOR:
        /*
         * FixWidthTxt reserves room for a representative reading. Without
         * it the object is only as wide as whatever it happens to show at
         * layout time, so the window collapses and later, longer values are
         * clipped -- "21.4 °C" becoming "21." as soon as the reading grows.
         */
        uw->value = MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents,  (IPTR)uw->text,
            MUIA_Text_PreParse,  (IPTR)"\33r",
            MUIA_Frame,          MUIV_Frame_Text,
            MUIA_FixWidthTxt,    (IPTR)"-8888.8 XXXX",
            TAG_DONE);
        if (!uw->value)
            return 0;
        DoMethod(parent, OM_ADDMEMBER, (IPTR)make_label(w->label));
        DoMethod(parent, OM_ADDMEMBER, (IPTR)uw->value);
        return 1;

    case W_GAUGE:
        uw->value = MUI_NewObject(MUIC_Gauge,
            MUIA_Gauge_Horiz,   TRUE,
            MUIA_Gauge_Max,     (IPTR)1000,
            MUIA_Gauge_Current, (IPTR)0,
            MUIA_Gauge_InfoText,(IPTR)uw->text,
            /* Frame_Text rather than Frame_Gauge: the two reserve different
             * amounts of horizontal space, and since FixWidthTxt fixes the
             * inner width, that difference would leave the gauge a few
             * pixels narrower than the reading fields beside it. */
            MUIA_Frame,         MUIV_Frame_Text,
            MUIA_FixWidthTxt,   (IPTR)"-8888.8 XXXX",
            TAG_DONE);
        if (!uw->value)
            return 0;
        DoMethod(parent, OM_ADDMEMBER, (IPTR)make_label(w->label));
        DoMethod(parent, OM_ADDMEMBER, (IPTR)uw->value);
        return 1;

    case W_TOGGLE:
        uw->control = MUI_NewObject(MUIC_Image,
            MUIA_Frame,            MUIV_Frame_ImageButton,
            MUIA_InputMode,        MUIV_InputMode_Toggle,
            MUIA_Image_Spec,       (IPTR)MUII_CheckMark,
            MUIA_ShowSelState,     FALSE,
            MUIA_Background,       MUII_ButtonBack,
            TAG_DONE);
        if (!uw->control)
            return 0;
        DoMethod(parent, OM_ADDMEMBER, (IPTR)make_label(w->label));
        /* Keep the checkmark its natural size rather than stretched. */
        DoMethod(parent, OM_ADDMEMBER, (IPTR)MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz, TRUE,
            MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Rectangle, TAG_DONE),
            MUIA_Group_Child, (IPTR)uw->control,
            TAG_DONE));
        return 1;

    case W_DIMMER:
        /*
         * A slider rather than a checkbox, because a dimmable lamp has a
         * hundred useful states and two of them are on and off. The
         * reading beside it is the number people actually want: MUI can
         * draw the level on the knob, but not while the slider is also
         * being told what the server says.
         */
        uw->control = MUI_NewObject(MUIC_Slider,
            MUIA_Slider_Min,   (IPTR)0,
            MUIA_Slider_Max,   (IPTR)100,
            MUIA_Slider_Level, (IPTR)0,
            TAG_DONE);
        uw->value = MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)"-",
            MUIA_Text_PreParse, (IPTR)"\33r",
            MUIA_Frame,         MUIV_Frame_Text,
            MUIA_FixWidthTxt,   (IPTR)"8888%",
            TAG_DONE);
        if (!uw->control || !uw->value)
            return 0;
        DoMethod(parent, OM_ADDMEMBER, (IPTR)make_label(w->label));
        DoMethod(parent, OM_ADDMEMBER, (IPTR)MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz,  TRUE,
            MUIA_Group_Child,  (IPTR)uw->control,
            MUIA_Group_Child,  (IPTR)uw->value,
            TAG_DONE));
        return 1;

    case W_COLOR: {
        /*
         * Built from three sliders and a swatch rather than using
         * Coloradjust. That class puts a colour wheel and a gradient
         * slider in the row, and the gradient slider is a few pixels wide
         * -- fiddly to hit with a mouse and impossible to make bigger,
         * since its size is internal to the class. These are the same
         * sliders a dimmer uses.
         *
         * Disabled lives on the enclosing group, which MUI passes down to
         * the children, so the whole control greys out together until the
         * light reports a colour.
         */
        static const char *const gun[3] = { "R", "G", "B" };
        Object *rows;
        int     c;

        rows = MUI_NewObject(MUIC_Group, TAG_DONE);
        if (!rows)
            return 0;
        for (c = 0; c < 3; c++) {
            Object *row;

            uw->rgb[c] = MUI_NewObject(MUIC_Slider,
                MUIA_Slider_Min,   (IPTR)0,
                MUIA_Slider_Max,   (IPTR)255,
                MUIA_Slider_Level, (IPTR)0,
                TAG_DONE);
            row = MUI_NewObject(MUIC_Group,
                MUIA_Group_Horiz, TRUE,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)gun[c],
                    MUIA_Weight,        (IPTR)0,
                    TAG_DONE),
                MUIA_Group_Child, (IPTR)uw->rgb[c],
                TAG_DONE);
            if (!uw->rgb[c] || !row)
                return 0;
            DoMethod(rows, OM_ADDMEMBER, (IPTR)row);
        }

        /* The preview. This is the one pen the control asks the screen
         * for, which is worth knowing on a 256-colour Workbench. */
        uw->swatch = MUI_NewObject(MUIC_Colorfield,
            MUIA_Frame,       MUIV_Frame_ImageButton,
            MUIA_FixWidth,    (IPTR)28,
            TAG_DONE);
        if (!uw->swatch)
            return 0;

        uw->control = MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz, TRUE,
            MUIA_Disabled,    TRUE,
            MUIA_Group_Child, (IPTR)uw->swatch,
            MUIA_Group_Child, (IPTR)rows,
            TAG_DONE);
        if (!uw->control)
            return 0;

        DoMethod(parent, OM_ADDMEMBER, (IPTR)make_label(w->label));
        DoMethod(parent, OM_ADDMEMBER, (IPTR)uw->control);
        return 1;
    }

    case W_COVER: {
        /*
         * What a blind's own remote has, in the order it has them. Stop is
         * the one that matters on a slat blind and the one a checkbox can
         * never offer, which is why a cover is not a toggle.
         */
        static const char *const cover_labels[COVER_ACTIONS] = {
            "Open", "Stop", "Close"
        };
        Object *row;
        int     b;

        row = MUI_NewObject(MUIC_Group, MUIA_Group_Horiz, TRUE, TAG_DONE);
        if (!row)
            return 0;
        for (b = 0; b < COVER_ACTIONS; b++) {
            uw->cover_btn[b] = MUI_NewObject(MUIC_Text,
                MUIA_Text_Contents, (IPTR)cover_labels[b],
                MUIA_Text_PreParse, (IPTR)"\33c",
                MUIA_Frame,         MUIV_Frame_Button,
                MUIA_Background,    MUII_ButtonBack,
                MUIA_InputMode,     MUIV_InputMode_RelVerify,
                TAG_DONE);
            if (!uw->cover_btn[b])
                return 0;
            DoMethod(row, OM_ADDMEMBER, (IPTR)uw->cover_btn[b]);
        }

        /* Where it actually is. A blind reports 0..100, and "open" alone
         * does not tell you whether that means fully or a handspan. */
        uw->value = MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)"-",
            MUIA_Text_PreParse, (IPTR)"\33r",
            MUIA_Frame,         MUIV_Frame_Text,
            MUIA_FixWidthTxt,   (IPTR)"8888%",
            TAG_DONE);
        if (!uw->value)
            return 0;
        DoMethod(row, OM_ADDMEMBER, (IPTR)uw->value);

        DoMethod(parent, OM_ADDMEMBER, (IPTR)make_label(w->label));
        DoMethod(parent, OM_ADDMEMBER, (IPTR)row);
        return 1;
    }

    case W_CAMERA:
        /*
         * Created empty: the picture cannot be decoded until there is a
         * screen to remap it onto, and at build time the window has not
         * been opened yet. The first frame arrives from a fetch.
         *
         * InputMode makes the picture itself the refresh control -- there
         * is no room on a dashboard for a button beside every camera, and
         * clicking the image is what anyone would try first.
         */
        uw->control = MUI_NewObject(MUIC_Bitmap,
            MUIA_Frame,          MUIV_Frame_ImageButton,
            MUIA_InputMode,      MUIV_InputMode_RelVerify,
            MUIA_Background,     MUII_BACKGROUND,
            MUIA_Bitmap_Width,   (IPTR)w->cam_w,
            MUIA_Bitmap_Height,  (IPTR)w->cam_h,
            MUIA_FixWidth,       (IPTR)w->cam_w,
            MUIA_FixHeight,      (IPTR)w->cam_h,
            TAG_DONE);
        if (!uw->control)
            return 0;
        DoMethod(parent, OM_ADDMEMBER, (IPTR)make_label(w->label));

        /*
         * Right-click is the menu button on this machine, and MUI turns it
         * into a context menu for whichever object is under the pointer --
         * so the obvious gesture on a picture is available without spending
         * dashboard space on a button. Left-click stays the refresh.
         */
        uw->cam_save = MUI_NewObject(MUIC_Menuitem,
            MUIA_Menuitem_Title, (IPTR)"Save snapshot",
            MUIA_UserData,       (IPTR)(ID_CAMSAVE_BASE + index),
            TAG_DONE);
        uw->cam_menu = MUI_NewObject(MUIC_Menustrip,
            MUIA_Family_Child, (IPTR)MUI_NewObject(MUIC_Menu,
                MUIA_Menu_Title, (IPTR)w->label,
                MUIA_Family_Child, (IPTR)uw->cam_save,
                TAG_DONE),
            TAG_DONE);
        if (uw->cam_menu && uw->cam_save)
            set(uw->control, MUIA_ContextMenu, (IPTR)uw->cam_menu);
        else
            uw->cam_menu = uw->cam_save = NULL;

        if (w->cam_stamp) {
            /* The picture and its caption share the cell, so the grid is
             * still two columns and the camera row still lines up with the
             * readings above it. */
            Object *cell;

            strcpy(uw->stamp_text, "--");
            uw->stamp = MUI_NewObject(MUIC_Text,
                MUIA_Text_Contents, (IPTR)uw->stamp_text,
                MUIA_Text_PreParse, (IPTR)"\33c",
                MUIA_Font,          (IPTR)MUIV_Font_Tiny,
                TAG_DONE);
            cell = MUI_NewObject(MUIC_Group,
                MUIA_Group_Child, (IPTR)uw->control,
                MUIA_Group_Child, (IPTR)uw->stamp,
                TAG_DONE);
            if (!cell || !uw->stamp) {
                uw->stamp = NULL;
                DoMethod(parent, OM_ADDMEMBER, (IPTR)uw->control);
                return 1;
            }
            DoMethod(parent, OM_ADDMEMBER, (IPTR)cell);
        } else {
            DoMethod(parent, OM_ADDMEMBER, (IPTR)uw->control);
        }
        return 1;

    case W_MEDIA: {
        /*
         * Two rows in one cell: what is playing, and the transport under it.
         * The buttons are Text objects with a button frame, exactly as
         * W_BUTTON does it -- MUIC_Button is not in MUI 3.8's public class
         * list, and the framed Text is what the rest of this file already
         * trusts.
         *
         * A Rectangle after the buttons soaks up the spare width, or MUI
         * stretches five buttons across the whole dashboard.
         */
        static const char *const face[MEDIA_ACTIONS] = {
            "<<", ">/||", ">>", "Vol -", "Vol +"
        };
        Object *row, *cell;
        int     b;

        strcpy(uw->media_text, "-");
        uw->value = MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)uw->media_text,
            MUIA_Text_PreParse, (IPTR)"\33l",
            MUIA_Frame,         MUIV_Frame_Text,
            TAG_DONE);

        row = MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz,   TRUE,
            MUIA_Group_Spacing, 2,
            TAG_DONE);
        if (!uw->value || !row)
            return 0;

        for (b = 0; b < MEDIA_ACTIONS; b++) {
            uw->media_btn[b] = MUI_NewObject(MUIC_Text,
                MUIA_Text_Contents, (IPTR)face[b],
                MUIA_Text_PreParse, (IPTR)"\33c",
                MUIA_Frame,         MUIV_Frame_Button,
                MUIA_Background,    MUII_ButtonBack,
                MUIA_InputMode,     MUIV_InputMode_RelVerify,
                MUIA_FixWidthTxt,   (IPTR)"Vol -",
                TAG_DONE);
            if (!uw->media_btn[b])
                return 0;
            DoMethod(row, OM_ADDMEMBER, (IPTR)uw->media_btn[b]);
        }
        strcpy(uw->media_vol_text, "-");
        uw->media_vol = MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)uw->media_vol_text,
            MUIA_Text_PreParse, (IPTR)"\33r",
            MUIA_FixWidthTxt,   (IPTR)"100%",
            MUIA_Frame,         MUIV_Frame_Text,
            TAG_DONE);
        if (!uw->media_vol)
            return 0;
        DoMethod(row, OM_ADDMEMBER, (IPTR)uw->media_vol);
        DoMethod(row, OM_ADDMEMBER, (IPTR)MUI_NewObject(MUIC_Rectangle, TAG_DONE));

        strcpy(uw->media_extra, "");
        uw->stamp = MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)uw->media_extra,
            MUIA_Text_PreParse, (IPTR)"\33l",
            MUIA_Font,          (IPTR)MUIV_Font_Tiny,
            MUIA_Frame,         MUIV_Frame_Text,
            TAG_DONE);

        cell = MUI_NewObject(MUIC_Group,
            MUIA_Group_Child, (IPTR)uw->value,
            MUIA_Group_Child, (IPTR)uw->stamp,
            MUIA_Group_Child, (IPTR)row,
            TAG_DONE);
        if (!cell || !uw->stamp)
            return 0;

        DoMethod(parent, OM_ADDMEMBER, (IPTR)make_label(w->label));
        DoMethod(parent, OM_ADDMEMBER, (IPTR)cell);
        return 1;
    }

    case W_BUTTON:
        uw->control = MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)w->label,
            MUIA_Text_PreParse, (IPTR)"\33c",
            MUIA_Frame,         MUIV_Frame_Button,
            MUIA_Background,    MUII_ButtonBack,
            MUIA_InputMode,     MUIV_InputMode_RelVerify,
            TAG_DONE);
        if (!uw->control)
            return 0;
        DoMethod(parent, OM_ADDMEMBER, (IPTR)uw->control);
        DoMethod(parent, OM_ADDMEMBER,
                 (IPTR)MUI_NewObject(MUIC_Rectangle, TAG_DONE));
        return 1;
    }

    return 0;
}

/*
 * Bind each control to the loop, reporting its own widget index. Called
 * again after a rebuild: the old controls are disposed along with their
 * notifications, so new ones have to be bound or nothing a toggle or a
 * button does ever reaches the event loop.
 */
static void bind_controls(a2h_ui *ui)
{
    int i;

    for (i = 0; i < ui->nwidgets; i++) {
        const a2h_widget *w = &ui->cfg->widgets[i];

        /*
         * A media widget has a row of buttons rather than one control, so it
         * is bound before the guard below -- which exists for the kinds that
         * have no control at all, and would otherwise skip this one too.
         */
        if (w->kind == W_MEDIA) {
            int b;
            for (b = 0; b < MEDIA_ACTIONS; b++)
                if (ui->widgets[i].media_btn[b])
                    DoMethod(ui->widgets[i].media_btn[b], MUIM_Notify,
                             MUIA_Pressed, FALSE, (IPTR)ui->app, 2,
                             MUIM_Application_ReturnID,
                             ID_MEDIA_BASE + i * ID_MEDIA_STRIDE + b);
            continue;
        }

        /* A cover is a row of buttons too, so it is bound above the
         * one-control guard for the same reason the media widget is. */
        if (w->kind == W_COVER) {
            int b;
            for (b = 0; b < COVER_ACTIONS; b++)
                if (ui->widgets[i].cover_btn[b])
                    DoMethod(ui->widgets[i].cover_btn[b], MUIM_Notify,
                             MUIA_Pressed, FALSE, (IPTR)ui->app, 2,
                             MUIM_Application_ReturnID,
                             ID_COVER_BASE + i * ID_COVER_STRIDE + b);
            continue;
        }

        if (!ui->widgets[i].control)
            continue;

        /*
         * Deliberately not notified. A slider reports every pixel of a
         * drag, and each report came back as an application return ID --
         * but this loop only services the sockets on the pass where MUI
         * has nothing pending, so a chatty gadget starved the network
         * outright: with one dimmer on the dashboard no camera ever
         * fetched a picture at all. The value controls are polled in
         * pending_tick instead, which already runs on every pass.
         */
        if (w->kind == W_DIMMER || w->kind == W_COLOR)
            continue;

        if (w->kind == W_TOGGLE)
            DoMethod(ui->widgets[i].control, MUIM_Notify, MUIA_Selected,
                     MUIV_EveryTime, (IPTR)ui->app, 2,
                     MUIM_Application_ReturnID, ID_WIDGET_BASE + i);
        else if (w->kind == W_BUTTON || w->kind == W_CAMERA) {
            DoMethod(ui->widgets[i].control, MUIM_Notify, MUIA_Pressed, FALSE,
                     (IPTR)ui->app, 2,
                     MUIM_Application_ReturnID, ID_WIDGET_BASE + i);
            /*
             * A context menu choice arrives as MUIA_ContextMenuTrigger on the
             * object the menu belongs to, holding the item that was picked --
             * which is why that attribute exists, and it means no subclass is
             * needed. Watching for the exact item keeps this working when
             * there is more than one.
             */
            if (ui->widgets[i].cam_save)
                DoMethod(ui->widgets[i].control, MUIM_Notify,
                         MUIA_ContextMenuTrigger,
                         (IPTR)ui->widgets[i].cam_save,
                         (IPTR)ui->app, 2,
                         MUIM_Application_ReturnID, ID_CAMSAVE_BASE + i);
        }

    }
}

/* Create one framed box per configured group and fill it with widgets. */
static void build_groups(a2h_ui *ui)
{
    int g, i;

    for (g = 0; g < ui->cfg->ngroups; g++) {
        const a2h_group *grp = &ui->cfg->groups[g];
        Object          *box;

        /* A group with nothing in it would draw as a small empty framed
         * box. The settings window can leave those behind, so skip them. */
        if (grp->nwidgets <= 0)
            continue;

        /*
         * Two columns, with the label and the value added as separate
         * children. Wrapping each row in its own horizontal group instead
         * makes MUI lay every row out independently, so the value fields
         * start wherever that row's label happens to end and the column
         * comes out ragged.
         */
        box = MUI_NewObject(MUIC_Group,
            MUIA_Group_Columns, (IPTR)2,
            MUIA_Frame,         MUIV_Frame_Group,
            MUIA_FrameTitle,    (IPTR)grp->title,
            MUIA_Background,    MUII_GroupBack,
            TAG_DONE);
        if (!box)
            continue;

        for (i = grp->first_widget;
             i < grp->first_widget + grp->nwidgets && i < ui->nwidgets; i++)
            build_widget(ui, i, box);

        DoMethod(ui->root, OM_ADDMEMBER, (IPTR)box);
    }
}

/*
 * Build the dashboard's contents from the configuration: the widget
 * table, the group boxes and the notifications that make the controls
 * report back.
 *
 * The first build and every rebuild both go through here. They used to be
 * written out separately, which is how they came apart: a rebuild disposes
 * every gadget, a disposed gadget takes its notifications with it, and the
 * rebuild did not put them back, so after any visit to the settings window
 * the toggles quietly stopped doing anything. Anything that has to be true
 * of the dashboard belongs in this one function.
 */
/*
 * Let go of every decoded picture. MUI must already have been disposed or
 * pointed elsewhere: the bitmaps belong to the datatype objects freed here,
 * and MUI would otherwise go on drawing from memory that has been returned.
 */
static void free_pictures(a2h_ui *ui)
{
    int i;

    for (i = 0; i < ui->nwidgets; i++)
        campic_free(&ui->widgets[i].pic);
}

/*
 * A menustrip handed over through MUIA_ContextMenu is not part of the
 * application's object tree, so nothing else ever frees it -- not
 * MUI_DisposeObject(app), and not the group rebuild that throws away the
 * tile it was attached to.
 */
static void free_context_menus(a2h_ui *ui)
{
    int i;

    for (i = 0; i < ui->nwidgets; i++) {
        if (ui->widgets[i].cam_menu)
            MUI_DisposeObject(ui->widgets[i].cam_menu);
        ui->widgets[i].cam_menu = NULL;
        ui->widgets[i].cam_save = NULL;
    }
}

static int build_dashboard(a2h_ui *ui)
{
    /* The widget table is indexed by configuration order, so it has to
     * match the configuration's size. */
    int        want  = ui->cfg->nwidgets > 0 ? ui->cfg->nwidgets : 1;
    ui_widget *fresh = (ui_widget *)calloc((size_t)want, sizeof *fresh);

    if (!fresh)
        return 0;

    /* The old objects are gone by now, so nothing is drawing from these. */
    free_pictures(ui);
    free_context_menus(ui);
    free(ui->widgets);
    ui->widgets  = fresh;
    ui->nwidgets = ui->cfg->nwidgets;

    build_groups(ui);
    bind_controls(ui);
    return 1;
}

/*
 * Replace the window's contents after the settings window changed them.
 * MUI allows a group's children to be swapped while the window is open,
 * provided the change is bracketed by InitChange/ExitChange.
 */
void ui_rebuild(a2h_ui *ui)
{
    struct MinList *children;
    Object         *child;
    APTR            state;

    if (!ui || !ui->root)
        return;

    /*
     * The column count can have changed in the settings window. It is one
     * of the few group attributes MUI lets you set after creation
     * (MUIA_Group_Horiz is not), so the dashboard can re-flow without
     * rebuilding the window around it.
     */
    set(ui->root, MUIA_Group_Columns,
        (IPTR)(ui->cfg->columns > 0 ? ui->cfg->columns : 1));

    DoMethod(ui->root, MUIM_Group_InitChange);

    children = NULL;
    get(ui->root, MUIA_Group_ChildList, &children);
    if (children) {
        /* Collect first: removing while iterating the live list is unsafe. */
        Object *doomed[CFG_MAX_GROUPS + 4];
        int     n = 0;

        state = children->mlh_Head;
        while ((child = NextObject(&state)) != NULL &&
               n < (int)A2H_ARRAY_LEN(doomed))
            doomed[n++] = child;

        while (n-- > 0) {
            DoMethod(ui->root, OM_REMMEMBER, (IPTR)doomed[n]);
            MUI_DisposeObject(doomed[n]);
        }
    }

    build_dashboard(ui);

    DoMethod(ui->root, MUIM_Group_ExitChange);

    ui_refresh_all(ui);
}

a2h_ui *ui_create(a2h_config *cfg, ha_client *ha, a2h_socket *sock,
                  const char *cfgpath, char *err, size_t errsz)
{
    a2h_ui *ui;
    Object *root;

    if (!ui_libs_open()) {
        strncpy(err, "cannot open muimaster.library (is MUI installed?)",
                errsz - 1);
        err[errsz - 1] = '\0';
        ui_libs_close();
        return NULL;
    }

    ui = (a2h_ui *)calloc(1, sizeof *ui);
    if (!ui) {
        strncpy(err, "out of memory", errsz - 1);
        ui_libs_close();
        return NULL;
    }
    camfetch_init(&ui->cam);
    ui->cfg  = cfg;
    ui->ha   = ha;
    ui->sock = sock;
    if (cfgpath)
        strncpy(ui->cfgpath, cfgpath, sizeof ui->cfgpath - 1);

    strcpy(ui->status_text, "Connecting...");
    strcpy(ui->status_idle, ui->status_text);
    ui->link       = LINK_UP;   /* main() connected before we got here */
    ui->retry_secs = RETRY_FIRST_SECS;

    ui->status = MUI_NewObject(MUIC_Text,
        MUIA_Text_Contents, (IPTR)ui->status_text,
        MUIA_Text_PreParse, (IPTR)"\33l",
        MUIA_Frame,         MUIV_Frame_Text,
        MUIA_Background,    MUII_TextBack,
        TAG_DONE);

    /* An empty group to hang the configured groups off; children are added
     * below rather than in the varargs list, since the count is dynamic. */
    root = MUI_NewObject(MUIC_Group,
        MUIA_Group_Columns, (IPTR)cfg->columns,
        TAG_DONE);
    ui->root = root;

    if (!root || !ui->status) {
        strncpy(err, "could not create MUI objects", errsz - 1);
        free(ui);
        ui_libs_close();
        return NULL;
    }

    ui->win = MUI_NewObject(MUIC_Window,
        MUIA_Window_Title,     (IPTR)A2H_TITLE,
        /* What the Workbench title bar shows while this window is at the
         * front. Set on both windows, or it reverts to Workbench's own
         * text whenever the settings window is the active one. */
        MUIA_Window_ScreenTitle,(IPTR)A2H_TITLE,
        MUIA_Window_ID,        (IPTR)A2H_MAKE_ID('A','2','H','A'),
        MUIA_Window_RootObject,(IPTR)MUI_NewObject(MUIC_Group,
            MUIA_Group_Child, (IPTR)root,
            MUIA_Group_Child, (IPTR)ui->status,
            TAG_DONE),
        TAG_DONE);

    {
        Object *menu = MUI_NewObject(MUIC_Menustrip,
            MUIA_Family_Child, (IPTR)MUI_NewObject(MUIC_Menu,
                MUIA_Menu_Title, (IPTR)"Project",
                MUIA_Family_Child, (IPTR)MUI_NewObject(MUIC_Menuitem,
                    MUIA_Menuitem_Title, (IPTR)"Settings...",
                    MUIA_Menuitem_Shortcut, (IPTR)"S",
                    MUIA_UserData, (IPTR)ID_ED_OPEN,
                    TAG_DONE),
                MUIA_Family_Child, (IPTR)MUI_NewObject(MUIC_Menuitem,
                    MUIA_Menuitem_Title, (IPTR)"Reconnect",
                    MUIA_Menuitem_Shortcut, (IPTR)"R",
                    MUIA_UserData, (IPTR)ID_RECONNECT,
                    TAG_DONE),
                MUIA_Family_Child, (IPTR)MUI_NewObject(MUIC_Menuitem,
                    MUIA_Menuitem_Title, (IPTR)"About...",
                    MUIA_Menuitem_Shortcut, (IPTR)"A",
                    MUIA_UserData, (IPTR)ID_ABOUT,
                    TAG_DONE),
                MUIA_Family_Child, (IPTR)MUI_NewObject(MUIC_Menuitem,
                    MUIA_Menuitem_Title, (IPTR)-1,   /* separator bar */
                    TAG_DONE),
                MUIA_Family_Child, (IPTR)MUI_NewObject(MUIC_Menuitem,
                    MUIA_Menuitem_Title, (IPTR)"Quit",
                    MUIA_Menuitem_Shortcut, (IPTR)"Q",
                    MUIA_UserData, (IPTR)ID_QUIT,
                    TAG_DONE),
                TAG_DONE),
            TAG_DONE);
        ui->menu = menu;
    }

    ui->app = MUI_NewObject(MUIC_Application,
        MUIA_Application_Title,      (IPTR)"ami2ha",
        MUIA_Application_Version,    (IPTR)A2H_VERSTAG,
        MUIA_Application_Copyright,  (IPTR)"MIT licensed",
        MUIA_Application_Author,     (IPTR)"ami2ha contributors",
        MUIA_Application_Description,(IPTR)"Home Assistant client",
        MUIA_Application_Base,       (IPTR)"AMI2HA",
        MUIA_Application_Menustrip,  (IPTR)ui->menu,
        MUIA_Application_Window,     (IPTR)ui->win,
        TAG_DONE);

    if (!ui->app || !ui->win) {
        strncpy(err, "could not create the window (is MUI installed?)", errsz - 1);
        if (ui->app)
            MUI_DisposeObject(ui->app);
        free(ui);
        ui_libs_close();
        return NULL;
    }

    /* Closing the window quits. */
    DoMethod(ui->win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (IPTR)ui->app, 2, MUIM_Application_ReturnID, ID_QUIT);

    /*
     * Fill the dashboard in now rather than before the window was made:
     * binding the controls needs the application object. The window is
     * not open yet, so the group can take children without the
     * InitChange/ExitChange bracket a rebuild needs.
     */
    if (!build_dashboard(ui)) {
        strncpy(err, "out of memory", errsz - 1);
        MUI_DisposeObject(ui->app);
        free(ui);
        ui_libs_close();
        return NULL;
    }

    ui->editor = editor_create(ui->app, cfg, ha, ui->cfgpath);

    set(ui->win, MUIA_Window_Open, TRUE);
    if (!xget(ui->win, MUIA_Window_Open)) {
        strncpy(err, "could not open the window", errsz - 1);
        MUI_DisposeObject(ui->app);
        free(ui->widgets);
        free(ui);
        ui_libs_close();
        return NULL;
    }

    err[0] = '\0';
    return ui;
}

void ui_set_rexx(a2h_ui *ui, a2h_rexx *rexx)
{
    if (ui)
        ui->rexx = rexx;
}

void ui_dispose(a2h_ui *ui)
{
    if (!ui)
        return;
    editor_dispose(ui->editor);
    ui->editor = NULL;
    camfetch_cancel(&ui->cam);
    if (ui->app) {
        set(ui->win, MUIA_Window_Open, FALSE);
        MUI_DisposeObject(ui->app);
    }
    free_context_menus(ui);

    /* After the objects, so nothing is drawing from these bitmaps. */
    free_pictures(ui);
    free(ui->widgets);
    free(ui);
    ui_libs_close();
}

/* ------------------------------------------------------------------ *
 * Updates
 * ------------------------------------------------------------------ */

/* Three seconds, in ticks of a fiftieth of a second. */
#define STATUS_FLASH_TICKS 150

/*
 * Ticks since midnight. Enough resolution for a status timeout and cheap
 * enough to read on every pass -- no timer.device unit to open and serve.
 * It wraps once a day, which status_tick() treats as an expiry.
 */
static long ui_now(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (long)ds.ds_Minute * 3000L + (long)ds.ds_Tick;
}

static void status_show(a2h_ui *ui, const char *text)
{
    strncpy(ui->status_text, text, sizeof ui->status_text - 1);
    ui->status_text[sizeof ui->status_text - 1] = '\0';
    set(ui->status, MUIA_Text_Contents, (IPTR)ui->status_text);
}

void ui_set_status(a2h_ui *ui, const char *text)
{
    if (!ui || !ui->status)
        return;
    strncpy(ui->status_idle, text, sizeof ui->status_idle - 1);
    ui->status_idle[sizeof ui->status_idle - 1] = '\0';
    ui->status_timed = 0;
    status_show(ui, ui->status_idle);
}

void ui_flash_status(a2h_ui *ui, const char *text)
{
    if (!ui || !ui->status)
        return;
    ui->status_until = ui_now() + STATUS_FLASH_TICKS;
    ui->status_timed = 1;
    status_show(ui, text);
}

/*
 * Put the resting text back once a flashed message has had its time.
 * Returns how long until that is due in milliseconds, so the event loop
 * can wake up for it, or -1 when nothing is pending.
 */
static long status_tick(a2h_ui *ui)
{
    long left;

    if (!ui->status_timed)
        return -1;

    left = ui->status_until - ui_now();
    if (left <= 0 || left > STATUS_FLASH_TICKS) {   /* elapsed, or midnight */
        ui->status_timed = 0;
        status_show(ui, ui->status_idle);
        return -1;
    }
    return left * 20;
}

/*
 * One line describing what a media player is doing. Home Assistant sends the
 * pieces separately and any of them can be missing -- a radio stream often
 * has a title and no artist, and a stopped player has neither -- so this
 * builds up whatever is there rather than assuming a shape.
 *
 * Volume rides along at the end because it is the one thing the buttons
 * change that has no other display: without it, pressing Vol + twice tells
 * you nothing.
 */
/*
 * Only a player with something loaded has a track to name. Home Assistant
 * does withdraw the title when a player is switched off, but relying on that
 * alone means trusting every integration to do it -- and a switched-off
 * player still advertising the last song is exactly the wrong answer.
 */
static int media_has_track(const ha_entity *e)
{
    return e && (strcmp(e->state, "playing")   == 0 ||
                 strcmp(e->state, "paused")    == 0 ||
                 strcmp(e->state, "buffering") == 0);
}

static void media_describe(char *dst, size_t dstsz, const ha_entity *e)
{
    const char *title = media_has_track(e) ? ha_entity_attr(e, "media_title")
                                           : NULL;
    const char *state = (e && e->state[0]) ? e->state : "-";

    dst[0] = '\0';

    /*
     * The title alone on the wide line. Artist and station go underneath,
     * because a cell beside a 320-pixel camera tile is about 29 characters
     * and "Coldplay - Hymn for the Weekend" is 31 -- putting them together
     * clipped ordinary radio tracks mid-word.
     */
    if (title && *title)
        append_bounded(dst, dstsz, title);
    else
        append_bounded(dst, dstsz, state);   /* nothing playing: that is the news */
}

/*
 * Artist, station and volume, in the small font under the title. Whichever
 * of them Home Assistant is sending -- a stream often has no artist, a file
 * no station -- separated only where there is something on both sides.
 */
static void media_describe_extra(char *dst, size_t dstsz, const ha_entity *e)
{
    const char *artist = media_has_track(e) ? ha_entity_attr(e, "media_artist")
                                            : NULL;
    const char *chan   = media_has_track(e) ? ha_entity_attr(e, "media_channel")
                                            : NULL;

    dst[0] = '\0';

    if (artist && *artist)
        append_bounded(dst, dstsz, artist);

    if (chan && *chan) {
        if (dst[0])
            append_bounded(dst, dstsz, " | ");
        append_bounded(dst, dstsz, chan);
    }
}

/*
 * "0.71" -> "71%", without floating point: shift the decimal point and round
 * on the digit after the ones we keep. vbcc's %f would drag in the whole
 * floating point library for this.
 */
static void media_volume(char *dst, size_t dstsz, const ha_entity *e)
{
    const char *vol = e ? ha_entity_attr(e, "volume_level") : NULL;
    const char *p;
    int         pct;

    strncpy(dst, "-", dstsz - 1);
    dst[dstsz - 1] = '\0';
    if (!vol || !*vol)
        return;

    p   = strchr(vol, '.');
    pct = (int)strtol(vol, NULL, 10) * 100;
    if (p) {
        int tenths = (p[1] >= '0' && p[1] <= '9') ? p[1] - '0' : 0;
        int hund   = (p[2] >= '0' && p[2] <= '9') ? p[2] - '0' : 0;
        pct += tenths * 10 + hund + (p[3] >= '5' && p[3] <= '9' ? 1 : 0);
    }
    if (pct >= 0 && pct <= 100 && dstsz > 5)
        sprintf(dst, "%d%%", pct);
}

static void update_widget(a2h_ui *ui, int i, const ha_entity *e)
{
    const a2h_widget *w  = &ui->cfg->widgets[i];
    ui_widget        *uw = &ui->widgets[i];

    switch (w->kind) {
    case W_SENSOR:
        format_value(uw->text, sizeof uw->text, e);
        if (uw->value)
            set(uw->value, MUIA_Text_Contents, (IPTR)uw->text);
        break;

    case W_GAUGE:
        format_value(uw->text, sizeof uw->text, e);
        if (uw->value) {
            SetAttrs(uw->value,
                MUIA_Gauge_InfoText, (IPTR)uw->text,
                MUIA_Gauge_Current,  (IPTR)gauge_position(w, e),
                TAG_DONE);
        }
        break;

    case W_TOGGLE:
        if (uw->control) {
            /* NoNotify matters: without it, reflecting the server's state
             * would fire our own notification and send a command straight
             * back, so an external change would ping-pong. */
            SetAttrs(uw->control,
                MUIA_NoNotify, TRUE,
                MUIA_Selected, state_is_on(e) ? TRUE : FALSE,
                TAG_DONE);
        }
        break;

    case W_MEDIA:
        media_describe(uw->media_text, sizeof uw->media_text, e);
        if (uw->value)
            set(uw->value, MUIA_Text_Contents, (IPTR)uw->media_text);
        media_describe_extra(uw->media_extra, sizeof uw->media_extra, e);
        if (uw->stamp)
            set(uw->stamp, MUIA_Text_Contents, (IPTR)uw->media_extra);
        media_volume(uw->media_vol_text, sizeof uw->media_vol_text, e);
        if (uw->media_vol)
            set(uw->media_vol, MUIA_Text_Contents, (IPTR)uw->media_vol_text);
        break;

    case W_DIMMER: {
        /*
         * -1 means the lamp reports no brightness at all, which is what an
         * "off" light does. Show a dash rather than 0%: zero would claim
         * the user had dimmed it right down.
         *
         * NoNotify for the same reason the toggle needs it -- without it,
         * showing what the server said fires our own notification and
         * sends it straight back.
         */
        int pct = ha_attr_pct(e, "brightness", 1);

        uw->armed = 1;
        if (uw->send_at)
            break;              /* the user is still dragging; leave it be */

        /*
         * Only when it actually differs. Writing a value a gadget already
         * holds is not free: MUI still does the work, and this loop only
         * services its sockets on a pass where MUI has nothing pending, so
         * a control rewritten on every state update starved the network
         * outright -- no camera ever fetched a picture, Ctrl-C stopped
         * working, and it fell over from there. The guard is also why
         * `shown` exists: it is what we last put in the gadget.
         */
        if (uw->control && (pct < 0 ? 0 : pct) != uw->shown) {
            SetAttrs(uw->control,
                MUIA_NoNotify,     TRUE,
                MUIA_Slider_Level, (IPTR)(pct < 0 ? 0 : pct),
                TAG_DONE);
            /*
             * Read back what it actually took, not what we asked for. A
             * slider does not always hold the exact number handed to it,
             * and remembering the request instead means the next poll
             * reads a different value, decides the user moved the knob,
             * and sends it to the light -- which answers with a new
             * value, and round it goes. One lamp really did walk its own
             * colour away like that.
             */
            uw->shown = (int)xget(uw->control, MUIA_Slider_Level);
        }
        if (uw->value) {
            if (pct < 0)
                strcpy(uw->text, state_is_on(e) ? "on" : "-");
            else
                sprintf(uw->text, "%d%%", pct);
            set(uw->value, MUIA_Text_Contents, (IPTR)uw->text);
        }
        break;
    }

    case W_COLOR: {
        int r = 0, g = 0, b = 0;

        if (uw->send_at)
            break;
        /*
         * Armed only once the lamp has told us a colour, and disabled
         * until then. A Coloradjust invents a value for itself -- white --
         * and notifies about it as it lays out, so arming on any state at
         * all was still enough to push white at full brightness to a lamp
         * that was merely switched off. An off lamp reports no rgb_color,
         * so it never arms, and a gadget that cannot be touched cannot
         * fire. It comes alive when the light does.
         */
        if (uw->control && ha_attr_rgb(e, "rgb_color", &r, &g, &b)) {
            int packed = (r << 16) | (g << 8) | b;
            int gun[3], c;

            if (!uw->armed) {
                uw->armed = 1;
                set(uw->control, MUIA_Disabled, FALSE);
            }
            if (packed == uw->shown)
                break;                      /* nothing changed; see above */

            gun[0] = r; gun[1] = g; gun[2] = b;
            for (c = 0; c < 3; c++)
                if (uw->rgb[c])
                    SetAttrs(uw->rgb[c],
                        MUIA_NoNotify,     TRUE,
                        MUIA_Slider_Level, (IPTR)gun[c],
                        TAG_DONE);

            /* What they actually took, for the same reason as the dimmer. */
            uw->shown = 0;
            for (c = 0; c < 3; c++)
                uw->shown = (uw->shown << 8) |
                            (uw->rgb[c] ? (int)xget(uw->rgb[c], MUIA_Slider_Level)
                                        : 0);

            /* A Colorfield gun is 32 bits and the wire carries 8, and the
             * conversion has to fill the whole word: 0xFF becomes
             * 0xFFFFFFFF, not 0xFF000000, or white comes out grey. */
            if (uw->swatch)
                SetAttrs(uw->swatch,
                    MUIA_Colorfield_Red,   (IPTR)((ULONG)r * 0x01010101UL),
                    MUIA_Colorfield_Green, (IPTR)((ULONG)g * 0x01010101UL),
                    MUIA_Colorfield_Blue,  (IPTR)((ULONG)b * 0x01010101UL),
                    TAG_DONE);
        } else if (uw->control && !uw->armed) {
            set(uw->control, MUIA_Disabled, TRUE);
        }
        break;
    }

    case W_COVER: {
        /* A cover reports 0..100 already, so it is not rescaled. When it
         * reports nothing, fall back to the state -- open, closed,
         * opening, closing -- which is still more than a checkbox says. */
        int pos = ha_attr_pct(e, "current_position", 0);

        if (uw->value) {
            if (pos < 0) {
                strncpy(uw->text, e->state[0] ? e->state : "-",
                        sizeof uw->text - 1);
                uw->text[sizeof uw->text - 1] = '\0';
            } else {
                sprintf(uw->text, "%d%%", pos);
            }
            set(uw->value, MUIA_Text_Contents, (IPTR)uw->text);
        }
        break;
    }

    case W_BUTTON:
    case W_TEXT:
    case W_CAMERA:
        break;
    }
}

void ui_entity_changed(a2h_ui *ui, const ha_entity *e)
{
    int i;

    if (!ui || !e)
        return;

    /* A linear scan over at most CFG_MAX_WIDGETS entries, only when an
     * entity actually changes. Not worth an index. */
    for (i = 0; i < ui->nwidgets; i++)
        if (strcmp(ui->cfg->widgets[i].entity, e->entity_id) == 0)
            update_widget(ui, i, e);
}

void ui_refresh_all(a2h_ui *ui)
{
    int i;

    if (!ui)
        return;
    for (i = 0; i < ui->nwidgets; i++) {
        const a2h_widget *w = &ui->cfg->widgets[i];
        if (w->entity[0])
            update_widget(ui, i, ha_store_get(&ui->ha->store, w->entity));
    }
}

/* ------------------------------------------------------------------ *
 * Actions
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Camera snapshots
 * ------------------------------------------------------------------ */

/*
 * Where a snapshot lands on its way to the decoder. T: is RAM on any normal
 * setup, which matters: this is rewritten every refresh and a real disk
 * would grind for no reason.
 *
 * Two names, used alternately. Datatypes and MUI both key off the file, and
 * writing over the picture that is currently on screen is asking for a
 * decoder to read a half-written file.
 */
static void snapshot_path(char *out, size_t outsz, int widget, int slot)
{
    snprintf(out, outsz, "T:ami2ha-cam%d-%d.jpg", widget, slot ? 1 : 0);
}

/*
 * Caption a camera tile with the time its picture arrived.
 *
 * This is the Amiga's clock, not the camera's: it says when ami2ha last
 * looked, which is the thing the tile cannot otherwise tell you. Most
 * cameras burn their own timestamp into the image, and that one is the
 * moment the shutter fired -- the two are not the same, and on a camera
 * that takes ten seconds to wake they can differ by that much.
 *
 * DateToStr rather than arithmetic on the DateStamp: it honours the
 * user's locale settings, so the date reads the way the rest of their
 * Workbench does.
 */
static void stamp_now(a2h_ui *ui, int i)
{
    ui_widget       *uw = &ui->widgets[i];
    struct DateTime  dt;
    char             date[LEN_DATSTRING], time[LEN_DATSTRING];

    if (!uw->stamp)
        return;

    memset(&dt, 0, sizeof dt);
    DateStamp(&dt.dat_Stamp);
    dt.dat_Format  = FORMAT_DOS;
    dt.dat_StrDate = (STRPTR)date;
    dt.dat_StrTime = (STRPTR)time;
    date[0] = time[0] = '\0';

    if (!DateToStr(&dt)) {
        strcpy(uw->stamp_text, "--");
    } else {
        /* Seconds are noise on a tile that updates every few minutes. */
        char *colon = strrchr(time, ':');
        if (colon)
            *colon = '\0';
        snprintf(uw->stamp_text, sizeof uw->stamp_text, "%s %s", date, time);
    }

    set(uw->stamp, MUIA_Text_Contents, (IPTR)uw->stamp_text);
}

/*
 * Keep the picture that is on screen. The snapshot is already a file -- the
 * fetch wrote it and the datatype is still reading from it -- so this is a
 * copy, not a re-encode, and it keeps whichever frame the user is looking at
 * rather than fetching a newer one behind their back.
 *
 * Reading the displayed slot is safe: the datatype's lock stops it being
 * written, not read.
 */
static void camera_save(a2h_ui *ui, int i)
{
    const a2h_widget *w;
    ui_widget        *uw;
    char  src[64], dest[CFG_PATH_MAX + 64], name[64], stem[CFG_LABEL_MAX];
    struct DateStamp ds;
    struct ClockData cd;
    BPTR  in, out;
    char *buf;
    long  got;
    /*
     * Unconfigured, snapshots go in a drawer beside the program: it always
     * exists, it survives a reboot, and it is where someone would look for
     * pictures a program saved. Not RAM: -- T: is already RAM: on a normal
     * system, so that would copy a file from RAM to RAM and still lose it at
     * the next boot, which is not what "save" means.
     */
    const char *dir = ui->cfg->savedir[0] ? ui->cfg->savedir
                                          : "PROGDIR:snapshots";

    if (i < 0 || i >= ui->nwidgets)
        return;
    w  = &ui->cfg->widgets[i];
    uw = &ui->widgets[i];

    if (w->kind != W_CAMERA)
        return;
    if (!uw->pic.dt) {
        ui_flash_status(ui, "No picture to save yet");
        return;
    }

    /*
     * The label, not the entity: the drawer should read "Einfahrt-..." the
     * way the tile does, not "driveway_f-...". cfg_label_filename falls back
     * to the entity when there is no usable label.
     */
    cfg_label_filename(stem, sizeof stem, w->label, w->entity);

    /*
     * Amiga2Date rather than DateToStr: a file name wants numbers, and
     * DateToStr gives the month as a name whatever format is asked for once
     * a locale is in play -- which turned the first saved files into
     * "...-2625-223410.jpg", the month having been dropped by the filter that
     * kept only digits. This is locale-proof and sorts properly.
     */
    DateStamp(&ds);
    Amiga2Date((ULONG)ds.ds_Days * 86400UL + (ULONG)ds.ds_Minute * 60UL +
               (ULONG)ds.ds_Tick / TICKS_PER_SECOND, &cd);

    /*
     * Thirty characters, no more: that is the limit on a classic Amiga
     * filesystem, and it truncates silently -- the first file saved here
     * became "driveway_fluent-20260825-22464", losing the extension and with
     * it any hope of a script recognising it. The date and time are worth 16
     * and the extension 4, so the label gets the remaining 10.
     */
    snprintf(name, sizeof name, "%.10s-%04d%02d%02d-%02d%02d%02d.jpg",
             stem, (int)cd.year, (int)cd.month, (int)cd.mday,
             (int)cd.hour, (int)cd.min, (int)cd.sec);

    snprintf(dest, sizeof dest, "%s", dir);
    if (!AddPart((STRPTR)dest, (STRPTR)name, sizeof dest)) {
        ui_flash_status(ui, "The savedir path is too long");
        return;
    }

    snapshot_path(src, sizeof src, i, uw->cam_slot);

    in = Open((STRPTR)src, MODE_OLDFILE);
    if (!in) {
        ui_flash_status(ui, "The snapshot file has gone");
        return;
    }

    out = Open((STRPTR)dest, MODE_NEWFILE);
    if (!out) {
        /*
         * Most likely the directory does not exist yet. Make it once and try
         * again -- a save should not fail on a folder.
         *
         * CreateDir hands back a *lock* on what it made, not a success flag.
         * Treating it as a boolean leaves the directory locked for as long as
         * the program runs, which shows up as "object is in use" from List and
         * "directory not available, error 205" from Directory Opus -- a folder
         * that plainly exists and cannot be opened.
         */
        BPTR made = CreateDir((STRPTR)dir);
        if (made) {
            UnLock(made);
            out = Open((STRPTR)dest, MODE_NEWFILE);
        }
    }
    if (!out) {
        Close(in);
        ui_flash_status(ui, "Could not write to savedir");
        return;
    }

    buf = malloc(8192);
    if (!buf) {
        Close(in);
        Close(out);
        ui_flash_status(ui, "Out of memory");
        return;
    }

    while ((got = Read(in, buf, 8192)) > 0)
        if (Write(out, buf, got) != got) {
            got = -1;
            break;
        }

    free(buf);
    Close(in);
    Close(out);

    if (got < 0) {
        DeleteFile((STRPTR)dest);
        ui_flash_status(ui, "The save did not finish");
    } else {
        char said[128];
        snprintf(said, sizeof said, "Saved %s", name);
        ui_flash_status(ui, said);
    }
}

/* Ask for a new frame for widget `i`, unless a fetch is already running. */
static void request_snapshot(a2h_ui *ui, int i)
{
    const a2h_widget *w = &ui->cfg->widgets[i];
    char              path[CAM_PATH_MAX];
    char              msg[96];

    if (w->kind != W_CAMERA)
        return;

    if (camfetch_busy(&ui->cam)) {
        ui_flash_status(ui, "Already fetching a picture...");
        return;
    }

    /*
     * Alternate, so the file being written is never the one the picture on
     * screen was decoded from -- the datatype keeps that file open, and
     * MODE_NEWFILE on it fails with "cannot write the snapshot file".
     *
     * Only the displayed slot may be flipped against: testing "is there a
     * picture" instead would pick slot 1 forever from the second refresh
     * onwards, which is exactly the case that broke.
     */
    ui->widgets[i].cam_want = ui->widgets[i].pic.dt
                                ? (ui->widgets[i].cam_slot ? 0 : 1)
                                : 0;
    snapshot_path(path, sizeof path, i, ui->widgets[i].cam_want);

    if (!camfetch_start(&ui->cam, &ui->ha->cfg, w->entity,
                        w->cam_w, w->cam_h, path, i, ui_now())) {
        snprintf(msg, sizeof msg, "%s: %s", w->label,
                 ui->cam.err[0] ? ui->cam.err : "could not start");
        ui_flash_status(ui, msg);
        return;
    }

    snprintf(msg, sizeof msg, "Fetching %s...", w->label);
    ui_set_status(ui, msg);
}

/*
 * A fetch finished. Decode it onto the window's screen and hand the bitmap
 * to MUI.
 */
static void snapshot_arrived(a2h_ui *ui)
{
    int               i = ui->cam.widget;
    const a2h_widget *w;
    ui_widget        *uw;
    struct Screen    *scr = NULL;
    char              err[96];
    char              msg[128];

    if (i < 0 || i >= ui->nwidgets)
        return;

    w  = &ui->cfg->widgets[i];
    uw = &ui->widgets[i];

    if (!uw->control)
        return;

    scr = (struct Screen *)xget(ui->win, MUIA_Window_Screen);
    if (!scr) {
        ui_set_status_connected(ui);
        ui_flash_status(ui, "No screen to draw the picture on");
        return;
    }

    /*
     * Point MUI away from the old bitmap before the picture that owns it is
     * freed, or it would keep drawing from memory that has been given back.
     */
    set(uw->control, MUIA_Bitmap_Bitmap, (IPTR)NULL);

    if (!campic_load(&uw->pic, ui->cam.file, scr, err, sizeof err)) {
        ui_set_status_connected(ui);
        snprintf(msg, sizeof msg, "%s: %s", w->label, err);
        ui_flash_status(ui, msg);
        return;
    }

    /* campic_load freed the previous picture, so the file it was holding
     * open is free again and becomes the next target. */
    uw->cam_slot = uw->cam_want;

    SetAttrs(uw->control,
             MUIA_Bitmap_Width,  (IPTR)uw->pic.width,
             MUIA_Bitmap_Height, (IPTR)uw->pic.height,
             MUIA_Bitmap_Bitmap, (IPTR)uw->pic.bm,
             TAG_DONE);

    stamp_now(ui, i);

    ui_set_status_connected(ui);
    snprintf(msg, sizeof msg, "%s updated", w->label);
    ui_flash_status(ui, msg);
}

/*
 * Automatic refresh, for cameras configured with one. Deliberately not the
 * default: a frame costs a transfer and a JPEG decode, and some cameras
 * take ten seconds to answer, so polling every tile every minute would keep
 * a slow machine permanently busy for pictures nobody is looking at.
 */
static void camera_tick(a2h_ui *ui)
{
    long now = ui_now();
    int  i;

    if (camfetch_busy(&ui->cam))
        return;

    for (i = 0; i < ui->nwidgets; i++) {
        const a2h_widget *w  = &ui->cfg->widgets[i];
        ui_widget        *uw = &ui->widgets[i];

        if (w->kind != W_CAMERA)
            continue;

        /*
         * cam_next is 0 until a tile has been asked for once. Every camera
         * gets that first frame, even a manual one -- an empty frame with
         * no hint that clicking it would do something is a poor greeting.
         */
        if (uw->cam_next == 0) {
            uw->cam_next = (w->cam_refresh > 0)
                             ? now + (long)w->cam_refresh * 50L
                             : -1;   /* manual from here on */
            request_snapshot(ui, i);
            return;
        }

        if (w->cam_refresh <= 0 || uw->cam_next < 0 || now < uw->cam_next)
            continue;

        uw->cam_next = now + (long)w->cam_refresh * 50L;
        request_snapshot(ui, i);
        return;   /* one at a time */
    }
}

/*
 * Press one of a media widget's transport buttons. Every one of these is a
 * service that takes no data, which is why the whole transport is five
 * service names and no new plumbing. volume_set would need a value, and a
 * slider with it -- these two step it instead.
 */
static void fire_media(a2h_ui *ui, int i, int action)
{
    static const char *const service[MEDIA_ACTIONS] = {
        "media_previous_track", "media_play_pause", "media_next_track",
        "volume_down", "volume_up"
    };
    static const char *const said[MEDIA_ACTIONS] = {
        "Previous...", "Play/pause...", "Next...", "Volume down...",
        "Volume up..."
    };
    const a2h_widget *w;

    if (i < 0 || i >= ui->nwidgets || action < 0 || action >= MEDIA_ACTIONS)
        return;

    w = &ui->cfg->widgets[i];
    if (w->kind != W_MEDIA || !w->entity[0])
        return;

    ha_client_call_service(ui->ha, "media_player", service[action],
                           w->entity, NULL);
    ui_flash_status(ui, said[action]);
}

/* Both live further down; pending_tick sends, so it needs them here. */
static int  flush_output(a2h_ui *ui);
static void connection_lost(a2h_ui *ui, const char *why);

/* Send anything whose level has stopped moving. Returns ms until the next
 * one is due, or -1 when nothing is pending, in the shape the main loop's
 * other tick functions use. */
static long pending_tick(a2h_ui *ui)
{
    long now  = ui_now();
    long next = -1;
    int  i, sent = 0;

    for (i = 0; i < ui->nwidgets; i++) {
        const a2h_widget *w  = &ui->cfg->widgets[i];
        ui_widget        *uw = &ui->widgets[i];
        char              data[64];

        /*
         * Read what the gadget is showing. Polling rather than being told:
         * see bind_controls. A drag moves the knob many times between two
         * passes of this loop and only the value it settles on matters.
         */
        if (uw->armed && uw->control &&
            (w->kind == W_DIMMER || w->kind == W_COLOR)) {
            int now_val;

            if (w->kind == W_DIMMER) {
                now_val = (int)xget(uw->control, MUIA_Slider_Level);
            } else {
                int r = uw->rgb[0] ? (int)xget(uw->rgb[0], MUIA_Slider_Level) : 0;
                int g = uw->rgb[1] ? (int)xget(uw->rgb[1], MUIA_Slider_Level) : 0;
                int b = uw->rgb[2] ? (int)xget(uw->rgb[2], MUIA_Slider_Level) : 0;
                now_val = (r << 16) | (g << 8) | b;
            }

            if (now_val != uw->shown) {
                uw->shown   = now_val;
                uw->pending = now_val;
                uw->send_at = now + SETTLE_TICKS;
                if (w->kind == W_DIMMER && uw->value) {
                    sprintf(uw->text, "%d%%", now_val);
                    set(uw->value, MUIA_Text_Contents, (IPTR)uw->text);
                } else if (w->kind == W_COLOR && uw->swatch) {
                    /* So the preview follows the sliders as they move,
                     * rather than waiting for the lamp to answer. */
                    SetAttrs(uw->swatch,
                        MUIA_Colorfield_Red,
                            (IPTR)((ULONG)((now_val >> 16) & 0xFF) * 0x01010101UL),
                        MUIA_Colorfield_Green,
                            (IPTR)((ULONG)((now_val >> 8) & 0xFF) * 0x01010101UL),
                        MUIA_Colorfield_Blue,
                            (IPTR)((ULONG)(now_val & 0xFF) * 0x01010101UL),
                        TAG_DONE);
                }
            }
        }

        if (uw->send_at == 0)
            continue;

        if (now < uw->send_at) {
            long ms = (uw->send_at - now) * 20;
            if (next < 0 || ms < next)
                next = ms;
            continue;
        }

        uw->send_at = 0;

        if (w->kind == W_DIMMER) {
            if (ha_json_brightness_pct(data, sizeof data, uw->pending))
                ha_client_call_service(ui->ha, "light", "turn_on",
                                       w->entity, data);
        } else if (w->kind == W_COLOR) {
            if (ha_json_rgb_color(data, sizeof data,
                                  (uw->pending >> 16) & 0xFF,
                                  (uw->pending >> 8) & 0xFF,
                                  uw->pending & 0xFF))
                ha_client_call_service(ui->ha, "light", "turn_on",
                                       w->entity, data);
        }
        ui_flash_status(ui, w->label);
        sent = 1;
    }

    /*
     * Only when something actually went out. Flushing on every pass of the
     * loop instead put connection_lost() one failed write away from firing
     * at any moment -- including in the middle of a camera fetch, which
     * runs on its own socket and does not survive having the world torn
     * down underneath it. That crashed the program a few seconds after the
     * first snapshot every time, and left the tiles empty.
     */
    if (sent && !flush_output(ui))
        connection_lost(ui, "Send failed");

    return next;
}

/*
 * Open, Stop, Close. Sent the moment the button is released rather than
 * through the settle timer: a blind travelling the wrong way is stopped by
 * pressing Stop, and a third of a second of politeness is a third of a
 * second of the blind still moving.
 */
static void fire_cover(a2h_ui *ui, int i, int action)
{
    const a2h_widget *w;
    const char       *service = ha_cover_service(action);

    if (i < 0 || i >= ui->nwidgets || !service)
        return;
    w = &ui->cfg->widgets[i];
    if (w->kind != W_COVER)
        return;

    ha_client_call_service(ui->ha, "cover", service, w->entity, NULL);
    ui_flash_status(ui, w->label);
}

static void fire_widget(a2h_ui *ui, int i)
{
    const a2h_widget *w = &ui->cfg->widgets[i];

    if (i < 0 || i >= ui->nwidgets)
        return;

    if (w->kind == W_TOGGLE) {
        /* Send the state the user just asked for rather than a blind
         * toggle: if our view were stale, a toggle would do the opposite of
         * what they clicked. */
        int want_on = (int)xget(ui->widgets[i].control, MUIA_Selected);
        ha_client_turn(ui->ha, w->entity, want_on);
        ui_flash_status(ui, want_on ? "Switching on..." : "Switching off...");
    } else if (w->kind == W_BUTTON) {
        char domain[24], service[32];
        const char *dot = strchr(w->service, '.');

        if (dot) {
            size_t dn = (size_t)(dot - w->service);
            if (dn >= sizeof domain)
                dn = sizeof domain - 1;
            memcpy(domain, w->service, dn);
            domain[dn] = '\0';
            strncpy(service, dot + 1, sizeof service - 1);
            service[sizeof service - 1] = '\0';

            ha_client_call_service(ui->ha, domain, service,
                                   w->entity[0] ? w->entity : NULL,
                                   w->data[0] ? w->data : NULL);
            ui_flash_status(ui, w->label);
        }
    } else if (w->kind == W_CAMERA) {
        request_snapshot(ui, i);
    }
}

/* ------------------------------------------------------------------ *
 * Keeping the connection
 * ------------------------------------------------------------------ */

/*
 * What the program is, and what this copy of it is currently talking to.
 * The connection details are the part worth having: when something is not
 * updating, the first question is always which server it is asking.
 */
static void show_about(a2h_ui *ui)
{
    char text[512];

    sprintf(text,
        "%s %s\n\n"
        "A Home Assistant client for AmigaOS.\n"
        "MIT licensed. https://github.com/evil4dmin/ami2ha\n\n"
        "Server: %s:%d\n"
        "Home Assistant: %s\n"
        "Entities: %lu\n"
        "ARexx port: %s",
        A2H_NAME, A2H_VERSION,
        ui->ha->cfg.host, ui->ha->cfg.port,
        ui->ha->version[0] ? ui->ha->version : "not connected",
        (unsigned long)ha_store_count(&ui->ha->store),
        ui->rexx ? rexx_portname(ui->rexx) : "none");

    MUI_Request(ui->app, ui->win, 0, (char *)A2H_NAME, (char *)"Ok",
                (char *)"%s", text);
}

void ui_set_status_connected(a2h_ui *ui)
{
    char st[96];

    if (!ui)
        return;
    sprintf(st, "HA %s, %lu entities",
            ui->ha->version[0] ? ui->ha->version : "?",
            (unsigned long)ha_store_count(&ui->ha->store));
    ui_set_status(ui, st);
}

/* Say what happened and arrange to try again, less eagerly each time. */
static void schedule_retry(a2h_ui *ui, const char *why)
{
    char msg[96];

    ui->link     = LINK_RETRYING;
    ui->retry_at = ui_now() + (long)ui->retry_secs * 50L;
    sprintf(msg, "%.56s -- retrying in %ds", why, ui->retry_secs);
    ui_set_status(ui, msg);

    /* Back off, so a server that is off for an hour is not hammered for
     * an hour. */
    ui->retry_secs *= 2;
    if (ui->retry_secs > RETRY_MAX_SECS)
        ui->retry_secs = RETRY_MAX_SECS;
}

/*
 * The link went away. The window stays up and keeps showing the last
 * readings -- they are still the best information there is, and blanking
 * them would lose more than it tells -- with the status line saying that
 * they are no longer live.
 */
static void connection_lost(a2h_ui *ui, const char *why)
{
    char reason[64];

    /* Copy first: `why` often points at the client's own error buffer,
     * which the reset below clears. */
    strncpy(reason, why ? why : "Disconnected", sizeof reason - 1);
    reason[sizeof reason - 1] = '\0';

    net_disconnect(ui->sock);
    ha_client_reset(ui->ha);

    /*
     * A refused token is the one failure worth giving up on. Home
     * Assistant bans an address after repeated failed logins, and the
     * token is read once at startup, so nothing can change until the
     * program is restarted -- retrying would only get the Amiga locked
     * out.
     */
    if (ui->ha->auth_rejected) {
        ui->link = LINK_GAVE_UP;
        ui_set_status(ui, "Token rejected -- restart with a valid token");
        return;
    }

    schedule_retry(ui, reason);
}

static void try_reconnect(a2h_ui *ui)
{
    int rc;

    ui->attempt_at = ui_now();
    ui_set_status(ui, "Reconnecting...");

    rc = net_connect(ui->sock, ui->ha->cfg.host, ui->ha->cfg.port,
                     ui->ha->cfg.tls);
    if (rc < 0) {
        schedule_retry(ui, net_error_text(rc));
        return;
    }
    if (rc == NET_OK)
        ha_client_begin(ui->ha);
    /* Otherwise the connect is under way; net_wait reports the socket
     * writable and service_socket() finishes it. */
}

/*
 * Drive the reconnect timer. Returns how long until the next thing is due
 * in milliseconds, so the event loop can sleep exactly that long, or -1
 * when the link wants no attention.
 */
static long link_tick(a2h_ui *ui)
{
    long now, left;

    if (ui->link != LINK_RETRYING)
        return -1;

    now = ui_now();

    if (net_socket_is_open(ui->sock)) {
        left = ui->attempt_at + ATTEMPT_TICKS - now;
        if (left <= 0 || left > ATTEMPT_TICKS) {   /* elapsed, or midnight */
            connection_lost(ui, "No answer");
            return 0;
        }
        return left * 20;
    }

    left = ui->retry_at - now;
    if (left <= 0 || left > (long)RETRY_MAX_SECS * 50L) {
        try_reconnect(ui);
        return 0;
    }
    return left * 20;
}

/* The link was heard from. Restarts the silence window. */
static void link_heard(a2h_ui *ui)
{
    ui->last_rx = ui_now();
}

/*
 * Watch a connection that is up for going quiet. Returns how long until the
 * next check is due in milliseconds, so the event loop can sleep exactly
 * that long, or -1 when there is nothing to watch.
 *
 * The keepalive is queued rather than sent here: the event loop already
 * notices anything waiting in the client's output buffer, asks select for
 * writability and reports a failed send through the same path as any other.
 */
static long link_watch(a2h_ui *ui)
{
    long now, idle, left;

    if (ui->link != LINK_UP || ui->ha->state != HA_ST_READY)
        return -1;

    now  = ui_now();
    idle = now - ui->last_rx;

    /* Midnight, or a clock someone has just set: begin the window again
     * rather than declare a healthy link dead. */
    if (idle < 0) {
        ui->last_rx = ui->last_ping = now;
        return IDLE_PING_TICKS * 20;
    }

    if (idle >= IDLE_DEAD_TICKS) {
        connection_lost(ui, "No reply from server");
        return 0;
    }

    /* One keepalive per silence, and only once nothing has been heard for
     * a while. The answer counts as traffic, which resets everything. */
    if (idle >= IDLE_PING_TICKS && ui->last_ping <= ui->last_rx) {
        ui->last_ping = now;
        ha_client_ping(ui->ha);
    }

    left = (ui->last_ping <= ui->last_rx ? IDLE_PING_TICKS
                                         : IDLE_DEAD_TICKS) - idle;
    if (left < 1)
        left = 1;
    return left * 20;
}

/*
 * Reconnect because the user asked. Worth having for the case this program
 * cannot detect on its own, and it saves waiting out a long backoff when a
 * server is known to be back.
 */
static void ui_reconnect_now(a2h_ui *ui)
{
    /*
     * A rejected token is read once at startup, so a retry would present
     * the same one -- and Home Assistant bans an address that keeps trying.
     * Say so instead.
     */
    if (ui->ha->auth_rejected) {
        ui_set_status(ui, "Token rejected -- restart with a valid token");
        return;
    }

    net_disconnect(ui->sock);
    ha_client_reset(ui->ha);
    ui->link       = LINK_RETRYING;
    ui->retry_secs = RETRY_FIRST_SECS;
    try_reconnect(ui);
}

/* ------------------------------------------------------------------ *
 * Event loop
 * ------------------------------------------------------------------ */

static int flush_output(a2h_ui *ui)
{
    a2h_buf *out = ha_client_out(ui->ha);

    while (out->len > 0) {
        long sent = net_send(ui->sock, out->data, out->len);
        if (sent == NET_WOULDBLOCK)
            return 1;
        if (sent < 0)
            return 0;
        buf_consume(out, (size_t)sent);
    }
    return 1;
}

/*
 * Move bytes in and out. Nothing here ends the program any more: a link
 * that fails is handed to connection_lost(), which keeps the window up
 * and schedules another attempt.
 */
static void service_socket(a2h_ui *ui, int readable, int writable)
{
    static unsigned char buf[2048]; /* keep it off the stack, as in main.c */

    if (net_connect_pending(ui->sock)) {
        /* A TLS handshake advances on whichever direction became ready,
         * not on writability alone the way a bare TCP connect does. */
        if (readable || writable) {
            int rc = net_connect_done(ui->sock);
            if (rc < 0) {
                const char *detail = net_tls_last_error();
                connection_lost(ui, detail ? detail : net_error_text(rc));
                return;
            }
            if (rc == NET_OK)
                ha_client_begin(ui->ha);
        }
        return;
    }

    if (writable && !flush_output(ui)) {
        connection_lost(ui, "Send failed");
        return;
    }

    /* Drain rather than read once: one TCP segment can carry several TLS
     * records, and select will not report the socket readable again for
     * bytes that are already buffered inside TLS. */
    while (readable || net_pending(ui->sock)) {
        long got = net_recv(ui->sock, buf, sizeof buf);
        /* One read per report of readiness; further passes have to be
         * justified by net_pending, or a busy socket never lets go. */
        readable = 0;
        if (got == NET_WOULDBLOCK)
            break;
        if (got == NET_CLOSED) {
            connection_lost(ui, "Connection closed");
            return;
        }
        if (got == NET_ERROR) {
            connection_lost(ui, "Network error");
            return;
        }
        if (got > 0) {
            link_heard(ui);
            if (!ha_client_feed(ui->ha, buf, (size_t)got)) {
                connection_lost(ui, ui->ha->error[0] ? ui->ha->error
                                                     : "Protocol error");
                return;
            }
        }
    }

    if (!flush_output(ui))
        connection_lost(ui, "Send failed");
}

int ui_run(a2h_ui *ui)
{
    ULONG sigs = 0;
    int   rc   = RETURN_OK;

    /* The first connection is made before the window opens, so the silence
     * window starts here rather than at a state change. */
    ui->last_rx = ui->last_ping = ui_now();

    for (;;) {
        long  status_ms, link_ms, watch_ms, send_ms, wait_ms;
        ULONG id;

        /*
         * Notice the link coming back. The first connection is made before
         * the window opens, so reaching READY here always means a
         * reconnect has just completed.
         */
        if (ui->link == LINK_RETRYING && ui->ha->state == HA_ST_READY) {
            ui->link       = LINK_UP;
            ui->retry_secs = RETRY_FIRST_SECS;
            ui->last_rx    = ui->last_ping = ui_now();
            ui_set_status_connected(ui);
            ui_refresh_all(ui);
        }

        status_ms = status_tick(ui);
        link_ms   = link_tick(ui);
        watch_ms  = link_watch(ui);
        send_ms   = pending_tick(ui);
        wait_ms   = status_ms;
        if (link_ms >= 0 && (wait_ms < 0 || link_ms < wait_ms))
            wait_ms = link_ms;
        if (watch_ms >= 0 && (wait_ms < 0 || watch_ms < wait_ms))
            wait_ms = watch_ms;
        /* A level waiting to be sent has to wake the loop, or it would sit
         * there until something else happened to. */
        if (send_ms >= 0 && (wait_ms < 0 || send_ms < wait_ms))
            wait_ms = send_ms;

        id = DoMethod(ui->app, MUIM_Application_NewInput, (IPTR)&sigs);

        if (id == MUIV_Application_ReturnID_Quit || id == ID_QUIT)
            break;

        if (id == ID_ABOUT) {
            show_about(ui);
            continue;
        }

        if (id == ID_RECONNECT) {
            ui_reconnect_now(ui);
            continue;
        }

        {
            int relayout = 0;
            if (editor_handle(ui->editor, id, &relayout)) {
                if (relayout)
                    ui_rebuild(ui);
                continue;
            }
        }

        if (id >= ID_CAMSAVE_BASE && id < ID_MEDIA_BASE) {
            camera_save(ui, (int)(id - ID_CAMSAVE_BASE));
            continue;
        }

        /*
         * Bounded. This was the last range in the ladder and so was written
         * open-ended, which quietly swallowed every ID added above it --
         * the cover buttons and the sliders both landed here and were
         * dispatched as media transport for a widget that did not exist.
         * Nothing crashed and nothing happened, which is the worst way for
         * it to fail.
         */
        if (id >= ID_MEDIA_BASE && id < ID_LEVEL_BASE) {
            ULONG rel = id - ID_MEDIA_BASE;
            fire_media(ui, (int)(rel / ID_MEDIA_STRIDE),
                           (int)(rel % ID_MEDIA_STRIDE));
            if (!flush_output(ui))
                connection_lost(ui, "Send failed");
            continue;
        }

        if (id >= ID_COVER_BASE && id < ID_COVER_BASE + 100000) {
            ULONG off = id - ID_COVER_BASE;
            fire_cover(ui, (int)(off / ID_COVER_STRIDE),
                       (int)(off % ID_COVER_STRIDE));
            if (!flush_output(ui))
                connection_lost(ui, "Send failed");
            continue;
        }

        if (id >= ID_WIDGET_BASE) {
            fire_widget(ui, (int)(id - ID_WIDGET_BASE));
            if (!flush_output(ui))
                connection_lost(ui, "Send failed");
            continue;
        }

        if (id != 0)
            continue; /* another MUI event; go round again */

        {
            int   readable = 0, writable = 0;
            int   cam_readable = 0, cam_writable = 0;
            int   want_write = net_want_write(ui->sock) ||
                               ha_client_out(ui->ha)->len > 0;
            int   cam_busy = camfetch_busy(&ui->cam);
            ULONG rexxsig = rexx_sigmask(ui->rexx);
            ULONG fired;

            /* A snapshot in flight must not be polled for: against a
             * battery camera it can be ten seconds, and this machine has
             * better things to do than spin. It goes into the same sleep. */
            if (cam_busy && (wait_ms < 0 || wait_ms > 250))
                wait_ms = 250;

            /* One sleep for MUI, both sockets, Ctrl-C and the ARexx port.
             * WaitSelect takes the Exec mask MUI just gave us, so nothing
             * polls. */
            fired = net_wait2(ui->sock, want_write,
                              cam_busy ? &ui->cam.sock : NULL,
                              cam_busy ? camfetch_want_write(&ui->cam) : 0,
                              sigs | SIGBREAKF_CTRL_C | rexxsig, wait_ms,
                              &readable, &writable,
                              &cam_readable, &cam_writable);

            if (fired & SIGBREAKF_CTRL_C)
                break;

            if (rexxsig && (fired & rexxsig)) {
                if (!rexx_poll(ui->rexx, ui->ha))
                    break;
                ui_refresh_all(ui);
            }

            /* Hand MUI back only the signals it asked about. */
            sigs = fired & sigs;

            if (cam_busy) {
                int r = camfetch_service(&ui->cam, cam_readable, cam_writable,
                                         ui_now());
                if (r > 0) {
                    snapshot_arrived(ui);
                    camfetch_cancel(&ui->cam);
                } else if (r < 0) {
                    char msg[128];
                    int  i = ui->cam.widget;
                    ui_set_status_connected(ui);
                    snprintf(msg, sizeof msg, "%s: %s",
                             (i >= 0 && i < ui->nwidgets)
                                 ? ui->cfg->widgets[i].label : "Camera",
                             ui->cam.err);
                    ui_flash_status(ui, msg);
                    camfetch_cancel(&ui->cam);
                }
            }

            camera_tick(ui);

            service_socket(ui, readable, writable);
        }
    }

    return rc;
}
