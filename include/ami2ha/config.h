/*
 * ami2ha -- dashboard configuration
 *
 * Portable C99: the parser has no OS dependencies, so it is covered by the
 * host test suite. src/config/ only supplies the file I/O.
 *
 * The format is deliberately a plain text file rather than an IFF prefs
 * chunk. It is meant to be readable, diffable and shareable -- someone
 * should be able to post their dashboard on a forum and have it work.
 *
 *   # comments start with # or ;
 *
 *   host       homeassistant.local
 *   port       8123
 *   tokenfile  S:ha.token
 *   columns    2
 *
 *   group "Wohnzimmer"
 *       sensor sensor.wohnzimmer_temperatur label "Temperatur"
 *       gauge  sensor.wohnzimmer_co2        label "CO2" min 400 max 2000
 *       toggle light.wohnzimmer             label "Licht"
 *   end
 *
 *   group "Szenen"
 *       button scene.gute_nacht  label "Gute Nacht"
 *       button light.turn_on     label "Hell"  entity light.kueche \
 *              data {"brightness":255}
 *   end
 */
#ifndef AMI2HA_CONFIG_H
#define AMI2HA_CONFIG_H

#include <stddef.h>

#include "ami2ha/buf.h"
#include "ami2ha/entity.h"

#define CFG_LABEL_MAX    40
#define CFG_TITLE_MAX    40
#define CFG_SERVICE_MAX  48
#define CFG_DATA_MAX     128
#define CFG_HOST_MAX     96
#define CFG_PATH_MAX     128
#define CFG_ERR_MAX      120
#define CFG_TOKEN_MAX    256

/* Bounds exist to keep a mistyped or hostile file from exhausting memory. */
#define CFG_MAX_GROUPS   32
#define CFG_MAX_WIDGETS  128

/* Widgets are allocated in blocks of this many as the file is read. */
#define CFG_WIDGET_CHUNK 16

typedef enum {
    W_SENSOR = 0, /* read-only value with optional unit    */
    W_TOGGLE,     /* checkbox bound to an on/off entity    */
    W_BUTTON,     /* fires a service call                  */
    W_GAUGE,      /* value drawn as a bar between min/max  */
    W_TEXT        /* static caption, no entity             */
} widget_kind;

typedef struct {
    widget_kind kind;
    char        entity[HA_ENTITY_ID_MAX];
    char        label[CFG_LABEL_MAX];
    char        service[CFG_SERVICE_MAX]; /* button: "domain.service" */
    char        data[CFG_DATA_MAX];       /* button: raw JSON service data */
    long        min, max;                 /* gauge range   */
    int         decimals;                 /* gauge scaling */
    int         group;                    /* owning group index */
} a2h_widget;

typedef struct {
    char title[CFG_TITLE_MAX];
    int  first_widget; /* index into cfg->widgets */
    int  nwidgets;
} a2h_group;

typedef struct {
    char host[CFG_HOST_MAX];
    int  port;
    char tokenfile[CFG_PATH_MAX];
    char token[CFG_TOKEN_MAX];
    /*
     * When set, the entities come from Home Assistant: everything tagged
     * with this label, rather than the widgets listed below. Lets the
     * selection be made in the HA UI instead of in this file.
     */
    char label[48];

    int  columns;      /* dashboard columns; 0 = let MUI decide */
    int  refresh_secs; /* application-level ping interval       */

    a2h_group   groups[CFG_MAX_GROUPS];
    int         ngroups;

    /*
     * Grown on demand rather than embedded. Embedding CFG_MAX_WIDGETS
     * widgets made a2h_config roughly 80 KB, which is both absurd on a 2 MB
     * machine for a dashboard holding a dozen controls, and large enough to
     * land in territory where the allocation misbehaved on real hardware.
     * A typical dashboard now costs a few KB.
     */
    a2h_widget *widgets;
    int         nwidgets;
    int         widget_cap;
} a2h_config;

/* Populate with defaults (port 8123, one column, no widgets). */
void cfg_init(a2h_config *cfg);

/* Release the widget array. Safe on a zeroed or already-freed config. */
void cfg_free(a2h_config *cfg);

/*
 * Parse `text`. Returns 1 on success. On failure returns 0 and writes a
 * message naming the offending line into `err`.
 */
int cfg_parse(a2h_config *cfg, const char *text, size_t len,
              char *err, size_t errsz);

/*
 * Append a widget, inferring its kind from the entity's domain. Used when
 * the dashboard is discovered from Home Assistant rather than written out.
 * Returns 0 if the cap or memory is reached.
 */
int cfg_add_discovered(a2h_config *cfg, const char *entity_id,
                       const char *label);

/* Human-readable name of a widget kind, for error messages and the editor. */
const char *cfg_widget_kind_name(widget_kind k);

/*
 * Write a starter configuration covering everything in `store`, grouped by
 * domain, into `out`. Explicit per-entity config is only tolerable if you
 * do not have to type two hundred entity IDs by hand: this produces a file
 * to prune rather than a blank page.
 *
 * Widget kinds are inferred from the domain (lights and switches become
 * toggles, scenes and scripts become buttons, everything else a readout).
 * Gauges are never guessed -- their ranges are a judgement call, so the
 * generated file explains how to convert a sensor into one instead.
 */
int cfg_generate(a2h_buf *out, const ha_store *store, const a2h_config *base);

#endif /* AMI2HA_CONFIG_H */
