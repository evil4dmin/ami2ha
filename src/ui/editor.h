/*
 * ami2ha -- dashboard settings window (internal to src/ui/)
 *
 * Lets the arrangement be edited in the application rather than in a text
 * file: which of the available entities appear, how they are grouped, and
 * in what order. Home Assistant still decides which entities are available
 * -- that is the label -- and this decides how they look.
 */
#ifndef AMI2HA_UI_EDITOR_H
#define AMI2HA_UI_EDITOR_H

#include "ami2ha/config.h"
#include "ami2ha/ha.h"

typedef struct a2h_editor a2h_editor;

/* Return IDs the editor answers to. Kept clear of the dashboard's range. */
#define ID_ED_BASE      2000
#define ID_ED_OPEN      (ID_ED_BASE + 0)
#define ID_ED_ADD       (ID_ED_BASE + 1)
#define ID_ED_REMOVE    (ID_ED_BASE + 2)
#define ID_ED_GROUP_NEW (ID_ED_BASE + 3)
#define ID_ED_GROUP_DEL (ID_ED_BASE + 4)
#define ID_ED_GROUP_SEL (ID_ED_BASE + 5)
#define ID_ED_KIND      (ID_ED_BASE + 6)
#define ID_ED_SAVE      (ID_ED_BASE + 7)
#define ID_ED_USE       (ID_ED_BASE + 8)
#define ID_ED_CANCEL    (ID_ED_BASE + 9)
#define ID_ED_UP        (ID_ED_BASE + 10)
#define ID_ED_DOWN      (ID_ED_BASE + 11)
#define ID_ED_LAST      (ID_ED_BASE + 11)

/*
 * `cfg` is edited in place when the user applies; a copy is kept so Cancel
 * can put it back. `path` is where Save writes.
 */
a2h_editor *editor_create(Object *app, a2h_config *cfg, ha_client *ha,
                          const char *path);
void        editor_dispose(a2h_editor *ed);

/* Bring the window up, rebuilt from the current configuration. */
void editor_show(a2h_editor *ed);

/*
 * Handle one application return ID. Returns 1 if it belonged to the editor.
 * *relayout is set when the dashboard should be rebuilt.
 */
int editor_handle(a2h_editor *ed, unsigned long id, int *relayout);

#endif /* AMI2HA_UI_EDITOR_H */
