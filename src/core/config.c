/* ami2ha -- dashboard configuration parser */
#include "ami2ha/config.h"

#include "ami2ha/charset.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cfg_init(a2h_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->port         = 8123;
    cfg->columns      = 1;
    cfg->refresh_secs = 60;
    cfg->tls_verify   = 1;   /* only ever turned off deliberately */
}

void cfg_free(a2h_config *cfg)
{
    free(cfg->widgets);
    cfg->widgets    = NULL;
    cfg->nwidgets   = 0;
    cfg->widget_cap = 0;
}

/* Make room for one more widget. Returns 0 if the cap or memory is reached. */
static int ensure_widget_room(a2h_config *cfg)
{
    a2h_widget *grown;
    int         want;

    if (cfg->nwidgets < cfg->widget_cap)
        return 1;
    if (cfg->nwidgets >= CFG_MAX_WIDGETS)
        return 0;

    want = cfg->widget_cap + CFG_WIDGET_CHUNK;
    if (want > CFG_MAX_WIDGETS)
        want = CFG_MAX_WIDGETS;

    grown = (a2h_widget *)realloc(cfg->widgets,
                                  (size_t)want * sizeof *grown);
    if (!grown)
        return 0;

    cfg->widgets    = grown;
    cfg->widget_cap = want;
    return 1;
}

const char *cfg_widget_kind_name(widget_kind k)
{
    switch (k) {
    case W_SENSOR: return "sensor";
    case W_TOGGLE: return "toggle";
    case W_BUTTON: return "button";
    case W_GAUGE:  return "gauge";
    case W_TEXT:   return "text";
    case W_CAMERA: return "camera";
    }
    return "?";
}

/* ------------------------------------------------------------------ *
 * Tokenising
 * ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
    int         line;
    char        err[CFG_ERR_MAX];
    int         failed;
} cfg_scan;

static void cfg_fail(cfg_scan *s, const char *what, const char *detail)
{
    if (s->failed)
        return;
    s->failed = 1;

    /* Hand-rolled rather than sprintf: this must stay usable if the C
     * library's formatted output is unavailable, and the shape is fixed. */
    {
        char        num[12];
        int         i = 0, n = s->line;
        size_t      o = 0;
        const char *pre = "line ";

        if (n <= 0) n = 1;
        while (n > 0 && i < (int)sizeof num - 1) {
            num[i++] = (char)('0' + (n % 10));
            n /= 10;
        }

        while (*pre && o + 1 < sizeof s->err)
            s->err[o++] = *pre++;
        while (i > 0 && o + 1 < sizeof s->err)
            s->err[o++] = num[--i];
        if (o + 2 < sizeof s->err) {
            s->err[o++] = ':';
            s->err[o++] = ' ';
        }
        while (*what && o + 1 < sizeof s->err)
            s->err[o++] = *what++;
        if (detail && *detail) {
            const char *sep = " '";
            while (*sep && o + 1 < sizeof s->err)
                s->err[o++] = *sep++;
            while (*detail && o + 2 < sizeof s->err)
                s->err[o++] = *detail++;
            if (o + 1 < sizeof s->err)
                s->err[o++] = '\'';
        }
        s->err[o] = '\0';
    }
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/*
 * Read the next token on the current logical line.
 *   1  token written to `out`
 *   0  end of line (or of input)
 * A backslash at end of line continues onto the next, so long widget
 * definitions can be wrapped.
 */
static int next_token(cfg_scan *s, char *out, size_t outsz)
{
    size_t o = 0;

    for (;;) {
        while (s->p < s->end && is_space(*s->p))
            s->p++;

        /* Line continuation. */
        if (s->p < s->end && *s->p == '\\') {
            const char *q = s->p + 1;
            while (q < s->end && is_space(*q))
                q++;
            if (q < s->end && *q == '\n') {
                s->p = q + 1;
                s->line++;
                continue;
            }
        }
        break;
    }

    if (s->p >= s->end || *s->p == '\n')
        return 0;

    if (*s->p == '#' || *s->p == ';') { /* comment runs to end of line */
        while (s->p < s->end && *s->p != '\n')
            s->p++;
        return 0;
    }

    if (*s->p == '"') {
        s->p++;
        while (s->p < s->end && *s->p != '"' && *s->p != '\n') {
            if (o + 1 < outsz)
                out[o++] = *s->p;
            s->p++;
        }
        if (s->p >= s->end || *s->p != '"') {
            cfg_fail(s, "unterminated quoted string", NULL);
            out[0] = '\0';
            return 0;
        }
        s->p++;
        out[o] = '\0';
        return 1;
    }

    /* Bare token. Braces are tracked so an unquoted JSON blob stays one
     * token: writing data {"brightness":255} should not need quoting. */
    {
        int depth = 0;
        while (s->p < s->end && *s->p != '\n') {
            char c = *s->p;
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') depth--;
            else if (depth == 0 && is_space(c)) break;
            if (o + 1 < outsz)
                out[o++] = c;
            s->p++;
        }
    }
    out[o] = '\0';
    return o > 0;
}

static void skip_rest_of_line(cfg_scan *s)
{
    while (s->p < s->end && *s->p != '\n')
        s->p++;
    if (s->p < s->end) {
        s->p++;
        s->line++;
    }
}

/*
 * Accept the several spellings people reasonably expect, rather than
 * insisting on one and rejecting a file over it.
 */
static int parse_bool(const char *t, int *out)
{
    if (strcmp(t, "yes") == 0 || strcmp(t, "on") == 0 ||
        strcmp(t, "true") == 0 || strcmp(t, "1") == 0) {
        *out = 1;
        return 1;
    }
    if (strcmp(t, "no") == 0 || strcmp(t, "off") == 0 ||
        strcmp(t, "false") == 0 || strcmp(t, "0") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int parse_long(const char *t, long *out)
{
    long v   = 0;
    int  neg = 0;
    int  any = 0;

    if (*t == '-') { neg = 1; t++; }
    else if (*t == '+') t++;

    while (*t >= '0' && *t <= '9') {
        if (v > (LONG_MAX - 9) / 10)
            return 0;
        v = v * 10 + (*t - '0');
        t++;
        any = 1;
    }
    if (!any || *t)
        return 0;
    *out = neg ? -v : v;
    return 1;
}

static void copy_str(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * Copy a string that will be shown on screen. The dashboard file may have
 * been saved as UTF-8 by an editor on a modern machine or as Latin-1 on the
 * Amiga; both must display correctly, so run display text through the
 * transcoder while leaving entity ids and service names alone.
 */
static widget_kind kind_for_domain(const char *domain);

static void copy_text(char *dst, size_t dstsz, const char *src)
{
    charset_utf8_to_latin1(dst, dstsz, src, strlen(src));
}

/* ------------------------------------------------------------------ *
 * Widget lines
 * ------------------------------------------------------------------ */

static int widget_kind_from(const char *t, widget_kind *k)
{
    if (strcmp(t, "sensor") == 0) { *k = W_SENSOR; return 1; }
    if (strcmp(t, "toggle") == 0) { *k = W_TOGGLE; return 1; }
    if (strcmp(t, "button") == 0) { *k = W_BUTTON; return 1; }
    if (strcmp(t, "gauge")  == 0) { *k = W_GAUGE;  return 1; }
    if (strcmp(t, "text")   == 0) { *k = W_TEXT;   return 1; }
    if (strcmp(t, "camera") == 0) { *k = W_CAMERA; return 1; }
    return 0;
}

/*
 * Derive a readable label from an entity id when none was given:
 * "sensor.wohnzimmer_temperatur" -> "Wohnzimmer temperatur".
 * Better than showing a raw id, and it saves typing in the common case.
 */
static void label_from_entity(char *dst, size_t dstsz, const char *entity)
{
    const char *p = strchr(entity, '.');
    size_t      o = 0;

    p = p ? p + 1 : entity;
    while (*p && o + 1 < dstsz) {
        char c = *p++;
        if (c == '_')
            c = ' ';
        dst[o++] = c;
    }
    dst[o] = '\0';
    if (dst[0] >= 'a' && dst[0] <= 'z')
        dst[0] = (char)(dst[0] - 'a' + 'A');
}

static void parse_widget(cfg_scan *s, a2h_config *cfg, widget_kind kind)
{
    a2h_widget *w;
    char        tok[CFG_DATA_MAX];
    int         have_label = 0;

    if (cfg->ngroups == 0) {
        cfg_fail(s, "widget outside any group", NULL);
        return;
    }

    if (!ensure_widget_room(cfg)) {
        cfg_fail(s, "too many widgets", NULL);
        return;
    }

    w = &cfg->widgets[cfg->nwidgets];
    memset(w, 0, sizeof *w);
    w->kind     = kind;
    w->group    = cfg->ngroups - 1;
    w->min      = 0;
    w->max      = 100;
    w->decimals = 1;

    /* First bare argument: the entity (or, for a button, the service). */
    if (next_token(s, tok, sizeof tok)) {
        if (kind == W_BUTTON)
            copy_str(w->service, sizeof w->service, tok);
        else if (kind == W_TEXT)
            copy_text(w->label, sizeof w->label, tok), have_label = 1,
            w->label_explicit = 1;
        else
            copy_str(w->entity, sizeof w->entity, tok);
    } else if (kind != W_TEXT) {
        cfg_fail(s, "missing entity for", cfg_widget_kind_name(kind));
        return;
    }

    while (next_token(s, tok, sizeof tok)) {
        char val[CFG_DATA_MAX];

        if (strcmp(tok, "label") == 0) {
            if (!next_token(s, val, sizeof val)) {
                cfg_fail(s, "label needs a value", NULL);
                return;
            }
            copy_text(w->label, sizeof w->label, val);
            w->label_explicit = 1;
            have_label = 1;
        } else if (strcmp(tok, "entity") == 0) {
            if (!next_token(s, val, sizeof val)) {
                cfg_fail(s, "entity needs a value", NULL);
                return;
            }
            copy_str(w->entity, sizeof w->entity, val);
        } else if (strcmp(tok, "data") == 0) {
            if (!next_token(s, val, sizeof val)) {
                cfg_fail(s, "data needs a value", NULL);
                return;
            }
            copy_str(w->data, sizeof w->data, val);
        } else if (strcmp(tok, "min") == 0 || strcmp(tok, "max") == 0) {
            long v;
            int  is_min = (tok[1] == 'i');
            if (!next_token(s, val, sizeof val) || !parse_long(val, &v)) {
                cfg_fail(s, "expected a number after", is_min ? "min" : "max");
                return;
            }
            if (is_min) w->min = v; else w->max = v;
        } else if (strcmp(tok, "width")  == 0 ||
                   strcmp(tok, "height") == 0) {
            long v;
            int  is_w = (tok[0] == 'w');
            if (!next_token(s, val, sizeof val) || !parse_long(val, &v) ||
                v < 16 || v > 1280) {
                cfg_fail(s, "expected 16 to 1280 after", is_w ? "width" : "height");
                return;
            }
            if (is_w) w->cam_w = (int)v; else w->cam_h = (int)v;
        } else if (strcmp(tok, "refresh") == 0) {
            long v;
            if (!next_token(s, val, sizeof val) || !parse_long(val, &v) ||
                v < 0 || v > 86400) {
                cfg_fail(s, "refresh must be 0 (manual) to 86400 seconds", NULL);
                return;
            }
            w->cam_refresh = (int)v;
        } else if (strcmp(tok, "decimals") == 0) {
            long v;
            if (!next_token(s, val, sizeof val) || !parse_long(val, &v) ||
                v < 0 || v > 4) {
                cfg_fail(s, "decimals must be 0 to 4", NULL);
                return;
            }
            w->decimals = (int)v;
        } else {
            cfg_fail(s, "unknown widget option", tok);
            return;
        }
    }

    if (kind == W_CAMERA) {
        /* Without a size Home Assistant sends the camera's native frame,
         * which is 1280x720 on most of them -- five times the bytes and far
         * more decoding than a dashboard tile needs. */
        if (w->cam_w <= 0) w->cam_w = 320;
        if (w->cam_h <= 0) w->cam_h = 180;
    }

    if (kind == W_GAUGE && w->max <= w->min) {
        cfg_fail(s, "gauge max must be greater than min", w->entity);
        return;
    }

    if (!have_label && w->entity[0])
        label_from_entity(w->label, sizeof w->label, w->entity);
    else if (!have_label && w->service[0])
        label_from_entity(w->label, sizeof w->label, w->service);

    cfg->groups[w->group].nwidgets++;
    cfg->nwidgets++;
}

/* ------------------------------------------------------------------ *
 * Top level
 * ------------------------------------------------------------------ */

int cfg_parse(a2h_config *cfg, const char *text, size_t len,
              char *err, size_t errsz)
{
    cfg_scan s;
    char     tok[CFG_DATA_MAX];
    int      in_group = 0;

    cfg_init(cfg);

    s.p      = text;
    s.end    = text + len;
    s.line   = 1;
    s.failed = 0;
    s.err[0] = '\0';

    while (s.p < s.end && !s.failed) {
        if (!next_token(&s, tok, sizeof tok)) {
            if (s.failed)
                break;
            skip_rest_of_line(&s);
            continue;
        }

        if (strcmp(tok, "group") == 0) {
            a2h_group *g;
            if (in_group) {
                cfg_fail(&s, "group inside a group", NULL);
                break;
            }
            if (cfg->ngroups >= CFG_MAX_GROUPS) {
                cfg_fail(&s, "too many groups", NULL);
                break;
            }
            g = &cfg->groups[cfg->ngroups];
            memset(g, 0, sizeof *g);
            g->first_widget = cfg->nwidgets;
            if (next_token(&s, tok, sizeof tok))
                copy_text(g->title, sizeof g->title, tok);
            cfg->ngroups++;
            in_group = 1;

        } else if (strcmp(tok, "end") == 0) {
            if (!in_group) {
                cfg_fail(&s, "'end' without 'group'", NULL);
                break;
            }
            in_group = 0;

        } else {
            widget_kind k;

            if (widget_kind_from(tok, &k)) {
                parse_widget(&s, cfg, k);
            } else if (strcmp(tok, "host") == 0) {
                if (next_token(&s, tok, sizeof tok))
                    copy_str(cfg->host, sizeof cfg->host, tok);
            } else if (strcmp(tok, "tokenfile") == 0) {
                if (next_token(&s, tok, sizeof tok))
                    copy_str(cfg->tokenfile, sizeof cfg->tokenfile, tok);
            } else if (strcmp(tok, "label") == 0) {
                if (next_token(&s, tok, sizeof tok))
                    copy_str(cfg->label, sizeof cfg->label, tok);
            } else if (strcmp(tok, "token") == 0) {
                if (next_token(&s, tok, sizeof tok))
                    copy_str(cfg->token, sizeof cfg->token, tok);
            } else if (strcmp(tok, "tls") == 0 ||
                       strcmp(tok, "tlsverify") == 0) {
                char key[16];
                int  on;
                copy_str(key, sizeof key, tok);
                if (!next_token(&s, tok, sizeof tok) || !parse_bool(tok, &on)) {
                    cfg_fail(&s, "expected yes or no after", key);
                    break;
                }
                if (strcmp(key, "tls") == 0) cfg->tls = on;
                else                         cfg->tls_verify = on;
            } else if (strcmp(tok, "port") == 0 ||
                       strcmp(tok, "columns") == 0 ||
                       strcmp(tok, "refresh") == 0) {
                char key[16];
                long v;
                copy_str(key, sizeof key, tok);
                if (!next_token(&s, tok, sizeof tok) || !parse_long(tok, &v)) {
                    cfg_fail(&s, "expected a number after", key);
                    break;
                }
                if (strcmp(key, "port") == 0) {
                    cfg->port          = (int)v;
                    cfg->port_explicit = 1;
                }
                else if (strcmp(key, "columns") == 0) cfg->columns = (int)v;
                else                                  cfg->refresh_secs = (int)v;
            } else {
                cfg_fail(&s, "unknown keyword", tok);
                break;
            }
        }

        if (!s.failed)
            skip_rest_of_line(&s);
    }

    if (!s.failed && in_group)
        cfg_fail(&s, "missing 'end' for last group", NULL);

    if (s.failed) {
        if (err && errsz)
            copy_str(err, errsz, s.err);
        return 0;
    }

    if (cfg->columns < 1) cfg->columns = 1;
    if (cfg->columns > 8) cfg->columns = 8;

    if (err && errsz)
        err[0] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ *
 * Starter file generation
 * ------------------------------------------------------------------ */

static widget_kind kind_for_domain(const char *domain)
{
    if (strcmp(domain, "light")         == 0 ||
        strcmp(domain, "switch")        == 0 ||
        strcmp(domain, "fan")           == 0 ||
        strcmp(domain, "siren")         == 0 ||
        strcmp(domain, "input_boolean") == 0 ||
        strcmp(domain, "automation")    == 0)
        return W_TOGGLE;

    if (strcmp(domain, "camera") == 0)
        return W_CAMERA;

    if (strcmp(domain, "scene")        == 0 ||
        strcmp(domain, "script")       == 0 ||
        strcmp(domain, "button")       == 0 ||
        strcmp(domain, "input_button") == 0)
        return W_BUTTON;

    return W_SENSOR;
}

/* Quote a label only when it needs it, to keep the file readable. */
static void append_label(a2h_buf *out, const char *label)
{
    buf_append_str(out, " label \"");
    buf_append_str(out, label);
    buf_append_str(out, "\"");
}

int cfg_add_discovered(a2h_config *cfg, const char *entity_id,
                       const char *label)
{
    a2h_widget *w;
    char        dom[24];
    const char *dot;
    size_t      n;

    if (!entity_id || !*entity_id)
        return 0;

    /* A discovered dashboard has one implicit group. */
    if (cfg->ngroups == 0) {
        memset(&cfg->groups[0], 0, sizeof cfg->groups[0]);
        copy_str(cfg->groups[0].title, sizeof cfg->groups[0].title,
                 label && *label ? label : "Home Assistant");
        cfg->groups[0].first_widget = 0;
        cfg->ngroups = 1;
    }

    if (!ensure_widget_room(cfg))
        return 0;

    dot = strchr(entity_id, '.');
    n   = dot ? (size_t)(dot - entity_id) : strlen(entity_id);
    if (n >= sizeof dom)
        n = sizeof dom - 1;
    memcpy(dom, entity_id, n);
    dom[n] = '\0';

    w = &cfg->widgets[cfg->nwidgets];
    memset(w, 0, sizeof *w);
    w->kind     = kind_for_domain(dom);
    w->group    = 0;
    w->min      = 0;
    w->max      = 100;
    w->decimals = 1;
    copy_str(w->entity, sizeof w->entity, entity_id);

    if (w->kind == W_BUTTON) {
        /* scene/script/button are triggered by a service, not toggled. */
        if (strcmp(dom, "button") == 0 || strcmp(dom, "input_button") == 0)
            sprintf(w->service, "%.20s.press", dom);
        else
            sprintf(w->service, "%.20s.turn_on", dom);
    }

    if (w->kind == W_CAMERA) {
        /* Same defaults a hand-written camera line gets. Updating only when
         * asked is the right default for a discovered camera too: labelling
         * four of them should not put the machine to work every minute. */
        w->cam_w       = 320;
        w->cam_h       = 180;
        w->cam_refresh = 0;
    }

    label_from_entity(w->label, sizeof w->label, entity_id);

    cfg->groups[0].nwidgets++;
    cfg->nwidgets++;
    return 1;
}

int cfg_generate(a2h_buf *out, const ha_store *store, const a2h_config *base)
{
    const ha_entity *e;
    char             domains[CFG_MAX_GROUPS][24];
    int              ndomains = 0;
    int              i;
    int              truncated = 0;

    buf_append_str(out, "# ami2ha dashboard\n"
                        "#\n"
                        "# Generated from a live Home Assistant instance. Delete what you do\n"
                        "# not want, reorder freely, and rename the groups and labels.\n"
                        "#\n"
                        "# Widget kinds:\n"
                        "#   sensor <entity>            read-only value\n"
                        "#   toggle <entity>            checkbox for an on/off entity\n"
                        "#   gauge  <entity> min N max N  value drawn as a bar\n"
                        "#   button <domain.service> entity <entity> [data {...}]\n"
                        "#   text   \"caption\"           static label\n"
                        "#\n"
                        "# To turn a reading into a bar, swap 'sensor' for 'gauge' and give it\n"
                        "# a range, e.g.  gauge sensor.kitchen_co2 min 400 max 2000\n"
                        "\n");

    if (base && base->host[0]) {
        buf_printf(out, "host       %s\n", base->host);
        buf_printf(out, "port       %d\n", base->port);
        if (base->tls)
            buf_printf(out, "tls        yes\n");
        if (base->tls && !base->tls_verify)
            buf_printf(out, "tlsverify  no\n");
    }
    if (base && base->tokenfile[0])
        buf_printf(out, "tokenfile  %s\n", base->tokenfile);
    buf_printf(out, "columns    %d\n\n", base && base->columns > 0 ? base->columns : 2);

    /* Collect domains in discovery order so the file mirrors the order Home
     * Assistant reported, rather than an arbitrary hash order. */
    for (e = ha_store_first(store); e; e = ha_store_next(e)) {
        char dom[24];
        int  seen = 0;

        ha_entity_domain(e, dom, sizeof dom);
        for (i = 0; i < ndomains; i++)
            if (strcmp(domains[i], dom) == 0) { seen = 1; break; }
        if (seen)
            continue;
        if (ndomains >= CFG_MAX_GROUPS) { truncated = 1; break; }
        copy_str(domains[ndomains], sizeof domains[0], dom);
        ndomains++;
    }

    for (i = 0; i < ndomains; i++) {
        int emitted = 0;

        buf_printf(out, "group \"%s\"\n", domains[i]);

        for (e = ha_store_first(store); e; e = ha_store_next(e)) {
            char        dom[24];
            widget_kind k;

            ha_entity_domain(e, dom, sizeof dom);
            if (strcmp(dom, domains[i]) != 0)
                continue;

            k = kind_for_domain(dom);
            switch (k) {
            case W_TOGGLE:
                buf_printf(out, "    toggle %s", e->entity_id);
                break;
            case W_BUTTON:
                /* scene/script/button are all triggered by turn_on except
                 * the button domain, which has its own press service. */
                if (strcmp(dom, "button") == 0 || strcmp(dom, "input_button") == 0)
                    buf_printf(out, "    button %s.press entity %s", dom, e->entity_id);
                else
                    buf_printf(out, "    button %s.turn_on entity %s", dom, e->entity_id);
                break;
            default:
                buf_printf(out, "    sensor %s", e->entity_id);
                break;
            }

            if (e->name[0])
                append_label(out, e->name);
            buf_append_str(out, "\n");
            emitted++;
        }

        if (!emitted)
            buf_append_str(out, "    text \"(nothing here)\"\n");
        buf_append_str(out, "end\n\n");
    }

    if (truncated)
        buf_printf(out, "# Note: only the first %d domains were written.\n",
                   CFG_MAX_GROUPS);

    return !out->failed;
}

/* ------------------------------------------------------------------ *
 * Serialisation
 * ------------------------------------------------------------------ */

/*
 * Write a value as a quoted string. The parser has no escape syntax inside
 * quotes, so an embedded double quote would produce a file it could not
 * read back; those become single quotes rather than corrupting the file.
 */
static void write_quoted(a2h_buf *out, const char *s)
{
    buf_append_byte(out, '"');
    for (; *s; s++)
        buf_append_byte(out, (unsigned char)(*s == '"' ? '\'' : *s));
    buf_append_byte(out, '"');
}

int cfg_write(const a2h_config *cfg, a2h_buf *out)
{
    int g, i;

    buf_append_str(out,
        "# ami2ha dashboard\n"
        "#\n"
        "# Written by ami2ha. Editing by hand is fine, but saving from the\n"
        "# settings window regenerates this file and will not keep comments.\n"
        "\n");

    if (cfg->host[0])
        buf_printf(out, "host       %s\n", cfg->host);
    if (cfg->port && cfg->port != 8123)
        buf_printf(out, "port       %d\n", cfg->port);
    if (cfg->tls)
        buf_printf(out, "tls        yes\n");
    if (cfg->tls && !cfg->tls_verify)
        buf_printf(out, "tlsverify  no\n");
    if (cfg->tokenfile[0])
        buf_printf(out, "tokenfile  %s\n", cfg->tokenfile);
    /* Preserved only if it was already in the file; TOKENFILE is preferred. */
    if (cfg->token[0])
        buf_printf(out, "token      %s\n", cfg->token);
    if (cfg->label[0])
        buf_printf(out, "label      %s\n", cfg->label);
    if (cfg->columns != 1)
        buf_printf(out, "columns    %d\n", cfg->columns);
    if (cfg->refresh_secs != 60)
        buf_printf(out, "refresh    %d\n", cfg->refresh_secs);

    for (g = 0; g < cfg->ngroups; g++) {
        const a2h_group *grp = &cfg->groups[g];

        buf_append_str(out, "\ngroup ");
        write_quoted(out, grp->title);
        buf_append_byte(out, '\n');

        for (i = grp->first_widget;
             i < grp->first_widget + grp->nwidgets && i < cfg->nwidgets; i++) {
            const a2h_widget *w = &cfg->widgets[i];

            buf_printf(out, "    %-6s ", cfg_widget_kind_name(w->kind));

            if (w->kind == W_TEXT) {
                write_quoted(out, w->label);
                buf_append_byte(out, '\n');
                continue;
            }

            /* A button leads with its service; everything else the entity. */
            buf_append_str(out, w->kind == W_BUTTON ? w->service : w->entity);

            if (w->kind == W_BUTTON && w->entity[0]) {
                buf_append_str(out, " entity ");
                buf_append_str(out, w->entity);
            }

            buf_append_str(out, " label ");
            write_quoted(out, w->label);

            if (w->kind == W_GAUGE)
                buf_printf(out, " min %ld max %ld", w->min, w->max);
            if (w->kind == W_CAMERA) {
                buf_printf(out, " width %d height %d", w->cam_w, w->cam_h);
                if (w->cam_refresh > 0)
                    buf_printf(out, " refresh %d", w->cam_refresh);
            }
            /* decimals is meaningless on a picture, and writing it would
             * only invite someone to set it. */
            if (w->decimals != 1 && w->kind != W_CAMERA)
                buf_printf(out, " decimals %d", w->decimals);
            if (w->kind == W_BUTTON && w->data[0]) {
                buf_append_str(out, " data ");
                buf_append_str(out, w->data);
            }

            buf_append_byte(out, '\n');
        }

        buf_append_str(out, "end\n");
    }

    return !out->failed;
}
