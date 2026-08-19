/*
 * ami2ha -- dashboard settings window
 *
 * Two lists: what Home Assistant offers on the left, what the dashboard
 * shows on the right, with the group being edited chosen above. Entries move
 * between them with the buttons, and the dashboard list can be dragged into
 * whatever order you want -- MUI's List does the reordering itself once
 * MUIA_Listview_DragType is set.
 */
#include "ami2ha/compat.h"

#include <libraries/mui.h>
#include <proto/muimaster.h>
#include <proto/intuition.h>

#include "ami2ha/cfgfile.h"
#include "ami2ha/version.h"
#include "editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One row in either list. Entries are pointers, so these must outlive them. */
typedef struct {
    char entity[HA_ENTITY_ID_MAX];
    char display[72];
} ed_row;

struct a2h_editor {
    Object *app;
    Object *win;
    Object *lv_pool, *list_pool;
    Object *lv_used, *list_used;
    Object *cyc_group, *list_kind;
    Object *bt_add, *bt_remove, *bt_gnew, *bt_gdel;
    Object *bt_up, *bt_down;
    Object *str_group, *str_label, *str_min, *str_max;
    /* Camera only, and the dashboard-wide column count. */
    Object *str_refresh, *str_camw, *str_camh, *cm_stamp;
    Object *cyc_columns;
    Object *bt_save, *bt_use, *bt_cancel;

    a2h_config *cfg;      /* the live configuration, edited on apply */
    a2h_config  backup;   /* what to restore if the user cancels     */
    ha_client  *ha;
    char        path[CFG_PATH_MAX];

    /* Working copy: rebuilt from cfg on show, written back on apply. */
    ed_row *pool;
    int     npool;
    ed_row *used;
    int     nused;
    int     group;        /* which group is being edited */

    const char **group_titles;   /* for the cycle gadget, NULL terminated */
    int          ngroup_titles;

    /*
     * Settings of widgets that are currently off the dashboard. Moving an
     * entity to another group means taking it out of one group and adding
     * it to the next, and between those two steps its widget no longer
     * exists in the configuration. Without somewhere to keep them, the
     * label and the chosen kind would be rediscovered from Home Assistant
     * and the user's choices lost.
     */
    a2h_widget *stash;
    int         nstash;

    /*
     * Which entity the properties box is showing. Edits are committed to
     * this one rather than to whatever is selected now: by the time a
     * click on another entry reaches us, the selection has already moved.
     */
    char shown[HA_ENTITY_ID_MAX];
};

/* MUI shows a list entry by asking this for the column text. */
HOOKPROTONH(DisplayFunc, LONG, char **array, ed_row *row)
{
    if (row)
        *array = row->display;
    else
        *array = (char *)"Entity";
    return 0;
}
MakeStaticHook(DisplayHook, DisplayFunc);

/*
 * The lists keep their own copy of every row. Without this MUI would hold
 * the pointer it was handed, which points into the working arrays -- and
 * those are freed and reallocated whenever the lists are rebuilt or read
 * back, leaving the lists full of dangling pointers. Entries then come
 * back with their first few bytes overwritten by the allocator, which
 * showed up as widgets losing their entity id and label.
 */
HOOKPROTONH(ConstructFunc, APTR, APTR pool, ed_row *row)
{
    ed_row *copy = (ed_row *)malloc(sizeof *copy);

    A2H_UNUSED(pool);
    if (copy)
        *copy = *row;
    return copy;
}
MakeStaticHook(ConstructHook, ConstructFunc);

HOOKPROTONH(DestructFunc, LONG, APTR pool, ed_row *row)
{
    A2H_UNUSED(pool);
    free(row);
    return 0;
}
MakeStaticHook(DestructHook, DestructFunc);

/* Order must match widget_kind: the cycle's active index is the kind. */
static const char *kind_labels[] = {
    "reading", "toggle", "button", "gauge", "text", "camera", NULL
};

/* Dashboard columns, as offered in the cycle. Index + 1 is the value. */
static const char *column_labels[] = { "1", "2", "3", "4", NULL };

/* ------------------------------------------------------------------ *
 * Building the working copy
 * ------------------------------------------------------------------ */

static void row_fill(ed_row *r, ha_client *ha, const char *entity,
                     const char *label, widget_kind kind)
{
    const ha_entity *e;

    strncpy(r->entity, entity, sizeof r->entity - 1);
    r->entity[sizeof r->entity - 1] = '\0';

    /* Prefer what the user sees on the dashboard, then Home Assistant's
     * friendly name, then the bare id. */
    if (label && *label)
        strncpy(r->display, label, sizeof r->display - 1);
    else if ((e = ha_store_get(&ha->store, entity)) != NULL && e->name[0])
        strncpy(r->display, e->name, sizeof r->display - 1);
    else
        strncpy(r->display, entity, sizeof r->display - 1);
    r->display[sizeof r->display - 1] = '\0';

    if (kind == W_TOGGLE || kind == W_BUTTON || kind == W_GAUGE) {
        size_t n = strlen(r->display);
        const char *suffix = (kind == W_TOGGLE) ? "  (toggle)"
                           : (kind == W_BUTTON) ? "  (button)" : "  (gauge)";
        if (n + strlen(suffix) < sizeof r->display)
            strcat(r->display, suffix);
    }
}

/* Remember what a widget looked like while it sits in the pool. */
static void stash_put(a2h_editor *ed, const a2h_widget *w)
{
    a2h_widget *grown;
    int         i;

    for (i = 0; i < ed->nstash; i++)
        if (strcmp(ed->stash[i].entity, w->entity) == 0) {
            ed->stash[i] = *w;
            return;
        }

    grown = (a2h_widget *)realloc(ed->stash,
                                  (size_t)(ed->nstash + 1) * sizeof *grown);
    if (!grown)
        return;   /* out of memory: the settings are lost, nothing worse */
    ed->stash = grown;
    ed->stash[ed->nstash++] = *w;
}

static const a2h_widget *stash_get(const a2h_editor *ed, const char *entity)
{
    int i;

    for (i = 0; i < ed->nstash; i++)
        if (strcmp(ed->stash[i].entity, entity) == 0)
            return &ed->stash[i];
    return NULL;
}

static int entity_is_used(const a2h_config *cfg, const char *entity)
{
    int i;

    for (i = 0; i < cfg->nwidgets; i++)
        if (strcmp(cfg->widgets[i].entity, entity) == 0)
            return 1;
    return 0;
}

/* The pool is whatever Home Assistant offers that is not on the dashboard. */
static void build_pool(a2h_editor *ed)
{
    const ha_entity *e;
    int              n = 0;

    free(ed->pool);
    ed->pool  = NULL;
    ed->npool = 0;

    for (e = ha_store_first(&ed->ha->store); e; e = ha_store_next(e))
        n++;
    if (n == 0)
        return;

    ed->pool = (ed_row *)calloc((size_t)n, sizeof *ed->pool);
    if (!ed->pool)
        return;

    for (e = ha_store_first(&ed->ha->store); e; e = ha_store_next(e)) {
        const a2h_widget *kept;

        if (entity_is_used(ed->cfg, e->entity_id))
            continue;
        kept = stash_get(ed, e->entity_id);
        row_fill(&ed->pool[ed->npool++], ed->ha, e->entity_id,
                 kept ? kept->label : NULL,
                 kept ? kept->kind  : W_SENSOR);
    }
}

static void build_used(a2h_editor *ed)
{
    const a2h_group *g;
    int              i, n = 0;

    free(ed->used);
    ed->used  = NULL;
    ed->nused = 0;

    if (ed->group < 0 || ed->group >= ed->cfg->ngroups)
        return;

    g = &ed->cfg->groups[ed->group];
    if (g->nwidgets <= 0)
        return;

    ed->used = (ed_row *)calloc((size_t)g->nwidgets, sizeof *ed->used);
    if (!ed->used)
        return;

    for (i = g->first_widget; i < g->first_widget + g->nwidgets; i++) {
        const a2h_widget *w = &ed->cfg->widgets[i];
        row_fill(&ed->used[n++], ed->ha, w->entity, w->label, w->kind);
    }
    ed->nused = n;
}

static void show_properties(a2h_editor *ed);
static int  apply(a2h_editor *ed);

static void fill_list(Object *list, ed_row *rows, int n)
{
    int i;

    DoMethod(list, MUIM_List_Clear);
    for (i = 0; i < n; i++)
        DoMethod(list, MUIM_List_InsertSingle, (IPTR)&rows[i],
                 MUIV_List_Insert_Bottom);
}

/*
 * Put the current group's title in the Name field.
 *
 * Its own function because two paths need it and they used to disagree:
 * refresh_groups did it, but choosing a different group from the cycle
 * only refreshed the lists, so the name went on showing the group you had
 * just left.
 */
static void show_group_name(a2h_editor *ed)
{
    if (!ed->str_group)
        return;

    SetAttrs(ed->str_group, MUIA_NoNotify, TRUE,
             MUIA_String_Contents,
             (IPTR)(ed->cfg->ngroups && ed->group < ed->cfg->ngroups
                        ? ed->cfg->groups[ed->group].title : ""),
             TAG_DONE);
}

static void refresh_lists(a2h_editor *ed)
{
    build_pool(ed);
    build_used(ed);
    set(ed->list_pool, MUIA_List_Quiet, TRUE);
    set(ed->list_used, MUIA_List_Quiet, TRUE);
    fill_list(ed->list_pool, ed->pool, ed->npool);
    fill_list(ed->list_used, ed->used, ed->nused);
    set(ed->list_pool, MUIA_List_Quiet, FALSE);
    set(ed->list_used, MUIA_List_Quiet, FALSE);
    show_properties(ed);
}

static void refresh_groups(a2h_editor *ed)
{
    int i;

    free(ed->group_titles);
    ed->group_titles = (const char **)calloc((size_t)ed->cfg->ngroups + 2,
                                             sizeof *ed->group_titles);
    if (!ed->group_titles)
        return;

    for (i = 0; i < ed->cfg->ngroups; i++)
        ed->group_titles[i] = ed->cfg->groups[i].title;
    if (ed->cfg->ngroups == 0)
        ed->group_titles[0] = "(no groups)";
    ed->ngroup_titles = ed->cfg->ngroups ? ed->cfg->ngroups : 1;

    {
        LONG want  = (ed->group < ed->ngroup_titles) ? ed->group : 0;
        LONG nudge = want ? 0 : (ed->ngroup_titles > 1 ? 1 : 0);

        /*
         * Three steps, and each one is needed.
         *
         * New entries have to land before an index into them means
         * anything, so they go on their own. Handing a Cycle new entries
         * makes it draw the first of them -- but it leaves the active
         * attribute alone. So on reopening with, say, group 3 still
         * selected, the attribute was already 3, setting it to 3 changed
         * nothing, nothing repainted, and the gadget went on showing the
         * first entry while the rest of the window showed group 3.
         *
         * Asking it for the active index gave 3 the whole time, which is
         * what made this look like a bookkeeping bug: only the label was
         * wrong. Hence the nudge -- a real change, so it repaints.
         */
        SetAttrs(ed->cyc_group,
                 MUIA_NoNotify,      TRUE,
                 MUIA_Cycle_Entries, (IPTR)ed->group_titles,
                 TAG_DONE);
        SetAttrs(ed->cyc_group,
                 MUIA_NoNotify,     TRUE,
                 MUIA_Cycle_Active, (IPTR)nudge,
                 TAG_DONE);
        SetAttrs(ed->cyc_group,
                 MUIA_NoNotify,     TRUE,
                 MUIA_Cycle_Active, (IPTR)want,
                 TAG_DONE);
    }

    show_group_name(ed);
}

/* ------------------------------------------------------------------ *
 * Applying the working copy back to the configuration
 * ------------------------------------------------------------------ */

/*
 * Rewrite the edited group's widgets in the order the list now shows,
 * keeping each widget's other settings. Widgets are stored as one flat
 * array with groups indexing into it, so the whole array is rebuilt.
 */
static int apply_order(a2h_editor *ed)
{
    a2h_config *cfg = ed->cfg;
    a2h_widget *rebuilt;
    int         out = 0, g, i, j;

    if (cfg->ngroups == 0)
        return 1;

    rebuilt = (a2h_widget *)calloc((size_t)(cfg->nwidgets + ed->nused + 1),
                                   sizeof *rebuilt);
    if (!rebuilt)
        return 0;

    for (g = 0; g < cfg->ngroups; g++) {
        a2h_group *grp   = &cfg->groups[g];
        int        first = out;

        if (g != ed->group) {
            for (i = grp->first_widget;
                 i < grp->first_widget + grp->nwidgets; i++)
                rebuilt[out++] = cfg->widgets[i];
        } else {
            /* Whatever is no longer in the list is on its way to another
             * group, or to the pool; keep its settings either way. */
            for (i = grp->first_widget;
                 i < grp->first_widget + grp->nwidgets; i++) {
                int still = 0;

                for (j = 0; j < ed->nused; j++)
                    if (strcmp(cfg->widgets[i].entity,
                               ed->used[j].entity) == 0) {
                        still = 1;
                        break;
                    }
                if (!still)
                    stash_put(ed, &cfg->widgets[i]);
            }

            /* The list is the order now; find each row's widget. */
            for (i = 0; i < ed->nused; i++) {
                int found = -1;

                for (j = grp->first_widget;
                     j < grp->first_widget + grp->nwidgets; j++) {
                    if (strcmp(cfg->widgets[j].entity, ed->used[i].entity) == 0) {
                        found = j;
                        break;
                    }
                }
                if (found >= 0) {
                    rebuilt[out] = cfg->widgets[found];
                } else {
                    const a2h_widget *kept = stash_get(ed, ed->used[i].entity);

                    if (kept) {
                        /* Came from another group: keep how it was set up. */
                        rebuilt[out] = *kept;
                        row_fill(&ed->used[i], ed->ha, kept->entity,
                                 kept->label, kept->kind);
                    } else {
                        /* Genuinely new: infer a sensible widget. */
                        a2h_config tmp;
                        cfg_init(&tmp);
                        if (cfg_add_discovered(&tmp, ed->used[i].entity, NULL)) {
                            rebuilt[out] = tmp.widgets[0];
                            row_fill(&ed->used[i], ed->ha, ed->used[i].entity,
                                     tmp.widgets[0].label, tmp.widgets[0].kind);
                        }
                        cfg_free(&tmp);
                    }
                }
                rebuilt[out].group = g;
                out++;
            }
        }
        grp->first_widget = first;
        grp->nwidgets     = out - first;
    }

    for (i = 0; i < out; i++)
        cfg->widgets[i] = rebuilt[i];
    cfg->nwidgets = out;
    free(rebuilt);
    return 1;
}

/* Read the dashboard list back, since dragging changed its order. */
static void read_back_order(a2h_editor *ed)
{
    ed_row *fresh;
    LONG    n = 0, i;

    get(ed->list_used, MUIA_List_Entries, &n);
    if (n <= 0) {
        ed->nused = 0;
        return;
    }

    fresh = (ed_row *)calloc((size_t)n, sizeof *fresh);
    if (!fresh)
        return;

    for (i = 0; i < n; i++) {
        ed_row *row = NULL;
        DoMethod(ed->list_used, MUIM_List_GetEntry, i, (IPTR)&row);
        if (row)
            fresh[i] = *row;
    }

    free(ed->used);
    ed->used  = fresh;
    ed->nused = (int)n;
}

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */

static Object *make_list(Object **listp, const char *heading, int draggable)
{
    Object *view;
    Object *list = MUI_NewObject(MUIC_List,
        MUIA_Frame,             MUIV_Frame_InputList,
        MUIA_List_DisplayHook,  (IPTR)&DisplayHook,
        MUIA_List_ConstructHook,(IPTR)&ConstructHook,
        MUIA_List_DestructHook, (IPTR)&DestructHook,
        TAG_DONE);

    *listp = list;
    if (!list)
        return NULL;

    view = MUI_NewObject(MUIC_Listview,
        MUIA_Listview_List,     (IPTR)list,
        /* Without a floor MUI shrinks the window to whatever the list
         * happens to hold, which is unusable for reordering. */
        MUIA_Listview_Input,    TRUE,
        /* Immediate drag lets the user reorder by dragging inside the
         * list; MUI does the moving itself. */
        MUIA_Listview_DragType, draggable ? MUIV_Listview_DragType_Immediate
                                          : MUIV_Listview_DragType_None,
        TAG_DONE);
    if (!view)
        return NULL;

    /*
     * The heading goes above the list rather than in it. MUIA_List_Title
     * draws inside the list frame in the same style as the entries, so it
     * read as one of them -- as though "Available" were an entity you
     * could select. VertWeight 0 keeps it at one line high; without it the
     * heading and the list would share the height between them.
     */
    return MUI_NewObject(MUIC_Group,
        MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
            MUIA_Text_Contents, (IPTR)heading,
            MUIA_Text_PreParse, (IPTR)"\33c\33b",
            MUIA_VertWeight,    (IPTR)0,
            TAG_DONE),
        MUIA_Group_Child, (IPTR)view,
        TAG_DONE);
}

static Object *make_button(const char *label)
{
    return MUI_NewObject(MUIC_Text,
        MUIA_Text_Contents, (IPTR)label,
        MUIA_Text_PreParse, (IPTR)"\33c",
        MUIA_Frame,         MUIV_Frame_Button,
        MUIA_Background,    MUII_ButtonBack,
        MUIA_InputMode,     MUIV_InputMode_RelVerify,
        TAG_DONE);
}

a2h_editor *editor_create(Object *app, a2h_config *cfg, ha_client *ha,
                          const char *path)
{
    a2h_editor *ed;

    ed = (a2h_editor *)calloc(1, sizeof *ed);
    if (!ed)
        return NULL;

    ed->app = app;
    ed->cfg = cfg;
    ed->ha  = ha;
    strncpy(ed->path, path ? path : "", sizeof ed->path - 1);

    ed->cyc_group = MUI_NewObject(MUIC_Cycle, TAG_DONE);
    ed->str_group = MUI_NewObject(MUIC_String,
        MUIA_Frame,          MUIV_Frame_String,
        MUIA_String_MaxLen,  (IPTR)CFG_TITLE_MAX,
        TAG_DONE);
    ed->str_label = MUI_NewObject(MUIC_String,
        MUIA_Frame,          MUIV_Frame_String,
        MUIA_String_MaxLen,  (IPTR)CFG_LABEL_MAX,
        TAG_DONE);
    ed->str_min = MUI_NewObject(MUIC_String,
        MUIA_Frame,          MUIV_Frame_String,
        MUIA_String_Accept,  (IPTR)"-0123456789",
        MUIA_String_MaxLen,  (IPTR)12,
        TAG_DONE);
    ed->str_max = MUI_NewObject(MUIC_String,
        MUIA_Frame,          MUIV_Frame_String,
        MUIA_String_Accept,  (IPTR)"-0123456789",
        MUIA_String_MaxLen,  (IPTR)12,
        TAG_DONE);
    ed->list_kind = MUI_NewObject(MUIC_Cycle,
        MUIA_Cycle_Entries, (IPTR)kind_labels, TAG_DONE);

    /*
     * Camera settings. The size is what gets asked of Home Assistant, which
     * scales server-side, so it decides both the transfer and how much
     * decoding this machine has to do -- not just how big the tile looks.
     */
    ed->str_refresh = MUI_NewObject(MUIC_String,
        MUIA_Frame,          MUIV_Frame_String,
        MUIA_String_Accept,  (IPTR)"0123456789",
        MUIA_String_MaxLen,  (IPTR)8,
        TAG_DONE);
    ed->str_camw = MUI_NewObject(MUIC_String,
        MUIA_Frame,          MUIV_Frame_String,
        MUIA_String_Accept,  (IPTR)"0123456789",
        MUIA_String_MaxLen,  (IPTR)6,
        TAG_DONE);
    ed->str_camh = MUI_NewObject(MUIC_String,
        MUIA_Frame,          MUIV_Frame_String,
        MUIA_String_Accept,  (IPTR)"0123456789",
        MUIA_String_MaxLen,  (IPTR)6,
        TAG_DONE);
    ed->cm_stamp = MUI_NewObject(MUIC_Image,
        MUIA_Frame,        MUIV_Frame_ImageButton,
        MUIA_InputMode,    MUIV_InputMode_Toggle,
        MUIA_Image_Spec,   (IPTR)MUII_CheckMark,
        MUIA_ShowSelState, FALSE,
        MUIA_Background,   MUII_ButtonBack,
        TAG_DONE);
    ed->cyc_columns = MUI_NewObject(MUIC_Cycle,
        MUIA_Cycle_Entries, (IPTR)column_labels, TAG_DONE);

    ed->bt_add    = make_button("Add >>");
    ed->bt_up     = make_button("Up");
    ed->bt_down   = make_button("Down");
    ed->bt_remove = make_button("<< Remove");
    ed->bt_gnew   = make_button("New group");
    ed->bt_gdel   = make_button("Delete group");
    ed->bt_save   = make_button("Save");
    ed->bt_use    = make_button("Use");
    ed->bt_cancel = make_button("Cancel");

    ed->lv_pool = make_list(&ed->list_pool, "Available HA entities", 0);
    ed->lv_used = make_list(&ed->list_used, "Selected entities", 1);

    if (!ed->lv_pool || !ed->lv_used || !ed->cyc_group) {
        free(ed);
        return NULL;
    }

    ed->win = MUI_NewObject(MUIC_Window,
        MUIA_Window_Title,  (IPTR)"ami2ha settings",
        MUIA_Window_ScreenTitle, (IPTR)A2H_TITLE,
        /* Lists need room to be usable; MUI would otherwise shrink the
         * window to the few rows it happens to hold at open time. */
        MUIA_Window_Width,  MUIV_Window_Width_Visible(48),
        MUIA_Window_Height, MUIV_Window_Height_Visible(55),
        MUIA_Window_ID,     (IPTR)(('A' << 24) | ('2' << 16) | ('E' << 8) | '2'),
        MUIA_Window_RootObject, (IPTR)MUI_NewObject(MUIC_Group,
            MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Group,
                MUIA_Group_Horiz, TRUE,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Group:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->cyc_group,
                MUIA_Group_Child, (IPTR)ed->bt_gnew,
                MUIA_Group_Child, (IPTR)ed->bt_gdel,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Name:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->str_group,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Columns:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->cyc_columns,
                TAG_DONE),
            MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Group,
                MUIA_Group_Horiz, TRUE,
                MUIA_Group_Child, (IPTR)ed->lv_pool,
                /*
                 * The spacers matter: a horizontal group's maximum height
                 * is capped by its least stretchy child, so a column of
                 * fixed-height buttons would pin the whole window to their
                 * height no matter what size was asked for.
                 */
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Group,
                    MUIA_Weight,      (IPTR)0,
                    MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Rectangle, TAG_DONE),
                    MUIA_Group_Child, (IPTR)ed->bt_add,
                    MUIA_Group_Child, (IPTR)ed->bt_remove,
                    MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Rectangle, TAG_DONE),
                    MUIA_Group_Child, (IPTR)ed->bt_up,
                    MUIA_Group_Child, (IPTR)ed->bt_down,
                    MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Rectangle, TAG_DONE),
                    TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->lv_used,
                TAG_DONE),
            MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Group,
                /*
                 * A grid of label/gadget pairs rather than one long row.
                 * Six pairs side by side would run off a 640-wide screen,
                 * and this wraps them onto as many rows as it needs.
                 */
                MUIA_Group_Columns, (IPTR)4,
                MUIA_Frame,       MUIV_Frame_Group,
                MUIA_FrameTitle,  (IPTR)"Properties",
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Label:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->str_label,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Show as:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->list_kind,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Min:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->str_min,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Max:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->str_max,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Refresh s:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->str_refresh,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Width:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->str_camw,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Height:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                MUIA_Group_Child, (IPTR)ed->str_camh,
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Text,
                    MUIA_Text_Contents, (IPTR)"Show time:",
                    MUIA_Weight, (IPTR)0, TAG_DONE),
                /* Keep the checkmark its natural size rather than stretched
                 * across the cell, as the dashboard's toggles do. */
                MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz, TRUE,
                    MUIA_Group_Child, (IPTR)ed->cm_stamp,
                    MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Rectangle,
                        TAG_DONE),
                    TAG_DONE),
                TAG_DONE),
            MUIA_Group_Child, (IPTR)MUI_NewObject(MUIC_Group,
                MUIA_Group_Horiz, TRUE,
                MUIA_Group_Child, (IPTR)ed->bt_save,
                MUIA_Group_Child, (IPTR)ed->bt_use,
                MUIA_Group_Child, (IPTR)ed->bt_cancel,
                TAG_DONE),
            TAG_DONE),
        TAG_DONE);

    if (!ed->win) {
        free(ed);
        return NULL;
    }

    DoMethod(app, OM_ADDMEMBER, (IPTR)ed->win);

    DoMethod(ed->win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_CANCEL);

    DoMethod(ed->bt_add, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_ADD);
    DoMethod(ed->bt_remove, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_REMOVE);
    DoMethod(ed->bt_gnew, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_GROUP_NEW);
    DoMethod(ed->bt_gdel, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_GROUP_DEL);
    DoMethod(ed->bt_up, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_UP);
    DoMethod(ed->bt_down, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_DOWN);
    DoMethod(ed->bt_save, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_SAVE);
    DoMethod(ed->bt_use, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_USE);
    DoMethod(ed->bt_cancel, MUIM_Notify, MUIA_Pressed, FALSE,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_CANCEL);
    DoMethod(ed->cyc_group, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_GROUP_SEL);
    DoMethod(ed->list_kind, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_KIND);
    DoMethod(ed->list_used, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_SELECT);
    DoMethod(ed->str_group, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_RENAME);
    DoMethod(ed->str_label, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_LABEL);
    DoMethod(ed->str_min, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_RANGE);
    DoMethod(ed->str_max, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_RANGE);
    /* Reuse ID_ED_RANGE: every one of these ends in commit_properties,
     * which is what the handler does before it looks at the id at all. */
    DoMethod(ed->str_refresh, MUIM_Notify, MUIA_String_Acknowledge,
             MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_ED_RANGE);
    DoMethod(ed->str_camw, MUIM_Notify, MUIA_String_Acknowledge,
             MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_ED_RANGE);
    DoMethod(ed->str_camh, MUIM_Notify, MUIA_String_Acknowledge,
             MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_ED_RANGE);
    DoMethod(ed->cm_stamp, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             (IPTR)app, 2, MUIM_Application_ReturnID, ID_ED_RANGE);

    return ed;
}

void editor_dispose(a2h_editor *ed)
{
    if (!ed)
        return;
    free(ed->pool);
    free(ed->used);
    free(ed->stash);
    free(ed->group_titles);
    cfg_free(&ed->backup);
    free(ed);
}

void editor_show(a2h_editor *ed)
{
    if (!ed)
        return;

    /* Show the dashboard's current column count. Clamped to what the cycle
     * can offer: a hand-written file may say anything, and an out-of-range
     * active index would leave the gadget showing something it is not. */
    {
        int col = ed->cfg->columns;
        if (col < 1) col = 1;
        if (col > 4) col = 4;
        SetAttrs(ed->cyc_columns, MUIA_NoNotify, TRUE,
                 MUIA_Cycle_Active, (IPTR)(col - 1), TAG_DONE);
    }

    /* A fresh session starts with nothing held back, so a label from a
     * run the user cancelled cannot come back on an entity they re-add. */
    free(ed->stash);
    ed->stash  = NULL;
    ed->nstash = 0;
    ed->shown[0] = '\0';

    /* Keep a copy so Cancel can put everything back. */
    cfg_free(&ed->backup);
    ed->backup = *ed->cfg;
    ed->backup.widgets = (a2h_widget *)calloc((size_t)(ed->cfg->nwidgets + 1),
                                              sizeof(a2h_widget));
    if (ed->backup.widgets) {
        memcpy(ed->backup.widgets, ed->cfg->widgets,
               (size_t)ed->cfg->nwidgets * sizeof(a2h_widget));
        ed->backup.widget_cap = ed->cfg->nwidgets + 1;
    } else {
        ed->backup.nwidgets = 0;
    }

    if (ed->group >= ed->cfg->ngroups)
        ed->group = 0;

    refresh_groups(ed);
    refresh_lists(ed);
    set(ed->win, MUIA_Window_Open, TRUE);
}

/* ------------------------------------------------------------------ *
 * Events
 * ------------------------------------------------------------------ */

static void move_selected(a2h_editor *ed, int to_dashboard)
{
    Object *from = to_dashboard ? ed->list_pool : ed->list_used;
    ed_row *row  = NULL;
    LONG    active = MUIV_List_Active_Off;

    get(from, MUIA_List_Active, &active);
    if (active == MUIV_List_Active_Off)
        return;

    DoMethod(from, MUIM_List_GetEntry, active, (IPTR)&row);
    if (!row)
        return;

    if (to_dashboard) {
        if (ed->cfg->ngroups == 0)
            return;                       /* nowhere to put it yet */
        DoMethod(ed->list_used, MUIM_List_InsertSingle, (IPTR)row,
                 MUIV_List_Insert_Bottom);
    } else {
        DoMethod(ed->list_pool, MUIM_List_InsertSingle, (IPTR)row,
                 MUIV_List_Insert_Bottom);
    }
    DoMethod(from, MUIM_List_Remove, active);
}

/* Move the selected dashboard entry one place up or down. */
static void nudge_selected(a2h_editor *ed, int delta)
{
    LONG active = MUIV_List_Active_Off, n = 0, target;

    get(ed->list_used, MUIA_List_Active, &active);
    get(ed->list_used, MUIA_List_Entries, &n);
    if (active == MUIV_List_Active_Off)
        return;

    target = active + delta;
    if (target < 0 || target >= n)
        return;

    DoMethod(ed->list_used, MUIM_List_Move, active, target);
    set(ed->list_used, MUIA_List_Active, target);
}

static void group_new(a2h_editor *ed)
{
    a2h_config *cfg = ed->cfg;
    a2h_group  *g;

    if (cfg->ngroups >= CFG_MAX_GROUPS)
        return;

    apply(ed);   /* keep the edits to the group we are leaving */

    g = &cfg->groups[cfg->ngroups];
    memset(g, 0, sizeof *g);
    sprintf(g->title, "Group %d", cfg->ngroups + 1);
    g->first_widget = cfg->nwidgets;
    g->nwidgets     = 0;
    cfg->ngroups++;

    ed->group = cfg->ngroups - 1;
    refresh_groups(ed);
    refresh_lists(ed);
}

static void group_delete(a2h_editor *ed)
{
    a2h_config *cfg = ed->cfg;
    a2h_group  *g;
    int         i;

    if (cfg->ngroups == 0 || ed->group < 0 || ed->group >= cfg->ngroups)
        return;

    apply(ed);   /* keep the edits to the group we are leaving */

    g = &cfg->groups[ed->group];

    /* Its widgets go back to the pool by simply not being used, but keep
     * their settings so putting one in another group does not reset it. */
    for (i = g->first_widget; i < g->first_widget + g->nwidgets; i++)
        stash_put(ed, &cfg->widgets[i]);

    for (i = g->first_widget + g->nwidgets; i < cfg->nwidgets; i++)
        cfg->widgets[i - g->nwidgets] = cfg->widgets[i];
    cfg->nwidgets -= g->nwidgets;

    for (i = ed->group; i < cfg->ngroups - 1; i++) {
        cfg->groups[i] = cfg->groups[i + 1];
        cfg->groups[i].first_widget -= g->nwidgets;
    }
    cfg->ngroups--;

    for (i = 0; i < cfg->nwidgets; i++)
        if (cfg->widgets[i].group > ed->group)
            cfg->widgets[i].group--;

    if (ed->group >= cfg->ngroups)
        ed->group = cfg->ngroups ? cfg->ngroups - 1 : 0;

    refresh_groups(ed);
    refresh_lists(ed);
}

/* The widget behind the highlighted row, or NULL. */
static a2h_widget *widget_for(a2h_editor *ed, const char *entity)
{
    int i;

    for (i = 0; i < ed->cfg->nwidgets; i++)
        if (strcmp(ed->cfg->widgets[i].entity, entity) == 0)
            return &ed->cfg->widgets[i];
    return NULL;
}

static a2h_widget *selected_widget(a2h_editor *ed, ed_row **rowp, LONG *posp)
{
    LONG    active = MUIV_List_Active_Off;
    ed_row *row    = NULL;

    get(ed->list_used, MUIA_List_Active, &active);
    if (active == MUIV_List_Active_Off)
        return NULL;

    DoMethod(ed->list_used, MUIM_List_GetEntry, active, (IPTR)&row);
    if (!row)
        return NULL;

    if (rowp) *rowp = row;
    if (posp) *posp = active;

    return widget_for(ed, row->entity);
}

/* Where an entity sits in the dashboard list, or -1. */
static int row_pos_of(a2h_editor *ed, const char *entity, ed_row **rowp)
{
    LONG n = 0, i;

    get(ed->list_used, MUIA_List_Entries, &n);
    for (i = 0; i < n; i++) {
        ed_row *row = NULL;

        DoMethod(ed->list_used, MUIM_List_GetEntry, i, (IPTR)&row);
        if (row && strcmp(row->entity, entity) == 0) {
            if (rowp)
                *rowp = row;
            return (int)i;
        }
    }
    return -1;
}

/*
 * Write the properties box back to the entity it is showing.
 *
 * The string gadgets notify on Return and nothing else, so a value typed
 * and then left behind by clicking another entry was simply dropped --
 * and since the box was immediately redrawn for the newly selected
 * entity, it looked as though the value had reverted.
 */
static void commit_properties(a2h_editor *ed)
{
    a2h_widget *w;
    ed_row     *row  = NULL;
    STRPTR      text = NULL;
    int         pos;

    if (!ed->shown[0])
        return;
    w = widget_for(ed, ed->shown);
    if (!w)
        return;

    get(ed->str_label, MUIA_String_Contents, &text);
    if (text) {
        strncpy(w->label, (const char *)text, sizeof w->label - 1);
        w->label[sizeof w->label - 1] = '\0';
    }

    /* Min and max only mean anything for a gauge, and their gadgets are
     * disabled otherwise; leave the other kinds' values alone. */
    if (w->kind == W_GAUGE) {
        LONG lo = 0, hi = 0;

        get(ed->str_min, MUIA_String_Integer, &lo);
        get(ed->str_max, MUIA_String_Integer, &hi);
        if (hi <= lo)      /* an inverted range would divide by zero */
            hi = lo + 1;
        w->min = lo;
        w->max = hi;
        /* Showing the corrected value must not re-trigger the handler. */
        SetAttrs(ed->str_max, MUIA_NoNotify, TRUE,
                 MUIA_String_Integer, (IPTR)hi, TAG_DONE);
    }

    if (w->kind == W_CAMERA) {
        LONG secs = 0, cw = 0, chh = 0;

        get(ed->str_refresh, MUIA_String_Integer, &secs);
        get(ed->str_camw,    MUIA_String_Integer, &cw);
        get(ed->str_camh,    MUIA_String_Integer, &chh);

        /*
         * Clamp rather than reject: this is a text field someone is typing
         * into, and a half-typed "3" should not become an error. The same
         * bounds the parser enforces, so a hand-edited file and this window
         * cannot disagree.
         */
        if (cw  < 16)   cw  = 16;
        if (cw  > 1280) cw  = 1280;
        if (chh < 16)   chh = 16;
        if (chh > 1280) chh = 1280;
        if (secs < 0)     secs = 0;
        if (secs > 86400) secs = 86400;

        {
            LONG on = 0;
            get(ed->cm_stamp, MUIA_Selected, &on);
            w->cam_stamp = on ? 1 : 0;
        }

        w->cam_refresh = (int)secs;
        w->cam_w       = (int)cw;
        w->cam_h       = (int)chh;

        SetAttrs(ed->str_camw, MUIA_NoNotify, TRUE,
                 MUIA_String_Integer, (IPTR)cw, TAG_DONE);
        SetAttrs(ed->str_camh, MUIA_NoNotify, TRUE,
                 MUIA_String_Integer, (IPTR)chh, TAG_DONE);
        SetAttrs(ed->str_refresh, MUIA_NoNotify, TRUE,
                 MUIA_String_Integer, (IPTR)secs, TAG_DONE);
    }

    pos = row_pos_of(ed, ed->shown, &row);
    if (row && pos >= 0) {
        row_fill(row, ed->ha, row->entity, w->label, w->kind);
        DoMethod(ed->list_used, MUIM_List_Redraw, pos);
    }
}

/*
 * Show the highlighted entity's settings in the property gadgets.
 *
 * Every set here uses MUIA_NoNotify. Without it, filling the gadgets fires
 * their own notifications, and the resulting event arrives after the
 * selection has already moved -- stamping the previously shown value onto
 * the newly selected entity. Selecting one entity would silently change
 * another.
 */
static void show_properties(a2h_editor *ed)
{
    ed_row     *row = NULL;
    a2h_widget *w   = selected_widget(ed, &row, NULL);
    int         gauge;

    /* Remember what is on show, so edits can be banked against it later. */
    if (w && row) {
        strncpy(ed->shown, row->entity, sizeof ed->shown - 1);
        ed->shown[sizeof ed->shown - 1] = '\0';
    } else {
        ed->shown[0] = '\0';
    }

    if (!w) {
        SetAttrs(ed->str_label, MUIA_NoNotify, TRUE,
                 MUIA_String_Contents, (IPTR)"",
                 MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(ed->list_kind, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(ed->str_min, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(ed->str_max, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(ed->str_refresh, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(ed->str_camw, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(ed->str_camh, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, TRUE, TAG_DONE);
        SetAttrs(ed->cm_stamp, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, TRUE, TAG_DONE);
        return;
    }

    gauge = (w->kind == W_GAUGE);
    SetAttrs(ed->str_label, MUIA_NoNotify, TRUE,
             MUIA_Disabled, FALSE,
             MUIA_String_Contents, (IPTR)w->label, TAG_DONE);
    SetAttrs(ed->list_kind, MUIA_NoNotify, TRUE,
             MUIA_Disabled, FALSE,
             MUIA_Cycle_Active, (IPTR)w->kind, TAG_DONE);
    /* Min and max only mean anything for a gauge. */
    SetAttrs(ed->str_min, MUIA_NoNotify, TRUE,
             MUIA_Disabled, !gauge,
             MUIA_String_Integer, (IPTR)w->min, TAG_DONE);
    SetAttrs(ed->str_max, MUIA_NoNotify, TRUE,
             MUIA_Disabled, !gauge,
             MUIA_String_Integer, (IPTR)w->max, TAG_DONE);

    /* Likewise the camera fields: shown always, live only for a camera, so
     * it is visible that they exist without pretending they apply. */
    {
        int cam = (w->kind == W_CAMERA);

        SetAttrs(ed->str_refresh, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, !cam,
                 MUIA_String_Integer, (IPTR)w->cam_refresh, TAG_DONE);
        SetAttrs(ed->str_camw, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, !cam,
                 MUIA_String_Integer, (IPTR)w->cam_w, TAG_DONE);
        SetAttrs(ed->str_camh, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, !cam,
                 MUIA_String_Integer, (IPTR)w->cam_h, TAG_DONE);
        SetAttrs(ed->cm_stamp, MUIA_NoNotify, TRUE,
                 MUIA_Disabled, !cam,
                 MUIA_Selected, (IPTR)(cam && w->cam_stamp), TAG_DONE);
    }
}

static void set_kind_of_selected(a2h_editor *ed)
{
    ed_row     *row = NULL;
    LONG        pos = 0, kind = 0;
    a2h_widget *w   = selected_widget(ed, &row, &pos);

    get(ed->list_kind, MUIA_Cycle_Active, &kind);
    if (!w || !row)
        return;

    w->kind = (widget_kind)kind;
    row_fill(row, ed->ha, row->entity, w->label, w->kind);
    DoMethod(ed->list_used, MUIM_List_Redraw, pos);
    show_properties(ed);   /* min/max become live for a gauge */
}



static void rename_group(a2h_editor *ed)
{
    STRPTR text = NULL;

    if (ed->group < 0 || ed->group >= ed->cfg->ngroups)
        return;

    get(ed->str_group, MUIA_String_Contents, &text);
    if (!text || !*text)
        return;

    strncpy(ed->cfg->groups[ed->group].title, (const char *)text,
            sizeof ed->cfg->groups[ed->group].title - 1);
    ed->cfg->groups[ed->group].title[
        sizeof ed->cfg->groups[ed->group].title - 1] = '\0';
    refresh_groups(ed);
}

static int apply(a2h_editor *ed)
{
    LONG col = 0;

    /* The column count belongs to the dashboard rather than to any widget,
     * so it is read here rather than in commit_properties. */
    get(ed->cyc_columns, MUIA_Cycle_Active, &col);
    ed->cfg->columns = (int)col + 1;   /* the cycle shows 1..4 */

    read_back_order(ed);
    return apply_order(ed);
}

int editor_handle(a2h_editor *ed, unsigned long id, int *relayout)
{
    if (!ed || id < ID_ED_BASE || id > ID_ED_LAST)
        return 0;

    if (relayout)
        *relayout = 0;

    /*
     * Whatever is in the properties box belongs to the entity it is
     * showing, so bank it before this event moves the selection, changes
     * the group, or rewrites the configuration. Opening the window is the
     * exception: nothing is on show yet.
     */
    if (id != ID_ED_OPEN)
        commit_properties(ed);

    switch (id) {
    case ID_ED_OPEN:
        editor_show(ed);
        break;

    case ID_ED_ADD:
        move_selected(ed, 1);
        /* Make it a real widget at once, so its properties are editable
         * without having to apply first. */
        apply(ed);
        show_properties(ed);
        break;

    case ID_ED_REMOVE:
        move_selected(ed, 0);
        break;

    case ID_ED_UP:
        nudge_selected(ed, -1);
        break;

    case ID_ED_DOWN:
        nudge_selected(ed, 1);
        break;

    case ID_ED_GROUP_NEW:
        apply(ed);
        group_new(ed);
        break;

    case ID_ED_GROUP_DEL:
        group_delete(ed);
        break;

    case ID_ED_GROUP_SEL: {
        LONG sel = 0;
        apply(ed);                 /* keep the edits to the group we leave */
        get(ed->cyc_group, MUIA_Cycle_Active, &sel);
        ed->group = (int)sel;
        show_group_name(ed);
        refresh_lists(ed);
        break;
    }

    case ID_ED_KIND:
        set_kind_of_selected(ed);
        break;

    case ID_ED_SELECT:
        show_properties(ed);
        break;

    case ID_ED_RENAME:
        rename_group(ed);
        break;

    /* Return in the label, min or max gadget. The commit above already
     * banked it against the entity on show. */
    case ID_ED_LABEL:
    case ID_ED_RANGE:
        break;

    case ID_ED_USE:
        apply(ed);
        set(ed->win, MUIA_Window_Open, FALSE);
        if (relayout)
            *relayout = 1;
        break;

    case ID_ED_SAVE: {
        char err[128];

        apply(ed);
        if (ed->path[0]) {
            a2h_buf out;
            buf_init(&out);
            if (cfg_write(ed->cfg, &out))
                cfg_write_file(ed->path, &out, err, sizeof err);
            buf_free(&out);
        }
        set(ed->win, MUIA_Window_Open, FALSE);
        if (relayout)
            *relayout = 1;
        break;
    }

    case ID_ED_CANCEL:
        /* Put back the copy taken when the window opened. */
        if (ed->backup.widgets) {
            free(ed->cfg->widgets);
            ed->cfg->widgets    = ed->backup.widgets;
            ed->cfg->nwidgets   = ed->backup.nwidgets;
            ed->cfg->widget_cap = ed->backup.widget_cap;
            memcpy(ed->cfg->groups, ed->backup.groups, sizeof ed->cfg->groups);
            ed->cfg->ngroups    = ed->backup.ngroups;
            ed->backup.widgets  = NULL;
        }
        set(ed->win, MUIA_Window_Open, FALSE);
        break;

    default:
        return 0;
    }

    return 1;
}
