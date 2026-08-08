/* ami2ha -- character set conversion tests */
#include "tinytest.h"
#include "ami2ha/charset.h"
#include "ami2ha/config.h"

static void test_utf8_input(void)
{
    char out[64];

    /* "Küche" as UTF-8 -> Latin-1 */
    CHECK_INT(charset_utf8_to_latin1(out, sizeof out, "K\xC3\xBC" "che", 6), 5);
    CHECK_INT((unsigned char)out[1], 0xFC);
    CHECK_STR(out + 2, "che");

    /* degree sign */
    charset_utf8_to_latin1(out, sizeof out, "\xC2\xB0" "C", 3);
    CHECK_INT((unsigned char)out[0], 0xB0);
    CHECK_STR(out + 1, "C");
}

static void test_latin1_input_passes_through(void)
{
    /*
     * The same text already saved as Latin-1 must survive untouched. This
     * is what lets one parser accept files written on either machine.
     */
    char out[64];
    char in[8];

    in[0] = 'K';
    in[1] = (char)0xFC; /* lone 0xFC is not valid UTF-8 */
    in[2] = 'c';
    in[3] = 'h';
    in[4] = 'e';
    in[5] = '\0';

    CHECK_INT(charset_utf8_to_latin1(out, sizeof out, in, 5), 5);
    CHECK_INT((unsigned char)out[1], 0xFC);
    CHECK_STR(out + 2, "che");
}

static void test_ascii_unchanged(void)
{
    char out[64];

    CHECK_INT(charset_utf8_to_latin1(out, sizeof out, "Living Room", 11), 11);
    CHECK_STR(out, "Living Room");
}

static void test_folds(void)
{
    char out[64];

    charset_utf8_to_latin1(out, sizeof out, "\xE2\x82\xAC", 3);
    CHECK_STR(out, "EUR");

    charset_utf8_to_latin1(out, sizeof out, "don\xE2\x80\x99t", 7);
    CHECK_STR(out, "don't");

    /* astral plane: nothing sensible to fall back to */
    charset_utf8_to_latin1(out, sizeof out, "\xF0\x9F\x98\x80", 4);
    CHECK_STR(out, "?");
}

static void test_overlong_is_rejected(void)
{
    /*
     * C0 80 is an overlong encoding of NUL. Accepting it would let a byte
     * pair that is really Latin-1 decode to something unrelated, so it must
     * fall back to per-byte passthrough.
     */
    char out[64];
    size_t n = charset_utf8_to_latin1(out, sizeof out, "\xC0\x80", 2);

    CHECK_INT(n, 2);
    CHECK_INT((unsigned char)out[0], 0xC0);
    CHECK_INT((unsigned char)out[1], 0x80);
}

static void test_truncated_sequence(void)
{
    /* A multi-byte lead with nothing following must not read past the end. */
    char out[64];

    CHECK_INT(charset_utf8_to_latin1(out, sizeof out, "ab\xC3", 3), 3);
    CHECK_INT((unsigned char)out[2], 0xC3);
}

static void test_output_is_bounded(void)
{
    char out[5];

    /* Must truncate and terminate, never overrun. */
    CHECK_INT(charset_utf8_to_latin1(out, sizeof out, "abcdefghij", 10), 4);
    CHECK_STR(out, "abcd");

    /* A fold that does not fit is truncated rather than half-written. */
    CHECK(charset_utf8_to_latin1(out, sizeof out, "ab\xE2\x82\xAC", 5) <= 4);
    CHECK_INT(out[4], 0);
}

static void test_config_labels_are_transcoded(void)
{
    /*
     * The end-to-end case that matters: a dashboard file saved as UTF-8 by
     * an editor on a Mac must show correct umlauts on the Amiga.
     */
    a2h_config cfg;
    char       err[CFG_ERR_MAX];
    static const char *doc =
        "group \"K\xC3\xBC" "che\"\n"
        "    toggle light.kueche label \"Deckenlicht \xE2\x80\x93 hell\"\n"
        "end\n";

    CHECK(cfg_parse(&cfg, doc, strlen(doc), err, sizeof err));

    /* Group title: "Küche" in Latin-1 */
    CHECK_INT((unsigned char)cfg.groups[0].title[1], 0xFC);
    CHECK_STR(cfg.groups[0].title + 2, "che");

    /* Label: en dash folded to a plain hyphen */
    CHECK_STR(cfg.widgets[0].label, "Deckenlicht - hell");

    /* Entity ids must NOT be touched. */
    CHECK_STR(cfg.widgets[0].entity, "light.kueche");
    cfg_free(&cfg);
}

void suite_charset(void)
{
    RUN(test_utf8_input);
    RUN(test_latin1_input_passes_through);
    RUN(test_ascii_unchanged);
    RUN(test_folds);
    RUN(test_overlong_is_rejected);
    RUN(test_truncated_sequence);
    RUN(test_output_is_bounded);
    RUN(test_config_labels_are_transcoded);
}
