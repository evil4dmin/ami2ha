/*
 * ami2ha -- entity store
 *
 * Portable C99. Holds the current state of every Home Assistant entity we
 * track, keyed by entity_id.
 *
 * Memory strategy: one allocation per entity, never one per field. AmigaOS
 * has no virtual memory and its allocator is prone to fragmentation, so a
 * store that churned thousands of small strings on every state change would
 * degrade a long-running session. Fields are fixed-size and values that do
 * not fit are truncated -- acceptable for a display client, and it keeps the
 * update path allocation-free after an entity first appears.
 *
 * Attributes are kept in one packed blob per entity rather than a list of
 * nodes, for the same reason. Most entities carry a handful of small
 * attributes, and scanning a few hundred bytes is far cheaper on a 68k than
 * chasing pointers through fragmented memory.
 */
#ifndef AMI2HA_ENTITY_H
#define AMI2HA_ENTITY_H

#include <stddef.h>

#define HA_ENTITY_ID_MAX 64
#define HA_STATE_MAX     40
#define HA_NAME_MAX      48
#define HA_UNIT_MAX      16
#define HA_CLASS_MAX     24

/*
 * Packed attribute blob: "key\0value\0key\0value\0\0". Sized to hold the
 * attributes a dashboard realistically displays (brightness, temperature
 * setpoints, media titles) without trying to mirror everything Home
 * Assistant sends -- a single media_player can carry several KB of entity
 * picture URLs that we would only throw away.
 */
#define HA_ATTRS_MAX 192

typedef struct ha_entity {
    char entity_id[HA_ENTITY_ID_MAX];
    char state[HA_STATE_MAX];
    char name[HA_NAME_MAX];         /* friendly_name                     */
    char unit[HA_UNIT_MAX];         /* unit_of_measurement               */
    char device_class[HA_CLASS_MAX];

    char attrs[HA_ATTRS_MAX];
    size_t attrs_len;               /* bytes used, excluding final terminator */

    unsigned long seq;              /* bumped on every update             */
    int           changed;          /* set on update, cleared by the UI   */

    struct ha_entity *hash_next;    /* bucket chain    */
    struct ha_entity *all_next;     /* iteration order */
} ha_entity;

typedef struct {
    ha_entity   **buckets;
    size_t        nbuckets;
    size_t        count;
    ha_entity    *all;              /* singly-linked list of every entity */
    ha_entity    *all_tail;
    unsigned long seq;              /* global update counter              */
} ha_store;

/* nbuckets is rounded up to a power of two. 0 selects a sensible default. */
int  ha_store_init(ha_store *s, size_t nbuckets);
void ha_store_free(ha_store *s);

/* Look up an entity, or NULL. */
ha_entity *ha_store_get(ha_store *s, const char *entity_id);

/* Look up an entity, creating it if absent. NULL only on allocation failure. */
ha_entity *ha_store_put(ha_store *s, const char *entity_id);

/* Remove one entity. Returns 1 if it existed. */
int ha_store_remove(ha_store *s, const char *entity_id);

/* Drop everything, keeping the bucket array for reuse across reconnects. */
void ha_store_clear(ha_store *s);

size_t ha_store_count(const ha_store *s);

/* Iteration: for (e = ha_store_first(s); e; e = ha_store_next(e)) */
ha_entity *ha_store_first(const ha_store *s);
ha_entity *ha_store_next(const ha_entity *e);

/* The part of "light.kitchen" before the dot. Writes at most dstsz bytes. */
size_t ha_entity_domain(const ha_entity *e, char *dst, size_t dstsz);

/* ---- field setters: truncate rather than fail, and mark the entity ---- */

void ha_entity_set_state(ha_entity *e, const char *state);
void ha_entity_set_name(ha_entity *e, const char *name);
void ha_entity_set_unit(ha_entity *e, const char *unit);
void ha_entity_set_class(ha_entity *e, const char *cls);

/* ---- attributes ---- */

/* Returns the value, or NULL if absent. Points into the entity's blob. */
const char *ha_entity_attr(const ha_entity *e, const char *key);

/*
 * Set or replace an attribute. Returns 1 on success, 0 if the blob is full
 * (the attribute is then simply not stored -- a display client can live
 * without it, and refusing to grow keeps memory bounded).
 */
int ha_entity_set_attr(ha_entity *e, const char *key, const char *value);

/* Drop all attributes, e.g. before applying a fresh state object. */
void ha_entity_clear_attrs(ha_entity *e);

/* Iterate attributes. Pass NULL to start; returns NULL when exhausted.
 * *value is set to the corresponding value. */
const char *ha_entity_attr_next(const ha_entity *e, const char *prev_key,
                                const char **value);

#endif /* AMI2HA_ENTITY_H */
