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

#include "ami2ha/json.h"
#include "ami2ha/ui.h"
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
    return IntuitionBase && MUIMasterBase;
}

static void ui_libs_close(void)
{
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
#define ID_WIDGET_BASE 1000

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

typedef struct {
    Object *value;   /* Text or Gauge showing the state; NULL for buttons */
    Object *control; /* Checkmark or Button; NULL for read-only widgets   */
    char    text[64];/* backing store for MUIA_Text_Contents             */
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

    a2h_config *cfg;
    ha_client  *ha;
    a2h_socket *sock;

    ui_widget  *widgets;
    int         nwidgets;

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

        if (!ui->widgets[i].control)
            continue;

        if (w->kind == W_TOGGLE)
            DoMethod(ui->widgets[i].control, MUIM_Notify, MUIA_Selected,
                     MUIV_EveryTime, (IPTR)ui->app, 2,
                     MUIM_Application_ReturnID, ID_WIDGET_BASE + i);
        else if (w->kind == W_BUTTON)
            DoMethod(ui->widgets[i].control, MUIM_Notify, MUIA_Pressed, FALSE,
                     (IPTR)ui->app, 2,
                     MUIM_Application_ReturnID, ID_WIDGET_BASE + i);
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
static int build_dashboard(a2h_ui *ui)
{
    /* The widget table is indexed by configuration order, so it has to
     * match the configuration's size. */
    int        want  = ui->cfg->nwidgets > 0 ? ui->cfg->nwidgets : 1;
    ui_widget *fresh = (ui_widget *)calloc((size_t)want, sizeof *fresh);

    if (!fresh)
        return 0;

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
        MUIA_Window_Title,     (IPTR)"ami2ha",
        /* What the Workbench title bar shows while this window is at the
         * front. Set on both windows, or it reverts to Workbench's own
         * text whenever the settings window is the active one. */
        MUIA_Window_ScreenTitle,(IPTR)"ami2ha",
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
        MUIA_Application_Version,    (IPTR)"$VER: ami2ha 0.1 (2026)",
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
    if (ui->app) {
        set(ui->win, MUIA_Window_Open, FALSE);
        MUI_DisposeObject(ui->app);
    }
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

    case W_BUTTON:
    case W_TEXT:
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
    }
}

/* ------------------------------------------------------------------ *
 * Keeping the connection
 * ------------------------------------------------------------------ */

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

    rc = net_connect(ui->sock, ui->ha->cfg.host, ui->ha->cfg.port);
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

    if (ui->sock->connecting) {
        if (writable) {
            int rc = net_connect_done(ui->sock);
            if (rc < 0) {
                connection_lost(ui, net_error_text(rc));
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

    if (readable) {
        long got = net_recv(ui->sock, buf, sizeof buf);
        if (got == NET_CLOSED) {
            connection_lost(ui, "Connection closed");
            return;
        }
        if (got == NET_ERROR) {
            connection_lost(ui, "Network error");
            return;
        }
        if (got > 0 && !ha_client_feed(ui->ha, buf, (size_t)got)) {
            connection_lost(ui, ui->ha->error[0] ? ui->ha->error
                                                 : "Protocol error");
            return;
        }
    }

    if (!flush_output(ui))
        connection_lost(ui, "Send failed");
}

int ui_run(a2h_ui *ui)
{
    ULONG sigs = 0;
    int   rc   = RETURN_OK;

    for (;;) {
        long  status_ms, link_ms, wait_ms;
        ULONG id;

        /*
         * Notice the link coming back. The first connection is made before
         * the window opens, so reaching READY here always means a
         * reconnect has just completed.
         */
        if (ui->link == LINK_RETRYING && ui->ha->state == HA_ST_READY) {
            ui->link       = LINK_UP;
            ui->retry_secs = RETRY_FIRST_SECS;
            ui_set_status_connected(ui);
            ui_refresh_all(ui);
        }

        status_ms = status_tick(ui);
        link_ms   = link_tick(ui);
        wait_ms   = status_ms;
        if (link_ms >= 0 && (wait_ms < 0 || link_ms < wait_ms))
            wait_ms = link_ms;

        id = DoMethod(ui->app, MUIM_Application_NewInput, (IPTR)&sigs);

        if (id == MUIV_Application_ReturnID_Quit || id == ID_QUIT)
            break;

        {
            int relayout = 0;
            if (editor_handle(ui->editor, id, &relayout)) {
                if (relayout)
                    ui_rebuild(ui);
                continue;
            }
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
            int   want_write = ui->sock->connecting ||
                               ha_client_out(ui->ha)->len > 0;
            ULONG rexxsig = rexx_sigmask(ui->rexx);
            ULONG fired;

            /* One sleep for MUI, the socket, Ctrl-C and the ARexx port.
             * WaitSelect takes the Exec mask MUI just gave us, so nothing
             * polls. */
            fired = net_wait(ui->sock, want_write,
                             sigs | SIGBREAKF_CTRL_C | rexxsig, wait_ms,
                             &readable, &writable);

            if (fired & SIGBREAKF_CTRL_C)
                break;

            if (rexxsig && (fired & rexxsig)) {
                if (!rexx_poll(ui->rexx, ui->ha))
                    break;
                ui_refresh_all(ui);
            }

            /* Hand MUI back only the signals it asked about. */
            sigs = fired & sigs;

            service_socket(ui, readable, writable);
        }
    }

    return rc;
}
