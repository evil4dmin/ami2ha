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

#include "ami2ha/json.h"
#include "ami2ha/ui.h"

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

typedef struct {
    Object *value;   /* Text or Gauge showing the state; NULL for buttons */
    Object *control; /* Checkmark or Button; NULL for read-only widgets   */
    char    text[64];/* backing store for MUIA_Text_Contents             */
} ui_widget;

struct a2h_ui {
    Object     *app;
    Object     *win;
    Object     *status;
    char        status_text[96];

    a2h_config *cfg;
    ha_client  *ha;
    a2h_socket *sock;

    ui_widget  *widgets;
    int         nwidgets;

    a2h_rexx   *rexx;   /* borrowed; may be NULL */

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
    Object           *row = NULL;

    uw->value   = NULL;
    uw->control = NULL;
    strcpy(uw->text, "-");

    switch (w->kind) {
    case W_TEXT:
        row = MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)w->label,
            MUIA_Text_PreParse, (IPTR)"\33c\33b",
            TAG_DONE);
        break;

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
        row = MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz,  TRUE,
            MUIA_Group_Child,  (IPTR)make_label(w->label),
            MUIA_Group_Child,  (IPTR)uw->value,
            TAG_DONE);
        break;

    case W_GAUGE:
        uw->value = MUI_NewObject(MUIC_Gauge,
            MUIA_Gauge_Horiz,   TRUE,
            MUIA_Gauge_Max,     (IPTR)1000,
            MUIA_Gauge_Current, (IPTR)0,
            MUIA_Gauge_InfoText,(IPTR)uw->text,
            MUIA_Frame,         MUIV_Frame_Gauge,
            MUIA_FixWidthTxt,   (IPTR)"-8888.8 XXXX",
            TAG_DONE);
        if (!uw->value)
            return 0;
        row = MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz,  TRUE,
            MUIA_Group_Child,  (IPTR)make_label(w->label),
            MUIA_Group_Child,  (IPTR)uw->value,
            TAG_DONE);
        break;

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
        row = MUI_NewObject(MUIC_Group,
            MUIA_Group_Horiz,  TRUE,
            MUIA_Group_Child,  (IPTR)make_label(w->label),
            MUIA_Group_Child,  (IPTR)uw->control,
            TAG_DONE);
        break;

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
        row = uw->control;
        break;
    }

    if (!row)
        return 0;

    DoMethod(parent, OM_ADDMEMBER, (IPTR)row);
    return 1;
}

a2h_ui *ui_create(a2h_config *cfg, ha_client *ha, a2h_socket *sock,
                  char *err, size_t errsz)
{
    a2h_ui *ui;
    Object *root;
    int     g, i;

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

    ui->nwidgets = cfg->nwidgets;
    ui->widgets  = (ui_widget *)calloc((size_t)(cfg->nwidgets > 0 ? cfg->nwidgets : 1),
                                       sizeof(ui_widget));
    if (!ui->widgets) {
        strncpy(err, "out of memory", errsz - 1);
        free(ui);
        ui_libs_close();
        return NULL;
    }

    strcpy(ui->status_text, "Connecting...");

    ui->status = MUI_NewObject(MUIC_Text,
        MUIA_Text_Contents, (IPTR)ui->status_text,
        MUIA_Text_PreParse, (IPTR)"\33l",
        MUIA_Frame,         MUIV_Frame_Text,
        TAG_DONE);

    /* An empty group to hang the configured groups off; children are added
     * below rather than in the varargs list, since the count is dynamic. */
    root = MUI_NewObject(MUIC_Group,
        MUIA_Group_Columns, (IPTR)cfg->columns,
        TAG_DONE);

    if (!root || !ui->status) {
        strncpy(err, "could not create MUI objects", errsz - 1);
        free(ui->widgets);
        free(ui);
        ui_libs_close();
        return NULL;
    }

    for (g = 0; g < cfg->ngroups; g++) {
        const a2h_group *grp = &cfg->groups[g];
        Object          *box;

        box = MUI_NewObject(MUIC_Group,
            MUIA_Frame,      MUIV_Frame_Group,
            MUIA_FrameTitle, (IPTR)grp->title,
            MUIA_Background, MUII_GroupBack,
            TAG_DONE);
        if (!box)
            continue;

        for (i = grp->first_widget; i < grp->first_widget + grp->nwidgets; i++)
            build_widget(ui, i, box);

        DoMethod(root, OM_ADDMEMBER, (IPTR)box);
    }

    ui->win = MUI_NewObject(MUIC_Window,
        MUIA_Window_Title,     (IPTR)"ami2ha",
        MUIA_Window_ID,        (IPTR)A2H_MAKE_ID('A','2','H','A'),
        MUIA_Window_RootObject,(IPTR)MUI_NewObject(MUIC_Group,
            MUIA_Group_Child, (IPTR)root,
            MUIA_Group_Child, (IPTR)ui->status,
            TAG_DONE),
        TAG_DONE);

    ui->app = MUI_NewObject(MUIC_Application,
        MUIA_Application_Title,      (IPTR)"ami2ha",
        MUIA_Application_Version,    (IPTR)"$VER: ami2ha 0.1 (2026)",
        MUIA_Application_Copyright,  (IPTR)"MIT licensed",
        MUIA_Application_Author,     (IPTR)"ami2ha contributors",
        MUIA_Application_Description,(IPTR)"Home Assistant client",
        MUIA_Application_Base,       (IPTR)"AMI2HA",
        MUIA_Application_Window,     (IPTR)ui->win,
        TAG_DONE);

    if (!ui->app || !ui->win) {
        strncpy(err, "could not create the window (is MUI installed?)", errsz - 1);
        if (ui->app)
            MUI_DisposeObject(ui->app);
        free(ui->widgets);
        free(ui);
        ui_libs_close();
        return NULL;
    }

    /* Closing the window quits. */
    DoMethod(ui->win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (IPTR)ui->app, 2, MUIM_Application_ReturnID, ID_QUIT);

    /* Each control reports its own widget index back to the loop. */
    for (i = 0; i < ui->nwidgets; i++) {
        const a2h_widget *w = &cfg->widgets[i];

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

void ui_set_status(a2h_ui *ui, const char *text)
{
    if (!ui || !ui->status)
        return;
    strncpy(ui->status_text, text, sizeof ui->status_text - 1);
    ui->status_text[sizeof ui->status_text - 1] = '\0';
    set(ui->status, MUIA_Text_Contents, (IPTR)ui->status_text);
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
        ui_set_status(ui, want_on ? "Switching on..." : "Switching off...");
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
            ui_set_status(ui, w->label);
        }
    }
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

static int service_socket(a2h_ui *ui, int readable, int writable)
{
    static unsigned char buf[2048]; /* keep it off the stack, as in main.c */

    if (ui->sock->connecting) {
        if (writable) {
            int rc = net_connect_done(ui->sock);
            if (rc < 0) {
                ui_set_status(ui, net_error_text(rc));
                return 0;
            }
            if (rc == NET_OK)
                ha_client_begin(ui->ha);
        }
        return 1;
    }

    if (writable && !flush_output(ui))
        return 0;

    if (readable) {
        long got = net_recv(ui->sock, buf, sizeof buf);
        if (got == NET_CLOSED) {
            ui_set_status(ui, "Connection closed by server");
            return 0;
        }
        if (got == NET_ERROR) {
            ui_set_status(ui, "Network error");
            return 0;
        }
        if (got > 0 && !ha_client_feed(ui->ha, buf, (size_t)got))
            return 0;
    }

    return flush_output(ui);
}

int ui_run(a2h_ui *ui)
{
    ULONG sigs = 0;
    int   rc   = RETURN_OK;

    for (;;) {
        ULONG id = DoMethod(ui->app, MUIM_Application_NewInput, (IPTR)&sigs);

        if (id == MUIV_Application_ReturnID_Quit || id == ID_QUIT)
            break;

        if (id >= ID_WIDGET_BASE) {
            fire_widget(ui, (int)(id - ID_WIDGET_BASE));
            if (!flush_output(ui)) {
                ui_set_status(ui, "Network error");
                rc = RETURN_FAIL;
                break;
            }
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
                             sigs | SIGBREAKF_CTRL_C | rexxsig, -1,
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

            if (!service_socket(ui, readable, writable)) {
                rc = RETURN_FAIL;
                break;
            }
        }
    }

    return rc;
}
