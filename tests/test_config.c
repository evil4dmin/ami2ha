/* ami2ha -- dashboard configuration tests */
#include "tinytest.h"
#include "ami2ha/config.h"
#include "ami2ha/buf.h"
#include "ami2ha/entity.h"

#include <stdio.h>

static int parse(a2h_config *cfg, const char *text, char *err, size_t errsz)
{
    return cfg_parse(cfg, text, strlen(text), err, errsz);
}

static void test_globals(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg,
        "# a comment\n"
        "; another comment\n"
        "host      homeassistant.local\n"
        "port      8123\n"
        "tokenfile S:ha.token\n"
        "columns   2\n"
        "refresh   30\n", err, sizeof err));

    CHECK_STR(cfg.host, "homeassistant.local");
    CHECK_INT(cfg.port, 8123);
    CHECK_STR(cfg.tokenfile, "S:ha.token");
    CHECK_INT(cfg.columns, 2);
    CHECK_INT(cfg.refresh_secs, 30);
    CHECK_INT(cfg.ngroups, 0);
    cfg_free(&cfg);
}

static void test_defaults(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg, "host ha.local\n", err, sizeof err));
    CHECK_INT(cfg.port, 8123);
    CHECK_INT(cfg.columns, 1);
    cfg_free(&cfg);
}

static void test_tls_defaults_off_but_verified(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    /* Plain http is the default, but the moment TLS is switched on the
     * certificate must be checked without anyone having to ask. */
    CHECK(parse(&cfg, "host ha.local\n", err, sizeof err));
    CHECK_INT(cfg.tls, 0);
    CHECK_INT(cfg.tls_verify, 1);
    cfg_free(&cfg);

    CHECK(parse(&cfg, "host ha.local\ntls yes\n", err, sizeof err));
    CHECK_INT(cfg.tls, 1);
    CHECK_INT(cfg.tls_verify, 1);
    cfg_free(&cfg);
}

static void test_tls_spellings(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];
    /* People will write any of these; rejecting a file over the spelling
     * of "yes" would be needless. */
    static const char *const yes[] = { "yes", "on", "true", "1" };
    static const char *const no[]  = { "no", "off", "false", "0" };
    char text[64];
    size_t i;

    for (i = 0; i < sizeof yes / sizeof yes[0]; i++) {
        sprintf(text, "host h\ntls %s\n", yes[i]);
        CHECK(parse(&cfg, text, err, sizeof err));
        CHECK_INT(cfg.tls, 1);
        cfg_free(&cfg);
    }
    for (i = 0; i < sizeof no / sizeof no[0]; i++) {
        sprintf(text, "host h\ntls %s\n", no[i]);
        CHECK(parse(&cfg, text, err, sizeof err));
        CHECK_INT(cfg.tls, 0);
        cfg_free(&cfg);
    }

    CHECK(!parse(&cfg, "host h\ntls maybe\n", err, sizeof err));
    cfg_free(&cfg);
}

static void test_tls_survives_a_write(void)
{
    a2h_config cfg, back;
    char       err[CFG_ERR_MAX];
    a2h_buf    out;

    /* Turning verification off is the setting most worth losing track of:
     * silently dropping it on a rewrite would quietly re-break a working
     * self-signed setup. */
    CHECK(parse(&cfg, "host ha.local\ntls yes\ntlsverify no\n",
                err, sizeof err));
    CHECK_INT(cfg.tls, 1);
    CHECK_INT(cfg.tls_verify, 0);

    buf_init(&out);
    CHECK(cfg_write(&cfg, &out));
    CHECK(parse(&back, (const char *)out.data, err, sizeof err));
    CHECK_INT(back.tls, 1);
    CHECK_INT(back.tls_verify, 0);

    buf_free(&out);
    cfg_free(&cfg);
    cfg_free(&back);
}

static void test_camera_widget(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg,
        "group \"Hof\"\n"
        "    camera camera.einfahrt label \"Einfahrt\" width 320 height 180\n"
        "    camera camera.tuer\n"
        "end\n", err, sizeof err));

    CHECK_INT(cfg.nwidgets, 2);
    CHECK_INT(cfg.widgets[0].kind, W_CAMERA);
    CHECK_STR(cfg.widgets[0].entity, "camera.einfahrt");
    CHECK_STR(cfg.widgets[0].label, "Einfahrt");
    CHECK_INT(cfg.widgets[0].cam_w, 320);
    CHECK_INT(cfg.widgets[0].cam_h, 180);
    /* Manual refresh unless asked otherwise: decoding costs real time. */
    CHECK_INT(cfg.widgets[0].cam_refresh, 0);

    /* A camera with no size still gets one. Asking Home Assistant for
     * nothing means it sends the native frame -- five times the bytes. */
    CHECK_INT(cfg.widgets[1].cam_w, 320);
    CHECK_INT(cfg.widgets[1].cam_h, 180);
    cfg_free(&cfg);
}

static void test_camera_rejects_silly_sizes(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(!parse(&cfg, "group \"g\"\n camera camera.a width 4000\n end\n",
                 err, sizeof err));
    cfg_free(&cfg);
    CHECK(!parse(&cfg, "group \"g\"\n camera camera.a height 0\n end\n",
                 err, sizeof err));
    cfg_free(&cfg);
    CHECK(!parse(&cfg, "group \"g\"\n camera camera.a refresh -5\n end\n",
                 err, sizeof err));
    cfg_free(&cfg);
}

static void test_camera_survives_a_write(void)
{
    a2h_config cfg, back;
    char       err[CFG_ERR_MAX];
    a2h_buf    out;

    CHECK(parse(&cfg,
        "group \"Hof\"\n"
        "    camera camera.einfahrt label \"Einfahrt\" width 640 height 360 refresh 300\n"
        "end\n", err, sizeof err));

    buf_init(&out);
    CHECK(cfg_write(&cfg, &out));
    CHECK(parse(&back, (const char *)out.data, err, sizeof err));
    CHECK_INT(back.nwidgets, 1);
    CHECK_INT(back.widgets[0].kind, W_CAMERA);
    CHECK_INT(back.widgets[0].cam_w, 640);
    CHECK_INT(back.widgets[0].cam_h, 360);
    CHECK_INT(back.widgets[0].cam_refresh, 300);

    buf_free(&out);
    cfg_free(&cfg);
    cfg_free(&back);
}

static void test_camera_discovered_from_label(void)
{
    a2h_config cfg;

    /* A camera carrying the label becomes a camera tile, the same way a
     * switch becomes a toggle -- no separate selection mechanism. */
    cfg_init(&cfg);
    CHECK(cfg_add_discovered(&cfg, "camera.hof", "amiga"));
    CHECK(cfg_add_discovered(&cfg, "switch.lamp", "amiga"));
    CHECK_INT(cfg.widgets[0].kind, W_CAMERA);
    CHECK_INT(cfg.widgets[0].cam_w, 320);
    CHECK_INT(cfg.widgets[0].cam_h, 180);
    CHECK_INT(cfg.widgets[1].kind, W_TOGGLE);
    cfg_free(&cfg);
}

static void test_media_widget(void)
{
    a2h_config cfg, back;
    char       err[CFG_ERR_MAX];
    a2h_buf    out;

    CHECK(parse(&cfg,
        "group \"Musik\"\n"
        "    media media_player.squeezer label \"Squeezer\"\n"
        "    media media_player.kueche\n"
        "end\n", err, sizeof err));

    CHECK_INT(cfg.nwidgets, 2);
    CHECK_INT(cfg.widgets[0].kind, W_MEDIA);
    CHECK_STR(cfg.widgets[0].entity, "media_player.squeezer");
    CHECK_STR(cfg.widgets[0].label, "Squeezer");
    /* No label given: derived from the id, as for every other kind. */
    CHECK_STR(cfg.widgets[1].label, "Kueche");

    /* And it survives being written back out, which is what the settings
     * window does to the whole file on every save. */
    buf_init(&out);
    CHECK(cfg_write(&cfg, &out));
    CHECK(parse(&back, (const char *)out.data, err, sizeof err));
    CHECK_INT(back.nwidgets, 2);
    CHECK_INT(back.widgets[0].kind, W_MEDIA);
    CHECK_STR(back.widgets[0].entity, "media_player.squeezer");
    CHECK_STR(back.widgets[0].label, "Squeezer");
    buf_free(&out);

    cfg_free(&cfg);
    cfg_free(&back);
}

static void test_media_discovered_from_label(void)
{
    a2h_config cfg;

    /* A labelled media_player becomes a media widget, the same way a camera
     * becomes a tile -- no separate selection mechanism. */
    cfg_init(&cfg);
    CHECK(cfg_add_discovered(&cfg, "media_player.squeezer", "amiga"));
    CHECK(cfg_add_discovered(&cfg, "sensor.temperatur", "amiga"));
    CHECK_INT(cfg.widgets[0].kind, W_MEDIA);
    CHECK_INT(cfg.widgets[1].kind, W_SENSOR);
    cfg_free(&cfg);
}

static void test_hand_written_labels_are_marked(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    /* A dashboard that also carries a `label` line used to have every
     * caption replaced by Home Assistant's friendly name. The friendly name
     * beats an entity id, but never beats what someone chose. */
    CHECK(parse(&cfg,
        "label amiga\n"
        "group \"g\"\n"
        "    sensor sensor.a label \"Aussentemperatur\"\n"
        "    sensor sensor.b\n"
        "end\n", err, sizeof err));

    CHECK_INT(cfg.widgets[0].label_explicit, 1);
    CHECK_STR(cfg.widgets[0].label, "Aussentemperatur");
    /* Derived from the entity id, so the friendly name may replace it. */
    CHECK_INT(cfg.widgets[1].label_explicit, 0);
    cfg_free(&cfg);
}

static void test_discovered_labels_are_not_explicit(void)
{
    a2h_config cfg;

    cfg_init(&cfg);
    CHECK(cfg_add_discovered(&cfg, "sensor.hof_temperatur", "amiga"));
    CHECK_INT(cfg.widgets[0].label_explicit, 0);
    cfg_free(&cfg);
}

static void test_camera_timestamp_is_optional(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    /* On unless the line says otherwise: someone who asked for a camera
     * wants to know how old the picture is. */
    CHECK(parse(&cfg, "group \"g\"\n camera camera.a\n end\n",
                err, sizeof err));
    CHECK_INT(cfg.widgets[0].cam_stamp, 1);
    cfg_free(&cfg);

    CHECK(parse(&cfg,
        "group \"g\"\n camera camera.a timestamp no\n end\n",
        err, sizeof err));
    CHECK_INT(cfg.widgets[0].cam_stamp, 0);
    cfg_free(&cfg);

    CHECK(!parse(&cfg, "group \"g\"\n camera camera.a timestamp perhaps\n end\n",
                 err, sizeof err));
    cfg_free(&cfg);
}

static void test_camera_timestamp_survives_a_write(void)
{
    a2h_config cfg, back;
    char       err[CFG_ERR_MAX];
    a2h_buf    out;

    /* Turning it off is the setting worth losing track of: the default
     * would quietly put the caption back on the next save. */
    CHECK(parse(&cfg,
        "group \"g\"\n camera camera.a timestamp no\n end\n",
        err, sizeof err));

    buf_init(&out);
    CHECK(cfg_write(&cfg, &out));
    CHECK(parse(&back, (const char *)out.data, err, sizeof err));
    CHECK_INT(back.widgets[0].cam_stamp, 0);

    buf_free(&out);
    cfg_free(&cfg);
    cfg_free(&back);
}

static void test_groups_and_widgets(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg,
        "group \"Wohnzimmer\"\n"
        "    sensor sensor.wz_temperatur label \"Temperatur\"\n"
        "    gauge  sensor.wz_co2 label \"CO2\" min 400 max 2000\n"
        "    toggle light.wohnzimmer label \"Licht\"\n"
        "end\n"
        "\n"
        "group \"Szenen\"\n"
        "    button scene.gute_nacht label \"Gute Nacht\"\n"
        "end\n", err, sizeof err));

    CHECK_INT(cfg.ngroups, 2);
    CHECK_INT(cfg.nwidgets, 4);

    CHECK_STR(cfg.groups[0].title, "Wohnzimmer");
    CHECK_INT(cfg.groups[0].first_widget, 0);
    CHECK_INT(cfg.groups[0].nwidgets, 3);

    CHECK_STR(cfg.groups[1].title, "Szenen");
    CHECK_INT(cfg.groups[1].first_widget, 3);
    CHECK_INT(cfg.groups[1].nwidgets, 1);

    CHECK_INT(cfg.widgets[0].kind, W_SENSOR);
    CHECK_STR(cfg.widgets[0].entity, "sensor.wz_temperatur");
    CHECK_STR(cfg.widgets[0].label, "Temperatur");
    CHECK_INT(cfg.widgets[0].group, 0);

    CHECK_INT(cfg.widgets[1].kind, W_GAUGE);
    CHECK_INT(cfg.widgets[1].min, 400);
    CHECK_INT(cfg.widgets[1].max, 2000);

    CHECK_INT(cfg.widgets[2].kind, W_TOGGLE);
    CHECK_STR(cfg.widgets[2].entity, "light.wohnzimmer");

    CHECK_INT(cfg.widgets[3].kind, W_BUTTON);
    CHECK_STR(cfg.widgets[3].service, "scene.gute_nacht");
    CHECK_INT(cfg.widgets[3].group, 1);
    cfg_free(&cfg);
}

static void test_label_defaults_from_entity(void)
{
    /* Typing a label for every widget would be tedious; derive a readable
     * one when it is omitted. */
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg,
        "group \"G\"\n"
        "    sensor sensor.kitchen_power_usage\n"
        "    button script.good_night\n"
        "end\n", err, sizeof err));

    CHECK_STR(cfg.widgets[0].label, "Kitchen power usage");
    CHECK_STR(cfg.widgets[1].label, "Good night");
    cfg_free(&cfg);
}

static void test_snapshot_name_uses_the_label(void)
{
    /* The drawer should read the way the tile does. */
    char name[CFG_LABEL_MAX];

    cfg_label_filename(name, sizeof name, "Einfahrt", "camera.driveway_fluent");
    CHECK_STR(name, "Einfahrt");
}

static void test_snapshot_name_falls_back_to_the_entity(void)
{
    /* No label, or a label with nothing usable in it: a file still needs a
     * name, and the entity's local part is the one we used to use. */
    char name[CFG_LABEL_MAX];

    cfg_label_filename(name, sizeof name, "", "camera.driveway_fluent");
    CHECK_STR(name, "driveway_fluent");

    cfg_label_filename(name, sizeof name, "///", "camera.driveway_fluent");
    CHECK_STR(name, "driveway_fluent");

    cfg_label_filename(name, sizeof name, "", "driveway");
    CHECK_STR(name, "driveway");
}

static void test_snapshot_name_is_safe_on_a_filesystem(void)
{
    /* AmigaDOS reads ':' and '/' as path syntax and '#?' as a pattern, so a
     * label containing them must not reach Open() intact. */
    char name[CFG_LABEL_MAX];

    cfg_label_filename(name, sizeof name, "PV Dach / Ost", "sensor.pv");
    CHECK_STR(name, "PV_Dach_Ost");

    cfg_label_filename(name, sizeof name, "Work:Cam*?", "camera.x");
    CHECK_STR(name, "Work_Cam");

    /* Leading and trailing punctuation leaves no stray separator. */
    cfg_label_filename(name, sizeof name, "  Garten  ", "camera.x");
    CHECK_STR(name, "Garten");
}

static void test_snapshot_name_keeps_umlauts(void)
{
    /* Latin-1 is what the Amiga's filesystem stores and its fonts draw. */
    char name[CFG_LABEL_MAX];

    cfg_label_filename(name, sizeof name, "K\xfc" "che", "camera.kitchen");
    CHECK_STR(name, "K\xfc" "che");
}

static void test_snapshot_name_is_bounded(void)
{
    /* Never writes past the buffer, and never ends on a separator. */
    char name[8];
    size_t n;

    n = cfg_label_filename(name, sizeof name, "Einfahrt Nord", "camera.x");
    CHECK(n < sizeof name);
    CHECK_STR(name, "Einfahr");

    n = cfg_label_filename(name, 1, "Einfahrt", "camera.x");
    CHECK(n == 0);
    CHECK_STR(name, "");
}

static void test_button_with_json_data(void)
{
    /* An unquoted JSON object must survive as a single token, so the common
     * case needs no quoting gymnastics. */
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg,
        "group \"G\"\n"
        "    button light.turn_on label \"Hell\" entity light.kueche "
        "data {\"brightness\":255}\n"
        "end\n", err, sizeof err));

    CHECK_INT(cfg.nwidgets, 1);
    CHECK_STR(cfg.widgets[0].service, "light.turn_on");
    CHECK_STR(cfg.widgets[0].entity, "light.kueche");
    CHECK_STR(cfg.widgets[0].data, "{\"brightness\":255}");
    CHECK_STR(cfg.widgets[0].label, "Hell");
    cfg_free(&cfg);
}

static void test_line_continuation(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg,
        "group \"G\"\n"
        "    gauge sensor.power \\\n"
        "        label \"Power\" \\\n"
        "        min 0 max 3000\n"
        "end\n", err, sizeof err));

    CHECK_INT(cfg.nwidgets, 1);
    CHECK_STR(cfg.widgets[0].label, "Power");
    CHECK_INT(cfg.widgets[0].min, 0);
    CHECK_INT(cfg.widgets[0].max, 3000);
    cfg_free(&cfg);
}

static void test_comments_and_blank_lines(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg,
        "\n\n"
        "group \"G\"   # trailing comment\n"
        "\n"
        "    ; a whole-line comment\n"
        "    sensor sensor.a   # another trailing comment\n"
        "\n"
        "end\n", err, sizeof err));

    CHECK_INT(cfg.ngroups, 1);
    CHECK_INT(cfg.nwidgets, 1);
    CHECK_STR(cfg.groups[0].title, "G");
    CHECK_STR(cfg.widgets[0].entity, "sensor.a");
    cfg_free(&cfg);
}

static void test_text_widget(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK(parse(&cfg,
        "group \"G\"\n"
        "    text \"Just a caption\"\n"
        "end\n", err, sizeof err));

    CHECK_INT(cfg.widgets[0].kind, W_TEXT);
    CHECK_STR(cfg.widgets[0].label, "Just a caption");
    CHECK_STR(cfg.widgets[0].entity, "");
    cfg_free(&cfg);
}

/* ---- error reporting ---- */

static void check_error(const char *text, const char *expect_substr)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK_INT(cfg_parse(&cfg, text, strlen(text), err, sizeof err), 0);
    if (!strstr(err, expect_substr))
        printf("    (error was \"%s\", wanted to contain \"%s\")\n",
               err, expect_substr);
    CHECK(strstr(err, expect_substr) != NULL);
    /* Every message must name the line, or it is useless for a hand-edited
     * file. */
    CHECK(strstr(err, "line ") == err);
    cfg_free(&cfg);
}

static void test_errors(void)
{
    check_error("group \"G\"\n    sensor sensor.a\n", "missing 'end'");
    check_error("end\n", "without 'group'");
    check_error("group \"A\"\ngroup \"B\"\n", "inside a group");
    check_error("sensor sensor.a\n", "outside any group");
    check_error("group \"G\"\n    sensor\nend\n", "missing entity");
    check_error("group \"G\"\n    sensor sensor.a bogus 1\nend\n",
                "unknown widget option");
    check_error("wibble 3\n", "unknown keyword");
    check_error("port abc\n", "expected a number");
    check_error("group \"G\"\n    gauge sensor.a min 100 max 10\nend\n",
                "greater than min");
    check_error("group \"unterminated\n", "unterminated quoted string");
    check_error("group \"G\"\n    sensor sensor.a decimals 9\nend\n",
                "decimals must be");
}

static void test_error_line_numbers(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];

    CHECK_INT(parse(&cfg,
        "host ha.local\n"      /* 1 */
        "\n"                   /* 2 */
        "group \"G\"\n"        /* 3 */
        "    sensor sensor.a\n"/* 4 */
        "    wibble\n"         /* 5 */
        "end\n", err, sizeof err), 0);
    CHECK_STR(err, "line 5: unknown keyword 'wibble'");
    cfg_free(&cfg);
}

static void test_bounds(void)
{
    /* A runaway file must be refused, not allowed to overrun the arrays. */
    a2h_config cfg;
    char       err[CFG_ERR_MAX];
    a2h_buf    big;
    int        i;

    buf_init(&big);
    buf_append_str(&big, "group \"G\"\n");
    for (i = 0; i < CFG_MAX_WIDGETS + 50; i++)
        buf_printf(&big, "    sensor sensor.probe_%d\n", i);
    buf_append_str(&big, "end\n");

    CHECK_INT(cfg_parse(&cfg, (const char *)big.data, big.len, err, sizeof err), 0);
    CHECK(strstr(err, "too many widgets") != NULL);
    CHECK(cfg.nwidgets <= CFG_MAX_WIDGETS);
    buf_free(&big);

    buf_init(&big);
    for (i = 0; i < CFG_MAX_GROUPS + 5; i++)
        buf_printf(&big, "group \"G%d\"\nend\n", i);
    CHECK_INT(cfg_parse(&cfg, (const char *)big.data, big.len, err, sizeof err), 0);
    CHECK(strstr(err, "too many groups") != NULL);
    CHECK(cfg.ngroups <= CFG_MAX_GROUPS);
    buf_free(&big);
    cfg_free(&cfg);
}

static void test_long_values_truncate(void)
{
    a2h_config cfg;
    char       err[CFG_ERR_MAX];
    a2h_buf    b;
    int        i;

    buf_init(&b);
    buf_append_str(&b, "group \"G\"\n    sensor sensor.a label \"");
    for (i = 0; i < 200; i++)
        buf_append_byte(&b, 'x');
    buf_append_str(&b, "\"\nend\n");

    CHECK(cfg_parse(&cfg, (const char *)b.data, b.len, err, sizeof err));
    CHECK_INT(strlen(cfg.widgets[0].label), CFG_LABEL_MAX - 1);
    buf_free(&b);
    cfg_free(&cfg);
}

static void test_generate_roundtrip(void)
{
    /*
     * The generated file is the main way anyone will start a dashboard, so
     * it has to parse cleanly and preserve every entity. Generating and
     * re-parsing catches quoting and escaping mistakes that eyeballing the
     * output would not.
     */
    ha_store   store;
    a2h_config gen, cfg;
    a2h_buf    out;
    char       err[CFG_ERR_MAX];
    ha_entity *e;
    int        i, toggles = 0, buttons = 0, sensors = 0;

    CHECK(ha_store_init(&store, 32));

    e = ha_store_put(&store, "sensor.wohnzimmer_temperatur");
    ha_entity_set_name(e, "Wohnzimmer Temperatur");
    e = ha_store_put(&store, "light.kueche");
    ha_entity_set_name(e, "K\xFC" "che");        /* Latin-1 umlaut */
    e = ha_store_put(&store, "switch.pumpe");
    e = ha_store_put(&store, "scene.gute_nacht");
    ha_entity_set_name(e, "Gute Nacht");
    e = ha_store_put(&store, "script.alles_aus");
    e = ha_store_put(&store, "binary_sensor.tuer");

    cfg_init(&gen);
    strcpy(gen.host, "homeassistant.local");
    gen.port = 8123;
    strcpy(gen.tokenfile, "S:ha.token");

    buf_init(&out);
    CHECK(cfg_generate(&out, &store, &gen));
    CHECK(out.len > 0);

    /* It must round-trip. */
    CHECK(cfg_parse(&cfg, (const char *)out.data, out.len, err, sizeof err));
    if (err[0])
        printf("    (generated file failed to parse: %s)\n", err);

    CHECK_STR(cfg.host, "homeassistant.local");
    CHECK_STR(cfg.tokenfile, "S:ha.token");

    /* Every entity survived, with a sensible widget kind. */
    CHECK_INT(cfg.nwidgets, 6);
    for (i = 0; i < cfg.nwidgets; i++) {
        switch (cfg.widgets[i].kind) {
        case W_TOGGLE: toggles++; break;
        case W_BUTTON: buttons++; break;
        case W_SENSOR: sensors++; break;
        default: break;
        }
    }
    CHECK_INT(toggles, 2); /* light + switch  */
    CHECK_INT(buttons, 2); /* scene + script  */
    CHECK_INT(sensors, 2); /* sensor + binary_sensor */

    /* Domains became groups: sensor, light, switch, scene, script,
     * binary_sensor. */
    CHECK_INT(cfg.ngroups, 6);

    /* A friendly name containing a non-ASCII byte must come back intact. */
    for (i = 0; i < cfg.nwidgets; i++)
        if (strcmp(cfg.widgets[i].entity, "light.kueche") == 0)
            CHECK_INT((unsigned char)cfg.widgets[i].label[1], 0xFC);

    /* Buttons target a real service with the entity carried alongside. */
    for (i = 0; i < cfg.nwidgets; i++)
        if (strcmp(cfg.widgets[i].service, "scene.turn_on") == 0)
            CHECK_STR(cfg.widgets[i].entity, "scene.gute_nacht");

    buf_free(&out);
    ha_store_free(&store);
}

static void test_generate_empty_store(void)
{
    ha_store   store;
    a2h_config gen, cfg;
    a2h_buf    out;
    char       err[CFG_ERR_MAX];

    CHECK(ha_store_init(&store, 8));
    cfg_init(&gen);
    buf_init(&out);

    CHECK(cfg_generate(&out, &store, &gen));
    /* Still has to be a valid file, not a broken stub. */
    CHECK(cfg_parse(&cfg, (const char *)out.data, out.len, err, sizeof err));
    CHECK_INT(cfg.ngroups, 0);

    buf_free(&out);
    ha_store_free(&store);
}

/* Compare two configurations field by field, so a round trip is provable. */
static void expect_same(const a2h_config *a, const a2h_config *b)
{
    int i;

    CHECK_STR(a->host, b->host);
    CHECK_INT(a->port, b->port);
    CHECK_STR(a->tokenfile, b->tokenfile);
    CHECK_STR(a->label, b->label);
    CHECK_INT(a->columns, b->columns);
    CHECK_INT(a->refresh_secs, b->refresh_secs);
    CHECK_INT(a->ngroups, b->ngroups);
    CHECK_INT(a->nwidgets, b->nwidgets);

    for (i = 0; i < a->ngroups && i < b->ngroups; i++) {
        CHECK_STR(a->groups[i].title, b->groups[i].title);
        CHECK_INT(a->groups[i].nwidgets, b->groups[i].nwidgets);
        CHECK_INT(a->groups[i].first_widget, b->groups[i].first_widget);
    }
    for (i = 0; i < a->nwidgets && i < b->nwidgets; i++) {
        CHECK_INT(a->widgets[i].kind, b->widgets[i].kind);
        CHECK_STR(a->widgets[i].entity, b->widgets[i].entity);
        CHECK_STR(a->widgets[i].label, b->widgets[i].label);
        CHECK_STR(a->widgets[i].service, b->widgets[i].service);
        CHECK_STR(a->widgets[i].data, b->widgets[i].data);
        CHECK_INT(a->widgets[i].min, b->widgets[i].min);
        CHECK_INT(a->widgets[i].max, b->widgets[i].max);
        CHECK_INT(a->widgets[i].decimals, b->widgets[i].decimals);
        CHECK_INT(a->widgets[i].group, b->widgets[i].group);
    }
}

static void test_write_roundtrip(void)
{
    /*
     * The settings window will save through cfg_write, so whatever the user
     * arranged has to survive being written and read back exactly.
     */
    a2h_config cfg, back;
    a2h_buf    out;
    char       err[CFG_ERR_MAX];
    static const char *doc =
        "host      ha.local\n"
        "port      8124\n"
        "tokenfile S:ha.token\n"
        "label     amiga\n"
        "columns   2\n"
        "refresh   30\n"
        "group \"Wohnzimmer\"\n"
        "    sensor sensor.temp label \"Temperatur\"\n"
        "    gauge  sensor.co2 label \"CO2\" min 400 max 2000\n"
        "    toggle light.wz label \"Licht\" \n"
        "    sensor sensor.hum label \"Feuchte\" decimals 0\n"
        "end\n"
        "group \"Szenen\"\n"
        "    button scene.turn_on entity scene.nacht label \"Gute Nacht\"\n"
        "    button light.turn_on entity light.wz label \"Hell\" "
        "data {\"brightness\":255}\n"
        "    text   \"Ein Klick\"\n"
        "end\n";

    CHECK(cfg_parse(&cfg, doc, strlen(doc), err, sizeof err));

    buf_init(&out);
    CHECK(cfg_write(&cfg, &out));

    CHECK(cfg_parse(&back, (const char *)out.data, out.len, err, sizeof err));
    if (err[0])
        printf("    (written file failed to parse: %s)\n", err);

    expect_same(&cfg, &back);

    /* Writing the result again must produce identical bytes. */
    {
        a2h_buf again;
        buf_init(&again);
        CHECK(cfg_write(&back, &again));
        CHECK_INT(again.len, out.len);
        CHECK(memcmp(again.data, out.data, out.len) == 0);
        buf_free(&again);
    }

    buf_free(&out);
    cfg_free(&cfg);
    cfg_free(&back);
}

static void test_write_survives_awkward_text(void)
{
    a2h_config cfg, back;
    a2h_buf    out;
    char       err[CFG_ERR_MAX];
    static const char *doc =
        "group \"K\xFC" "che & Flur\"\n"
        "    sensor sensor.a label \"Temp \xB0" "C draussen\"\n"
        "end\n";

    CHECK(cfg_parse(&cfg, doc, strlen(doc), err, sizeof err));
    buf_init(&out);
    CHECK(cfg_write(&cfg, &out));
    CHECK(cfg_parse(&back, (const char *)out.data, out.len, err, sizeof err));

    /* Latin-1 and spaces must come back unharmed. */
    CHECK_INT((unsigned char)back.groups[0].title[1], 0xFC);
    CHECK_STR(back.groups[0].title, cfg.groups[0].title);
    CHECK_STR(back.widgets[0].label, cfg.widgets[0].label);

    buf_free(&out);
    cfg_free(&cfg);
    cfg_free(&back);
}

static void test_write_quotes_do_not_break_the_file(void)
{
    /* A label typed with a quote in it must not produce an unreadable file. */
    a2h_config cfg, back;
    a2h_buf    out;
    char       err[CFG_ERR_MAX];

    cfg_init(&cfg);
    strcpy(cfg.host, "ha.local");
    CHECK(cfg_add_discovered(&cfg, "sensor.a", "G"));
    strcpy(cfg.widgets[0].label, "he said \"hi\"");

    buf_init(&out);
    CHECK(cfg_write(&cfg, &out));
    CHECK(cfg_parse(&back, (const char *)out.data, out.len, err, sizeof err));
    CHECK_INT(back.nwidgets, 1);
    CHECK(strstr(back.widgets[0].label, "hi") != NULL);

    buf_free(&out);
    cfg_free(&cfg);
    cfg_free(&back);
}

void suite_config(void)
{
    RUN(test_globals);
    RUN(test_defaults);
    RUN(test_tls_defaults_off_but_verified);
    RUN(test_tls_spellings);
    RUN(test_tls_survives_a_write);
    RUN(test_camera_widget);
    RUN(test_camera_rejects_silly_sizes);
    RUN(test_camera_survives_a_write);
    RUN(test_camera_discovered_from_label);
    RUN(test_hand_written_labels_are_marked);
    RUN(test_discovered_labels_are_not_explicit);
    RUN(test_camera_timestamp_is_optional);
    RUN(test_camera_timestamp_survives_a_write);
    RUN(test_media_widget);
    RUN(test_media_discovered_from_label);
    RUN(test_groups_and_widgets);
    RUN(test_label_defaults_from_entity);
    RUN(test_snapshot_name_uses_the_label);
    RUN(test_snapshot_name_falls_back_to_the_entity);
    RUN(test_snapshot_name_is_safe_on_a_filesystem);
    RUN(test_snapshot_name_keeps_umlauts);
    RUN(test_snapshot_name_is_bounded);
    RUN(test_button_with_json_data);
    RUN(test_line_continuation);
    RUN(test_comments_and_blank_lines);
    RUN(test_text_widget);
    RUN(test_errors);
    RUN(test_error_line_numbers);
    RUN(test_bounds);
    RUN(test_long_values_truncate);
    RUN(test_generate_roundtrip);
    RUN(test_generate_empty_store);
    RUN(test_write_roundtrip);
    RUN(test_write_survives_awkward_text);
    RUN(test_write_quotes_do_not_break_the_file);
}
