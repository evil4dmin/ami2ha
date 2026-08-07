/* ami2ha -- entity store tests */
#include "tinytest.h"
#include "ami2ha/entity.h"

#include <stdio.h>
#include <stdlib.h>

static void test_put_get(void)
{
    ha_store   s;
    ha_entity *a, *b;

    CHECK(ha_store_init(&s, 16));

    a = ha_store_put(&s, "light.kitchen");
    CHECK(a != NULL);
    CHECK_INT(ha_store_count(&s), 1);

    /* Putting the same id again returns the same object, not a duplicate. */
    b = ha_store_put(&s, "light.kitchen");
    CHECK(a == b);
    CHECK_INT(ha_store_count(&s), 1);

    CHECK(ha_store_get(&s, "light.kitchen") == a);
    CHECK(ha_store_get(&s, "light.missing") == NULL);

    ha_store_free(&s);
}

static void test_many_entities_and_collisions(void)
{
    /* Deliberately more entities than buckets, to exercise chaining. */
    ha_store s;
    char     id[HA_ENTITY_ID_MAX];
    int      i;

    CHECK(ha_store_init(&s, 8));

    for (i = 0; i < 300; i++) {
        ha_entity *e;
        sprintf(id, "sensor.probe_%d", i);
        e = ha_store_put(&s, id);
        CHECK(e != NULL);
        if (e) {
            sprintf(id, "%d", i);
            ha_entity_set_state(e, id);
        }
    }
    CHECK_INT(ha_store_count(&s), 300);

    for (i = 0; i < 300; i++) {
        ha_entity *e;
        sprintf(id, "sensor.probe_%d", i);
        e = ha_store_get(&s, id);
        CHECK(e != NULL);
        if (e) {
            sprintf(id, "%d", i);
            CHECK_STR(e->state, id);
        }
    }

    ha_store_free(&s);
}

static void test_iteration_order_is_stable(void)
{
    /* The dashboard iterates this list; entities must not reshuffle. */
    ha_store    s;
    ha_entity  *e;
    const char *want[] = { "a.one", "b.two", "c.three", "d.four" };
    int         i = 0;

    CHECK(ha_store_init(&s, 4));
    ha_store_put(&s, "a.one");
    ha_store_put(&s, "b.two");
    ha_store_put(&s, "c.three");
    ha_store_put(&s, "d.four");

    for (e = ha_store_first(&s); e; e = ha_store_next(e)) {
        CHECK(i < 4);
        if (i < 4)
            CHECK_STR(e->entity_id, want[i]);
        i++;
    }
    CHECK_INT(i, 4);

    ha_store_free(&s);
}

static void test_remove(void)
{
    ha_store   s;
    ha_entity *e;
    int        n;

    CHECK(ha_store_init(&s, 8));
    ha_store_put(&s, "a.one");
    ha_store_put(&s, "b.two");
    ha_store_put(&s, "c.three");

    CHECK_INT(ha_store_remove(&s, "b.two"), 1);
    CHECK_INT(ha_store_remove(&s, "b.two"), 0); /* already gone */
    CHECK_INT(ha_store_count(&s), 2);
    CHECK(ha_store_get(&s, "b.two") == NULL);

    n = 0;
    for (e = ha_store_first(&s); e; e = ha_store_next(e))
        n++;
    CHECK_INT(n, 2);

    /* Removing the tail must leave the list appendable. */
    CHECK_INT(ha_store_remove(&s, "c.three"), 1);
    ha_store_put(&s, "d.four");
    n = 0;
    for (e = ha_store_first(&s); e; e = ha_store_next(e))
        n++;
    CHECK_INT(n, 2);
    CHECK(ha_store_get(&s, "d.four") != NULL);

    /* Removing the head too. */
    CHECK_INT(ha_store_remove(&s, "a.one"), 1);
    CHECK_INT(ha_store_count(&s), 1);
    CHECK_STR(ha_store_first(&s)->entity_id, "d.four");

    ha_store_free(&s);
}

static void test_truncation(void)
{
    ha_store   s;
    ha_entity *e;
    char       longid[200];
    int        i;

    for (i = 0; i < (int)sizeof longid - 1; i++)
        longid[i] = 'x';
    longid[sizeof longid - 1] = '\0';

    CHECK(ha_store_init(&s, 4));
    e = ha_store_put(&s, longid);
    CHECK(e != NULL);
    if (e) {
        CHECK_INT(strlen(e->entity_id), HA_ENTITY_ID_MAX - 1);

        ha_entity_set_state(e, longid);
        CHECK_INT(strlen(e->state), HA_STATE_MAX - 1);

        ha_entity_set_name(e, longid);
        CHECK_INT(strlen(e->name), HA_NAME_MAX - 1);
    }
    ha_store_free(&s);
}

static void test_domain(void)
{
    ha_store   s;
    ha_entity *e;
    char       dom[32];

    CHECK(ha_store_init(&s, 4));

    e = ha_store_put(&s, "binary_sensor.front_door");
    ha_entity_domain(e, dom, sizeof dom);
    CHECK_STR(dom, "binary_sensor");

    /* No dot at all: the whole id is the domain. */
    e = ha_store_put(&s, "weird");
    ha_entity_domain(e, dom, sizeof dom);
    CHECK_STR(dom, "weird");

    /* Truncation must still terminate. */
    e = ha_store_put(&s, "binary_sensor.other");
    ha_entity_domain(e, dom, 5);
    CHECK_STR(dom, "bina");

    ha_store_free(&s);
}

static void test_change_tracking(void)
{
    ha_store   s;
    ha_entity *e;
    unsigned long seq;

    CHECK(ha_store_init(&s, 4));
    e = ha_store_put(&s, "sensor.temp");

    ha_entity_set_state(e, "21.0");
    CHECK_INT(e->changed, 1);
    seq = e->seq;

    e->changed = 0;

    /* Setting the same value must not mark it changed: the UI redraws on
     * this flag, and a sensor reporting an unchanged value every few
     * seconds would otherwise repaint constantly. */
    ha_entity_set_state(e, "21.0");
    CHECK_INT(e->changed, 0);
    CHECK_INT(e->seq, seq);

    ha_entity_set_state(e, "21.5");
    CHECK_INT(e->changed, 1);
    CHECK(e->seq > seq);

    ha_store_free(&s);
}

static void test_attributes(void)
{
    ha_store    s;
    ha_entity  *e;
    const char *v;

    CHECK(ha_store_init(&s, 4));
    e = ha_store_put(&s, "light.kitchen");

    CHECK(ha_entity_attr(e, "brightness") == NULL);

    CHECK_INT(ha_entity_set_attr(e, "brightness", "128"), 1);
    CHECK_INT(ha_entity_set_attr(e, "color_temp", "370"), 1);
    CHECK_INT(ha_entity_set_attr(e, "supported", "yes"), 1);

    v = ha_entity_attr(e, "brightness");
    CHECK(v != NULL);
    if (v) CHECK_STR(v, "128");
    v = ha_entity_attr(e, "color_temp");
    if (v) CHECK_STR(v, "370");

    /* Replacing a value in the middle must not corrupt its neighbours. */
    CHECK_INT(ha_entity_set_attr(e, "color_temp", "500"), 1);
    v = ha_entity_attr(e, "color_temp");
    if (v) CHECK_STR(v, "500");
    v = ha_entity_attr(e, "brightness");
    if (v) CHECK_STR(v, "128");
    v = ha_entity_attr(e, "supported");
    if (v) CHECK_STR(v, "yes");

    /* Replacing with a longer value, then a shorter one. */
    CHECK_INT(ha_entity_set_attr(e, "brightness", "255255255"), 1);
    v = ha_entity_attr(e, "brightness");
    if (v) CHECK_STR(v, "255255255");
    CHECK_INT(ha_entity_set_attr(e, "brightness", "1"), 1);
    v = ha_entity_attr(e, "brightness");
    if (v) CHECK_STR(v, "1");
    v = ha_entity_attr(e, "supported");
    if (v) CHECK_STR(v, "yes");

    CHECK(ha_entity_attr(e, "nope") == NULL);

    ha_entity_clear_attrs(e);
    CHECK(ha_entity_attr(e, "brightness") == NULL);

    ha_store_free(&s);
}

static void test_attribute_blob_is_bounded(void)
{
    /* A media_player can carry kilobytes of attributes. The store must
     * refuse the overflow rather than grow without limit. */
    ha_store   s;
    ha_entity *e;
    char       key[32];
    int        i, refused = 0;

    CHECK(ha_store_init(&s, 4));
    e = ha_store_put(&s, "media_player.big");

    for (i = 0; i < 100; i++) {
        sprintf(key, "attr_%02d", i);
        if (!ha_entity_set_attr(e, key, "some value here"))
            refused++;
    }
    CHECK(refused > 0);                       /* it did hit the limit  */
    CHECK(e->attrs_len < sizeof e->attrs);    /* and stayed in bounds  */

    /* Whatever did fit must still be readable and intact. */
    {
        const char *k, *v;
        int         n = 0;
        for (k = ha_entity_attr_next(e, NULL, &v); k;
             k = ha_entity_attr_next(e, k, &v)) {
            CHECK_STR(v, "some value here");
            n++;
        }
        CHECK(n > 0);
    }

    ha_store_free(&s);
}

static void test_attr_iteration(void)
{
    ha_store    s;
    ha_entity  *e;
    const char *k, *v;
    int         n = 0;

    CHECK(ha_store_init(&s, 4));
    e = ha_store_put(&s, "climate.hall");

    CHECK(ha_entity_attr_next(e, NULL, &v) == NULL); /* empty */

    ha_entity_set_attr(e, "temperature", "21");
    ha_entity_set_attr(e, "hvac_action", "heating");

    for (k = ha_entity_attr_next(e, NULL, &v); k;
         k = ha_entity_attr_next(e, k, &v)) {
        if (strcmp(k, "temperature") == 0)      CHECK_STR(v, "21");
        else if (strcmp(k, "hvac_action") == 0) CHECK_STR(v, "heating");
        else                                    CHECK(0);
        n++;
    }
    CHECK_INT(n, 2);

    ha_store_free(&s);
}

static void test_clear_reuses_buckets(void)
{
    ha_store s;
    size_t   nb;

    CHECK(ha_store_init(&s, 32));
    ha_store_put(&s, "a.one");
    ha_store_put(&s, "b.two");
    nb = s.nbuckets;

    ha_store_clear(&s);
    CHECK_INT(ha_store_count(&s), 0);
    CHECK(ha_store_first(&s) == NULL);
    CHECK_INT(s.nbuckets, nb); /* kept for reuse across reconnects */

    /* Still usable afterwards. */
    CHECK(ha_store_put(&s, "c.three") != NULL);
    CHECK_INT(ha_store_count(&s), 1);

    ha_store_free(&s);
}

void suite_entity(void)
{
    RUN(test_put_get);
    RUN(test_many_entities_and_collisions);
    RUN(test_iteration_order_is_stable);
    RUN(test_remove);
    RUN(test_truncation);
    RUN(test_domain);
    RUN(test_change_tracking);
    RUN(test_attributes);
    RUN(test_attribute_blob_is_bounded);
    RUN(test_attr_iteration);
    RUN(test_clear_reuses_buckets);
}
