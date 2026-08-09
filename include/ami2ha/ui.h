/*
 * ami2ha -- MUI dashboard
 *
 * Amiga-specific. Builds a window from a parsed configuration, keeps the
 * gadgets in step with the entity store, and turns clicks into service
 * calls.
 */
#ifndef AMI2HA_UI_H
#define AMI2HA_UI_H

#include "ami2ha/config.h"
#include "ami2ha/ha.h"
#include "ami2ha/net.h"
#include "ami2ha/rexx.h"

typedef struct a2h_ui a2h_ui;

/*
 * Create the application and window from `cfg`. The client and socket are
 * borrowed, not owned. Returns NULL on failure, with a reason in `err`.
 */
a2h_ui *ui_create(a2h_config *cfg, ha_client *ha, a2h_socket *sock,
                  const char *cfgpath, char *err, size_t errsz);

void ui_dispose(a2h_ui *ui);

/* Serve this ARexx port from the GUI event loop. Borrowed, not owned. */
void ui_set_rexx(a2h_ui *ui, a2h_rexx *rexx);

/*
 * Run the event loop until the user quits or the connection dies. Returns
 * an AmigaDOS RETURN_* code.
 */
int ui_run(a2h_ui *ui);

/* Reflect a changed entity in whichever gadgets are bound to it. */
void ui_entity_changed(a2h_ui *ui, const ha_entity *e);

/* Show a line of status text under the dashboard, and keep it there. */
void ui_set_status(a2h_ui *ui, const char *text);

/*
 * Show a message for three seconds, then fall back to whatever
 * ui_set_status() last set. For acknowledgements -- "Switching on..." is
 * worth seeing when the click happens and is only noise afterwards.
 */
void ui_flash_status(a2h_ui *ui, const char *text);

/* Refresh every widget from the store, e.g. after the initial load. */
void ui_refresh_all(a2h_ui *ui);

/* Rebuild the window's contents after the settings window changed them. */
void ui_rebuild(a2h_ui *ui);

#endif /* AMI2HA_UI_H */
