/* ami2ha -- JSON reader tests */
#include "tinytest.h"
#include "ami2ha/json.h"

#include <limits.h>
#include <stdio.h>

/* Walk to the value of a top-level key. Returns the token type. */
static json_type seek(json_parser *jp, const char *doc, const char *key,
                      json_token *tok)
{
    json_init(jp, doc, strlen(doc));
    json_next(jp, tok); /* OBJECT_BEGIN */
    while (json_next(jp, tok) == JSON_KEY) {
        int hit = json_key_is(tok, key);
        json_type t = json_next(jp, tok);
        if (hit)
            return t;
        json_skip(jp, tok);
    }
    return JSON_END;
}

static void test_flat_object(void)
{
    const char *doc = "{\"id\":42,\"type\":\"result\",\"success\":true,"
                      "\"nothing\":null,\"neg\":-3.5}";
    json_parser jp;
    json_token  tok;
    char        s[64];
    long        n;

    CHECK_INT(seek(&jp, doc, "id", &tok), JSON_NUMBER);
    CHECK(json_int(&tok, &n));
    CHECK_INT(n, 42);

    CHECK_INT(seek(&jp, doc, "type", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_STR(s, "result");

    CHECK_INT(seek(&jp, doc, "success", &tok), JSON_TRUE);
    CHECK_INT(seek(&jp, doc, "nothing", &tok), JSON_NULL);

    CHECK_INT(seek(&jp, doc, "neg", &tok), JSON_NUMBER);
    CHECK(json_fixed(&tok, &n, 1));
    CHECK_INT(n, -35);
}

static void test_nesting_and_skip(void)
{
    /* The value of "skipped" must be stepped over entirely, including its
     * nested containers, so that "after" is still reachable. */
    const char *doc = "{\"skipped\":{\"a\":[1,2,{\"b\":[3]}],\"c\":{}},"
                      "\"after\":\"ok\"}";
    json_parser jp;
    json_token  tok;
    char        s[16];

    CHECK_INT(seek(&jp, doc, "after", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_STR(s, "ok");
}

static void test_array_walk(void)
{
    const char *doc = "[10,20,30]";
    json_parser jp;
    json_token  tok;
    long        sum = 0, v;
    int         n   = 0;

    json_init(&jp, doc, strlen(doc));
    CHECK_INT(json_next(&jp, &tok), JSON_ARRAY_BEGIN);
    while (json_next(&jp, &tok) == JSON_NUMBER) {
        CHECK(json_int(&tok, &v));
        sum += v;
        n++;
    }
    CHECK_INT(n, 3);
    CHECK_INT(sum, 60);
    CHECK_INT(tok.type, JSON_ARRAY_END);
    CHECK_INT(json_next(&jp, &tok), JSON_END);
}

static void test_empty_containers(void)
{
    json_parser jp;
    json_token  tok;

    json_init(&jp, "{}", 2);
    CHECK_INT(json_next(&jp, &tok), JSON_OBJECT_BEGIN);
    CHECK_INT(json_next(&jp, &tok), JSON_OBJECT_END);
    CHECK_INT(json_next(&jp, &tok), JSON_END);

    json_init(&jp, "[]", 2);
    CHECK_INT(json_next(&jp, &tok), JSON_ARRAY_BEGIN);
    CHECK_INT(json_next(&jp, &tok), JSON_ARRAY_END);
    CHECK_INT(json_next(&jp, &tok), JSON_END);

    json_init(&jp, "[[],{},[{}]]", 12);
    CHECK_INT(json_next(&jp, &tok), JSON_ARRAY_BEGIN);
    CHECK(json_skip(&jp, &tok));
    CHECK_INT(json_next(&jp, &tok), JSON_END);
}

static void test_escapes(void)
{
    const char *doc =
        "{\"esc\":\"a\\nb\\tc\\\"d\\\\e\\/f\",\"uni\":\"\\u00e9\\u00fc\"}";
    json_parser jp;
    json_token  tok;
    char        s[64];

    CHECK_INT(seek(&jp, doc, "esc", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_STR(s, "a\nb\tc\"d\\e/f");

    /* é and ü are inside Latin-1, so they map straight through. */
    CHECK_INT(seek(&jp, doc, "uni", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_INT((unsigned char)s[0], 0xE9);
    CHECK_INT((unsigned char)s[1], 0xFC);
    CHECK_INT(s[2], 0);
}

static void test_utf8_to_latin1(void)
{
    /* Real payloads: a degree sign in a unit, an umlaut in a friendly name. */
    const char *doc = "{\"unit\":\"\xC2\xB0" "C\",\"name\":\"K\xC3\xBC" "che\"}";
    json_parser jp;
    json_token  tok;
    char        s[64];

    CHECK_INT(seek(&jp, doc, "unit", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_INT((unsigned char)s[0], 0xB0); /* degree sign in Latin-1 */
    CHECK_INT(s[1], 'C');

    CHECK_INT(seek(&jp, doc, "name", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_INT((unsigned char)s[1], 0xFC); /* u-umlaut */
    CHECK_STR(s + 2, "che");
}

static void test_charset_folds(void)
{
    /* Codepoints with no Latin-1 equivalent get an ASCII substitute so the
     * Amiga renders something meaningful instead of garbage. */
    const char *doc = "{\"a\":\"\xE2\x80\x99\",\"b\":\"\xE2\x82\xAC\","
                      "\"c\":\"\xE2\x80\xA6\",\"d\":\"\xF0\x9F\x98\x80\"}";
    json_parser jp;
    json_token  tok;
    char        s[32];

    CHECK_INT(seek(&jp, doc, "a", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_STR(s, "'"); /* right single quote */

    CHECK_INT(seek(&jp, doc, "b", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_STR(s, "EUR");

    CHECK_INT(seek(&jp, doc, "c", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_STR(s, "...");

    CHECK_INT(seek(&jp, doc, "d", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_STR(s, "?"); /* emoji: nothing sensible to fall back to */
}

static void test_surrogate_pair(void)
{
    /* 😀 is one astral codepoint, not two replacement chars. */
    const char *doc = "{\"e\":\"\\uD83D\\uDE00!\"}";
    json_parser jp;
    json_token  tok;
    char        s[32];

    CHECK_INT(seek(&jp, doc, "e", &tok), JSON_STRING);
    json_str_copy(&tok, s, sizeof s);
    CHECK_STR(s, "?!");
}

static void test_truncation_is_safe(void)
{
    const char *doc = "{\"k\":\"abcdefghij\"}";
    json_parser jp;
    json_token  tok;
    char        s[5];

    CHECK_INT(seek(&jp, doc, "k", &tok), JSON_STRING);
    CHECK_INT(json_str_copy(&tok, s, sizeof s), 4);
    CHECK_STR(s, "abcd"); /* always NUL-terminated, never overruns */
}

static void test_errors(void)
{
    json_parser jp;
    json_token  tok;
    int         guard;

    /* Unterminated string. */
    json_init(&jp, "{\"a\":\"xx", 8);
    json_next(&jp, &tok);
    json_next(&jp, &tok);
    CHECK_INT(json_next(&jp, &tok), JSON_ERROR);
    CHECK(jp.err != NULL);

    /* Bad literal. */
    json_init(&jp, "{\"a\":tru}", 9);
    json_next(&jp, &tok);
    json_next(&jp, &tok);
    CHECK_INT(json_next(&jp, &tok), JSON_ERROR);

    /* Missing colon. */
    json_init(&jp, "{\"a\" 1}", 7);
    json_next(&jp, &tok);
    CHECK_INT(json_next(&jp, &tok), JSON_ERROR);

    /* Once failed, the parser must stay failed rather than loop forever. */
    for (guard = 0; guard < 5; guard++)
        CHECK_INT(json_next(&jp, &tok), JSON_ERROR);

    /* Overflowing the depth limit must fail cleanly, not smash the stack. */
    {
        char deep[2 * JSON_MAX_DEPTH + 8];
        int  i;
        for (i = 0; i < (int)sizeof deep - 1; i++)
            deep[i] = '[';
        deep[sizeof deep - 1] = '\0';
        json_init(&jp, deep, strlen(deep));
        while (json_next(&jp, &tok) == JSON_ARRAY_BEGIN)
            ;
        CHECK_INT(tok.type, JSON_ERROR);
    }
}

static void test_ha_state_payload(void)
{
    /* Shaped like a real state_changed event from the WebSocket API. */
    static const char *doc =
        "{\"id\":7,\"type\":\"event\",\"event\":{\"event_type\":\"state_changed\","
        "\"data\":{\"entity_id\":\"sensor.wohnzimmer_temperatur\","
        "\"new_state\":{\"entity_id\":\"sensor.wohnzimmer_temperatur\","
        "\"state\":\"21.4\",\"attributes\":{\"unit_of_measurement\":\"\xC2\xB0" "C\","
        "\"friendly_name\":\"Wohnzimmer Temperatur\",\"device_class\":\"temperature\"},"
        "\"last_changed\":\"2026-08-07T12:00:00.000000+00:00\"},"
        "\"old_state\":{\"state\":\"21.3\"}}}}";

    json_parser jp;
    json_token  tok;
    char        entity[64] = "", state[32] = "", unit[16] = "", fname[64] = "";
    int         depth_guard = 0;

    json_init(&jp, doc, strlen(doc));
    CHECK_INT(json_next(&jp, &tok), JSON_OBJECT_BEGIN);

    /* Descend id/type/event -> data -> new_state, copying leaf fields. */
    while (json_next(&jp, &tok) == JSON_KEY && depth_guard++ < 200) {
        if (json_key_is(&tok, "event")) {
            json_next(&jp, &tok); /* OBJECT_BEGIN */
            while (json_next(&jp, &tok) == JSON_KEY) {
                if (json_key_is(&tok, "data")) {
                    json_next(&jp, &tok);
                    while (json_next(&jp, &tok) == JSON_KEY) {
                        if (json_key_is(&tok, "new_state")) {
                            json_next(&jp, &tok);
                            while (json_next(&jp, &tok) == JSON_KEY) {
                                if (json_key_is(&tok, "entity_id")) {
                                    json_next(&jp, &tok);
                                    json_str_copy(&tok, entity, sizeof entity);
                                } else if (json_key_is(&tok, "state")) {
                                    json_next(&jp, &tok);
                                    json_str_copy(&tok, state, sizeof state);
                                } else if (json_key_is(&tok, "attributes")) {
                                    json_next(&jp, &tok);
                                    while (json_next(&jp, &tok) == JSON_KEY) {
                                        if (json_key_is(&tok, "unit_of_measurement")) {
                                            json_next(&jp, &tok);
                                            json_str_copy(&tok, unit, sizeof unit);
                                        } else if (json_key_is(&tok, "friendly_name")) {
                                            json_next(&jp, &tok);
                                            json_str_copy(&tok, fname, sizeof fname);
                                        } else {
                                            json_next(&jp, &tok);
                                            json_skip(&jp, &tok);
                                        }
                                    }
                                } else {
                                    json_next(&jp, &tok);
                                    json_skip(&jp, &tok);
                                }
                            }
                        } else {
                            json_next(&jp, &tok);
                            json_skip(&jp, &tok);
                        }
                    }
                } else {
                    json_next(&jp, &tok);
                    json_skip(&jp, &tok);
                }
            }
        } else {
            json_next(&jp, &tok);
            json_skip(&jp, &tok);
        }
    }

    CHECK_STR(entity, "sensor.wohnzimmer_temperatur");
    CHECK_STR(state, "21.4");
    CHECK_INT((unsigned char)unit[0], 0xB0);
    CHECK_STR(unit + 1, "C");
    CHECK_STR(fname, "Wohnzimmer Temperatur");
    CHECK(jp.err == NULL);
}

static void test_fixed_point(void)
{
    /* Numbers are parsed without floating point: a 68000 has no FPU, and
     * strtod would drag in mathieeedoubbas.library at runtime. */
    static const struct { const char *num; int scale; long want; } cases[] = {
        { "0",         0, 0      },
        { "42",        0, 42     },
        { "-42",       0, -42    },
        { "21.4",      1, 214    },
        { "21.4",      2, 2140   },
        { "21.45",     2, 2145   },
        { "1013.25",   2, 101325 },
        { "-3.5",      1, -35    },
        { "0.001",     3, 1      },
        /* the first dropped digit rounds half away from zero */
        { "21.45",     1, 215    },
        { "21.44",     1, 214    },
        { "-21.45",    1, -215   },
        { "2.5",       0, 3      },
        { "2.4",       0, 2      },
        /* exponents */
        { "1e3",       0, 1000   },
        { "1.5e2",     0, 150    },
        { "15e-1",     0, 2      },
        { "1e2",       2, 10000  }
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char        doc[64];
        json_parser jp;
        json_token  tok;
        long        got = 0;

        sprintf(doc, "{\"v\":%s}", cases[i].num);
        CHECK_INT(seek(&jp, doc, "v", &tok), JSON_NUMBER);
        CHECK(json_fixed(&tok, &got, cases[i].scale));
        if (got != cases[i].want)
            printf("    (input %s scale %d -> %ld, want %ld)\n",
                   cases[i].num, cases[i].scale, got, cases[i].want);
        CHECK_INT(got, cases[i].want);
    }
}

static void test_fixed_point_saturates(void)
{
    /* long is 32-bit on 68k. An absurd value must clamp, not wrap round to
     * a small or negative number and quietly corrupt a gauge. */
    json_parser jp;
    json_token  tok;
    long        got = 0;

    /* 30 digits overflows long whether it is 32 or 64 bits wide, so this
     * asserts the same thing on the Amiga and on the test host. */
    CHECK_INT(seek(&jp, "{\"v\":999999999999999999999999999999}", "v", &tok),
              JSON_NUMBER);
    CHECK(json_fixed(&tok, &got, 0));
    CHECK_INT(got, LONG_MAX);

    CHECK_INT(seek(&jp, "{\"v\":-999999999999999999999999999999}", "v", &tok),
              JSON_NUMBER);
    CHECK(json_fixed(&tok, &got, 0));
    CHECK(got < 0);

    /* Scaling must not overflow either. */
    CHECK_INT(seek(&jp, "{\"v\":2000000}", "v", &tok), JSON_NUMBER);
    CHECK(json_fixed(&tok, &got, 6));
    CHECK(got > 0);
}

static void test_fixed_point_rejects_non_numbers(void)
{
    json_parser jp;
    json_token  tok;
    long        got = 12345;

    CHECK_INT(seek(&jp, "{\"v\":\"hello\"}", "v", &tok), JSON_STRING);
    CHECK_INT(json_fixed(&tok, &got, 0), 0);
    CHECK_INT(got, 12345); /* left untouched on failure */
}

void suite_json(void)
{
    RUN(test_flat_object);
    RUN(test_nesting_and_skip);
    RUN(test_array_walk);
    RUN(test_empty_containers);
    RUN(test_escapes);
    RUN(test_utf8_to_latin1);
    RUN(test_charset_folds);
    RUN(test_surrogate_pair);
    RUN(test_truncation_is_safe);
    RUN(test_errors);
    RUN(test_ha_state_payload);
    RUN(test_fixed_point);
    RUN(test_fixed_point_saturates);
    RUN(test_fixed_point_rejects_non_numbers);
}
