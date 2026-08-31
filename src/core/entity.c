/* ami2ha -- entity store */
#include "ami2ha/entity.h"

#include <stdlib.h>
#include <string.h>

#define HA_STORE_DEFAULT_BUCKETS 128

/* Copy with truncation, always NUL-terminating. */
static void copy_field(char *dst, size_t dstsz, const char *src)
{
    size_t n;

    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* FNV-1a. Cheap, no multiply-heavy mixing, good enough for entity IDs. */
static unsigned long hash_id(const char *s)
{
    unsigned long h = 2166136261UL;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619UL;
    }
    return h;
}

static size_t round_pow2(size_t n)
{
    size_t p = 1;

    while (p < n)
        p <<= 1;
    return p;
}

int ha_store_init(ha_store *s, size_t nbuckets)
{
    if (nbuckets == 0)
        nbuckets = HA_STORE_DEFAULT_BUCKETS;
    nbuckets = round_pow2(nbuckets);

    s->buckets = (ha_entity **)calloc(nbuckets, sizeof(ha_entity *));
    if (!s->buckets) {
        s->nbuckets = 0;
        s->count    = 0;
        s->all      = NULL;
        s->all_tail = NULL;
        s->seq      = 0;
        return 0;
    }
    s->nbuckets = nbuckets;
    s->count    = 0;
    s->all      = NULL;
    s->all_tail = NULL;
    s->seq      = 0;
    return 1;
}

void ha_store_clear(ha_store *s)
{
    ha_entity *e = s->all;

    while (e) {
        ha_entity *next = e->all_next;
        free(e);
        e = next;
    }
    s->all      = NULL;
    s->all_tail = NULL;
    s->count    = 0;
    if (s->buckets)
        memset(s->buckets, 0, s->nbuckets * sizeof(ha_entity *));
}

void ha_store_free(ha_store *s)
{
    ha_store_clear(s);
    free(s->buckets);
    s->buckets  = NULL;
    s->nbuckets = 0;
}

ha_entity *ha_store_get(ha_store *s, const char *entity_id)
{
    ha_entity *e;

    if (!s->buckets || !entity_id)
        return NULL;

    e = s->buckets[hash_id(entity_id) & (s->nbuckets - 1)];
    while (e) {
        if (strcmp(e->entity_id, entity_id) == 0)
            return e;
        e = e->hash_next;
    }
    return NULL;
}

ha_entity *ha_store_put(ha_store *s, const char *entity_id)
{
    ha_entity *e;
    size_t     b;

    if (!s->buckets || !entity_id || !*entity_id)
        return NULL;

    e = ha_store_get(s, entity_id);
    if (e)
        return e;

    e = (ha_entity *)calloc(1, sizeof *e);
    if (!e)
        return NULL;

    copy_field(e->entity_id, sizeof e->entity_id, entity_id);

    b = hash_id(entity_id) & (s->nbuckets - 1);
    e->hash_next  = s->buckets[b];
    s->buckets[b] = e;

    /* Append rather than prepend: the UI iterates in discovery order, and a
     * dashboard that reshuffled itself on every reconnect would be unusable. */
    if (s->all_tail)
        s->all_tail->all_next = e;
    else
        s->all = e;
    s->all_tail = e;

    s->count++;
    return e;
}

int ha_store_remove(ha_store *s, const char *entity_id)
{
    ha_entity **pp;
    ha_entity  *e;
    size_t      b;

    if (!s->buckets || !entity_id)
        return 0;

    b  = hash_id(entity_id) & (s->nbuckets - 1);
    pp = &s->buckets[b];
    while (*pp && strcmp((*pp)->entity_id, entity_id) != 0)
        pp = &(*pp)->hash_next;
    if (!*pp)
        return 0;

    e   = *pp;
    *pp = e->hash_next;

    /* Unlink from the iteration list too. */
    {
        ha_entity **ap = &s->all;
        while (*ap && *ap != e)
            ap = &(*ap)->all_next;
        if (*ap)
            *ap = e->all_next;
    }

    /* Walk to find the new tail. Removal is rare -- entities disappear only
     * when they are deleted in Home Assistant -- so an O(n) pass here is
     * cheaper than carrying a doubly-linked list through every update. */
    {
        ha_entity *t = s->all;
        while (t && t->all_next)
            t = t->all_next;
        s->all_tail = t;
    }

    free(e);
    s->count--;
    return 1;
}

size_t ha_store_count(const ha_store *s)
{
    return s->count;
}

ha_entity *ha_store_first(const ha_store *s)
{
    return s->all;
}

ha_entity *ha_store_next(const ha_entity *e)
{
    return e ? e->all_next : NULL;
}

size_t ha_entity_domain(const ha_entity *e, char *dst, size_t dstsz)
{
    const char *dot;
    size_t      n;

    if (dstsz == 0)
        return 0;

    dot = strchr(e->entity_id, '.');
    n   = dot ? (size_t)(dot - e->entity_id) : strlen(e->entity_id);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, e->entity_id, n);
    dst[n] = '\0';
    return n;
}

/* ---- field setters ---- */

static void touch(ha_entity *e)
{
    e->seq++;
    e->changed = 1;
}

void ha_entity_set_state(ha_entity *e, const char *state)
{
    if (strcmp(e->state, state ? state : "") == 0)
        return; /* no-op updates must not wake the UI */
    copy_field(e->state, sizeof e->state, state);
    touch(e);
}

void ha_entity_set_name(ha_entity *e, const char *name)
{
    copy_field(e->name, sizeof e->name, name);
}

void ha_entity_set_unit(ha_entity *e, const char *unit)
{
    copy_field(e->unit, sizeof e->unit, unit);
}

void ha_entity_set_class(ha_entity *e, const char *cls)
{
    copy_field(e->device_class, sizeof e->device_class, cls);
}

/* ---- attributes ---- */

void ha_entity_clear_attrs(ha_entity *e)
{
    e->attrs_len = 0;
    e->attrs[0]  = '\0';
}

/* Find the key slot; returns offset of the key, or (size_t)-1. */
static size_t attr_find(const ha_entity *e, const char *key)
{
    size_t off = 0;

    while (off < e->attrs_len) {
        size_t klen = strlen(e->attrs + off);
        size_t voff = off + klen + 1;
        size_t vlen;

        if (voff > e->attrs_len)
            break;
        vlen = strlen(e->attrs + voff);

        if (strcmp(e->attrs + off, key) == 0)
            return off;
        off = voff + vlen + 1;
    }
    return (size_t)-1;
}

const char *ha_entity_attr(const ha_entity *e, const char *key)
{
    size_t off;

    if (!key)
        return NULL;
    off = attr_find(e, key);
    if (off == (size_t)-1)
        return NULL;
    return e->attrs + off + strlen(e->attrs + off) + 1;
}

/* Remove the entry starting at `off`, closing the gap. */
static void attr_erase(ha_entity *e, size_t off)
{
    size_t klen = strlen(e->attrs + off);
    size_t voff = off + klen + 1;
    size_t vlen = strlen(e->attrs + voff);
    size_t end  = voff + vlen + 1;
    size_t tail = e->attrs_len - end;

    memmove(e->attrs + off, e->attrs + end, tail);
    e->attrs_len -= (end - off);
    e->attrs[e->attrs_len] = '\0';
}

int ha_entity_del_attr(ha_entity *e, const char *key)
{
    size_t off;

    if (!key || !*key)
        return 0;
    off = attr_find(e, key);
    if (off == (size_t)-1)
        return 0;
    attr_erase(e, off);
    touch(e);
    return 1;
}

int ha_entity_set_attr(ha_entity *e, const char *key, const char *value)
{
    size_t off, klen, vlen, need;

    if (!key || !*key)
        return 0;
    if (!value)
        value = "";

    off = attr_find(e, key);
    if (off != (size_t)-1) {
        const char *cur = e->attrs + off + strlen(e->attrs + off) + 1;
        if (strcmp(cur, value) == 0)
            return 1; /* unchanged */
        attr_erase(e, off);
    }

    klen = strlen(key);
    vlen = strlen(value);
    need = klen + 1 + vlen + 1;

    /* Room for the entry plus the trailing terminator. */
    if (e->attrs_len + need + 1 > sizeof e->attrs)
        return 0;

    memcpy(e->attrs + e->attrs_len, key, klen + 1);
    memcpy(e->attrs + e->attrs_len + klen + 1, value, vlen + 1);
    e->attrs_len += need;
    e->attrs[e->attrs_len] = '\0';

    touch(e);
    return 1;
}

const char *ha_entity_attr_next(const ha_entity *e, const char *prev_key,
                                const char **value)
{
    size_t off = 0;

    if (prev_key) {
        size_t p = attr_find(e, prev_key);
        if (p == (size_t)-1)
            return NULL;
        off = p + strlen(e->attrs + p) + 1;
        off += strlen(e->attrs + off) + 1;
    }

    if (off >= e->attrs_len)
        return NULL;

    if (value)
        *value = e->attrs + off + strlen(e->attrs + off) + 1;
    return e->attrs + off;
}

/*
 * Attribute values arrive as the JSON text that was in the message, so a
 * brightness is "191" and a colour is "[255, 128, 0]". Reading them is
 * therefore a small parse rather than a cast.
 */
int ha_attr_pct(const ha_entity *e, const char *key, int scale255)
{
    const char *v = ha_entity_attr(e, key);
    long        n;
    char       *end;

    if (!v || !*v)
        return -1;

    n = strtol(v, &end, 10);
    if (end == v)
        return -1;                       /* not a number at all */

    if (scale255) {
        /*
         * 0..255 to 0..100, rounded rather than truncated: 255 has to come
         * out as 100, and half-steps that land on .5 should go up, or a
         * lamp set to 50% reads back as 49 and the slider jumps under the
         * user's hand the moment the server answers.
         */
        if (n < 0) n = 0;
        if (n > 255) n = 255;
        return (int)((n * 100 + 127) / 255);
    }

    if (n < 0) n = 0;
    if (n > 100) n = 100;
    return (int)n;
}

int ha_attr_rgb(const ha_entity *e, const char *key, int *r, int *g, int *b)
{
    const char *v = ha_entity_attr(e, key);
    long        c[3];
    int         i;
    char       *end;

    if (!v)
        return 0;

    while (*v == ' ' || *v == '[')
        v++;

    for (i = 0; i < 3; i++) {
        c[i] = strtol(v, &end, 10);
        if (end == v)
            return 0;                    /* fewer than three numbers */
        if (c[i] < 0)   c[i] = 0;
        if (c[i] > 255) c[i] = 255;
        v = end;
        while (*v == ' ' || *v == ',')
            v++;
    }

    if (r) *r = (int)c[0];
    if (g) *g = (int)c[1];
    if (b) *b = (int)c[2];
    return 1;
}
