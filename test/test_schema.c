/*
 * test_schema.c — Tests for YAML 1.2 tag schema resolution
 */

#include "yam/yam.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while(0)

static bool str_eq(yam_str a, yam_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

/* ── Helper: resolve a plain scalar against a schema ─────── */

static yam_str resolve_plain(const yam_schema *schema, const char *val) {
    yam_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = YAM_EVT_SCALAR;
    evt.scalar_style = YAM_SCALAR_PLAIN;
    evt.value.data = val;
    evt.value.len = strlen(val);
    return yam_schema_resolve(schema, &evt);
}

static yam_str resolve_quoted(const yam_schema *schema, const char *val) {
    yam_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = YAM_EVT_SCALAR;
    evt.scalar_style = YAM_SCALAR_DOUBLE_QUOTED;
    evt.value.data = val;
    evt.value.len = strlen(val);
    return yam_schema_resolve(schema, &evt);
}

/* ── Test: Failsafe schema ───────────────────────────────── */

static void test_failsafe(void) {
    printf("test_failsafe:\n");
    yam_schema s = yam_schema_failsafe();

    /* everything is str in failsafe */
    ASSERT(str_eq(resolve_plain(&s, "null"), YAM_TAG_STR), "null → str");
    ASSERT(str_eq(resolve_plain(&s, "true"), YAM_TAG_STR), "true → str");
    ASSERT(str_eq(resolve_plain(&s, "42"), YAM_TAG_STR), "42 → str");
    ASSERT(str_eq(resolve_plain(&s, "hello"), YAM_TAG_STR), "hello → str");
    ASSERT(str_eq(resolve_plain(&s, ""), YAM_TAG_STR), "empty → str");
    ASSERT(str_eq(resolve_quoted(&s, "null"), YAM_TAG_STR), "quoted null → str");
}

/* ── Test: JSON schema ───────────────────────────────────── */

static void test_json(void) {
    printf("test_json:\n");
    yam_schema s = yam_schema_json();

    /* null */
    ASSERT(str_eq(resolve_plain(&s, "null"), YAM_TAG_NULL), "null → null");
    ASSERT(str_eq(resolve_plain(&s, "Null"), YAM_TAG_STR), "Null → str (JSON is case-sensitive)");
    ASSERT(str_eq(resolve_plain(&s, "NULL"), YAM_TAG_STR), "NULL → str");
    ASSERT(str_eq(resolve_plain(&s, "~"), YAM_TAG_STR), "~ → str");

    /* bool */
    ASSERT(str_eq(resolve_plain(&s, "true"), YAM_TAG_BOOL), "true → bool");
    ASSERT(str_eq(resolve_plain(&s, "false"), YAM_TAG_BOOL), "false → bool");
    ASSERT(str_eq(resolve_plain(&s, "True"), YAM_TAG_STR), "True → str");
    ASSERT(str_eq(resolve_plain(&s, "FALSE"), YAM_TAG_STR), "FALSE → str");

    /* int */
    ASSERT(str_eq(resolve_plain(&s, "42"), YAM_TAG_INT), "42 → int");
    ASSERT(str_eq(resolve_plain(&s, "-1"), YAM_TAG_INT), "-1 → int");
    ASSERT(str_eq(resolve_plain(&s, "0"), YAM_TAG_INT), "0 → int");
    ASSERT(str_eq(resolve_plain(&s, "0x1F"), YAM_TAG_INT), "0x1F → int");
    ASSERT(str_eq(resolve_plain(&s, "0o17"), YAM_TAG_INT), "0o17 → int");

    /* float */
    ASSERT(str_eq(resolve_plain(&s, "3.14"), YAM_TAG_FLOAT), "3.14 → float");
    ASSERT(str_eq(resolve_plain(&s, ".inf"), YAM_TAG_FLOAT), ".inf → float");
    ASSERT(str_eq(resolve_plain(&s, "-.inf"), YAM_TAG_FLOAT), "-.inf → float");
    ASSERT(str_eq(resolve_plain(&s, ".nan"), YAM_TAG_FLOAT), ".nan → float");
    ASSERT(str_eq(resolve_plain(&s, "1e10"), YAM_TAG_FLOAT), "1e10 → float");
    ASSERT(str_eq(resolve_plain(&s, "1.5E-3"), YAM_TAG_FLOAT), "1.5E-3 → float");

    /* string fallback */
    ASSERT(str_eq(resolve_plain(&s, "hello"), YAM_TAG_STR), "hello → str");

    /* quoted always str */
    ASSERT(str_eq(resolve_quoted(&s, "true"), YAM_TAG_STR), "quoted true → str");
    ASSERT(str_eq(resolve_quoted(&s, "42"), YAM_TAG_STR), "quoted 42 → str");
}

/* ── Test: Core schema ───────────────────────────────────── */

static void test_core(void) {
    printf("test_core:\n");
    yam_schema s = yam_schema_core();

    /* null variants */
    ASSERT(str_eq(resolve_plain(&s, "null"), YAM_TAG_NULL), "null → null");
    ASSERT(str_eq(resolve_plain(&s, "Null"), YAM_TAG_NULL), "Null → null");
    ASSERT(str_eq(resolve_plain(&s, "NULL"), YAM_TAG_NULL), "NULL → null");
    ASSERT(str_eq(resolve_plain(&s, "~"), YAM_TAG_NULL), "~ → null");
    ASSERT(str_eq(resolve_plain(&s, ""), YAM_TAG_NULL), "empty → null");

    /* bool variants */
    ASSERT(str_eq(resolve_plain(&s, "true"), YAM_TAG_BOOL), "true → bool");
    ASSERT(str_eq(resolve_plain(&s, "True"), YAM_TAG_BOOL), "True → bool");
    ASSERT(str_eq(resolve_plain(&s, "TRUE"), YAM_TAG_BOOL), "TRUE → bool");
    ASSERT(str_eq(resolve_plain(&s, "false"), YAM_TAG_BOOL), "false → bool");
    ASSERT(str_eq(resolve_plain(&s, "False"), YAM_TAG_BOOL), "False → bool");
    ASSERT(str_eq(resolve_plain(&s, "FALSE"), YAM_TAG_BOOL), "FALSE → bool");

    /* not booleans in core */
    ASSERT(str_eq(resolve_plain(&s, "yes"), YAM_TAG_STR), "yes → str (not core bool)");
    ASSERT(str_eq(resolve_plain(&s, "on"), YAM_TAG_STR), "on → str (not core bool)");

    /* int */
    ASSERT(str_eq(resolve_plain(&s, "42"), YAM_TAG_INT), "42 → int");
    ASSERT(str_eq(resolve_plain(&s, "+42"), YAM_TAG_INT), "+42 → int");
    ASSERT(str_eq(resolve_plain(&s, "-42"), YAM_TAG_INT), "-42 → int");
    ASSERT(str_eq(resolve_plain(&s, "0x2A"), YAM_TAG_INT), "0x2A → int");
    ASSERT(str_eq(resolve_plain(&s, "0o52"), YAM_TAG_INT), "0o52 → int");

    /* float */
    ASSERT(str_eq(resolve_plain(&s, "1.0"), YAM_TAG_FLOAT), "1.0 → float");
    ASSERT(str_eq(resolve_plain(&s, ".5"), YAM_TAG_FLOAT), ".5 → float");
    ASSERT(str_eq(resolve_plain(&s, "+.inf"), YAM_TAG_FLOAT), "+.inf → float");
    ASSERT(str_eq(resolve_plain(&s, ".nan"), YAM_TAG_FLOAT), ".nan → float");

    /* string */
    ASSERT(str_eq(resolve_plain(&s, "hello"), YAM_TAG_STR), "hello → str");

    /* quoted */
    ASSERT(str_eq(resolve_quoted(&s, "null"), YAM_TAG_STR), "quoted null → str");
    ASSERT(str_eq(resolve_quoted(&s, "42"), YAM_TAG_STR), "quoted 42 → str");
}

/* ── Test: Int matcher edge cases ────────────────────────── */

static void test_int_matcher(void) {
    printf("test_int_matcher:\n");
    yam_schema s = yam_schema_core();

    /* valid */
    ASSERT(str_eq(resolve_plain(&s, "0"), YAM_TAG_INT), "0");
    ASSERT(str_eq(resolve_plain(&s, "123456789"), YAM_TAG_INT), "123456789");
    ASSERT(str_eq(resolve_plain(&s, "0xDEAD"), YAM_TAG_INT), "0xDEAD");
    ASSERT(str_eq(resolve_plain(&s, "0o777"), YAM_TAG_INT), "0o777");
    ASSERT(str_eq(resolve_plain(&s, "-0"), YAM_TAG_INT), "-0");

    /* invalid — these should be str */
    ASSERT(str_eq(resolve_plain(&s, "0x"), YAM_TAG_STR), "0x alone");
    ASSERT(str_eq(resolve_plain(&s, "0o"), YAM_TAG_STR), "0o alone");
    ASSERT(str_eq(resolve_plain(&s, "0o8"), YAM_TAG_STR), "0o8 (invalid octal)");
    ASSERT(str_eq(resolve_plain(&s, "0xGG"), YAM_TAG_STR), "0xGG (invalid hex)");
    ASSERT(str_eq(resolve_plain(&s, "+"), YAM_TAG_STR), "lone +");
    ASSERT(str_eq(resolve_plain(&s, "-"), YAM_TAG_STR), "lone -");
    ASSERT(str_eq(resolve_plain(&s, "12a"), YAM_TAG_STR), "12a");
}

/* ── Test: Float matcher edge cases ──────────────────────── */

static void test_float_matcher(void) {
    printf("test_float_matcher:\n");
    yam_schema s = yam_schema_core();

    /* valid floats */
    ASSERT(str_eq(resolve_plain(&s, "1.0"), YAM_TAG_FLOAT), "1.0");
    ASSERT(str_eq(resolve_plain(&s, ".5"), YAM_TAG_FLOAT), ".5");
    ASSERT(str_eq(resolve_plain(&s, "1."), YAM_TAG_FLOAT), "1.");
    ASSERT(str_eq(resolve_plain(&s, "1e5"), YAM_TAG_FLOAT), "1e5");
    ASSERT(str_eq(resolve_plain(&s, "1E+5"), YAM_TAG_FLOAT), "1E+5");
    ASSERT(str_eq(resolve_plain(&s, "-1.5e-3"), YAM_TAG_FLOAT), "-1.5e-3");
    ASSERT(str_eq(resolve_plain(&s, ".inf"), YAM_TAG_FLOAT), ".inf");
    ASSERT(str_eq(resolve_plain(&s, "+.inf"), YAM_TAG_FLOAT), "+.inf");
    ASSERT(str_eq(resolve_plain(&s, "-.inf"), YAM_TAG_FLOAT), "-.inf");
    ASSERT(str_eq(resolve_plain(&s, ".nan"), YAM_TAG_FLOAT), ".nan");

    /* not floats — int takes precedence for pure digits */
    ASSERT(str_eq(resolve_plain(&s, "42"), YAM_TAG_INT), "42 is int not float");

    /* not floats — invalid patterns */
    ASSERT(str_eq(resolve_plain(&s, "."), YAM_TAG_STR), "lone dot");
    ASSERT(str_eq(resolve_plain(&s, "e5"), YAM_TAG_STR), "e5 (no digits)");
    ASSERT(str_eq(resolve_plain(&s, "1e"), YAM_TAG_STR), "1e (no exponent)");
    ASSERT(str_eq(resolve_plain(&s, "inf"), YAM_TAG_STR), "inf (no dot)");
    ASSERT(str_eq(resolve_plain(&s, "nan"), YAM_TAG_STR), "nan (no dot)");
}

/* ── Test: Schema builder ────────────────────────────────── */

static void test_builder(void) {
    printf("test_builder:\n");
    yam_arena *a = yam_arena_new(4096);

    /* Build a YAML 1.1 compatible schema with on/off/yes/no */
    yam_schema_builder *b = yam_schema_builder_new(a);
    ASSERT(b != NULL, "builder created");

    const char *trues[]  = { "true", "True", "TRUE", "yes", "Yes", "YES", "on", "On", "ON" };
    const char *falses[] = { "false", "False", "FALSE", "no", "No", "NO", "off", "Off", "OFF" };
    yam_schema_builder_add_bools(b, trues, 9, falses, 9);

    const char *nulls[] = { "null", "Null", "NULL", "~", "" };
    yam_schema_builder_add_nulls(b, nulls, 5);

    yam_schema_builder_add_int(b);
    yam_schema_builder_add_float(b);

    yam_schema s = yam_schema_builder_finish(b);
    yam_schema_builder_free(b);

    /* test 1.1 booleans */
    ASSERT(str_eq(resolve_plain(&s, "yes"), YAM_TAG_BOOL), "yes → bool");
    ASSERT(str_eq(resolve_plain(&s, "Yes"), YAM_TAG_BOOL), "Yes → bool");
    ASSERT(str_eq(resolve_plain(&s, "YES"), YAM_TAG_BOOL), "YES → bool");
    ASSERT(str_eq(resolve_plain(&s, "no"), YAM_TAG_BOOL), "no → bool");
    ASSERT(str_eq(resolve_plain(&s, "No"), YAM_TAG_BOOL), "No → bool");
    ASSERT(str_eq(resolve_plain(&s, "NO"), YAM_TAG_BOOL), "NO → bool");
    ASSERT(str_eq(resolve_plain(&s, "on"), YAM_TAG_BOOL), "on → bool");
    ASSERT(str_eq(resolve_plain(&s, "On"), YAM_TAG_BOOL), "On → bool");
    ASSERT(str_eq(resolve_plain(&s, "ON"), YAM_TAG_BOOL), "ON → bool");
    ASSERT(str_eq(resolve_plain(&s, "off"), YAM_TAG_BOOL), "off → bool");
    ASSERT(str_eq(resolve_plain(&s, "Off"), YAM_TAG_BOOL), "Off → bool");
    ASSERT(str_eq(resolve_plain(&s, "OFF"), YAM_TAG_BOOL), "OFF → bool");
    ASSERT(str_eq(resolve_plain(&s, "true"), YAM_TAG_BOOL), "true → bool");
    ASSERT(str_eq(resolve_plain(&s, "false"), YAM_TAG_BOOL), "false → bool");

    /* nulls */
    ASSERT(str_eq(resolve_plain(&s, "null"), YAM_TAG_NULL), "null → null");
    ASSERT(str_eq(resolve_plain(&s, "~"), YAM_TAG_NULL), "~ → null");
    ASSERT(str_eq(resolve_plain(&s, ""), YAM_TAG_NULL), "empty → null");

    /* int/float still work */
    ASSERT(str_eq(resolve_plain(&s, "42"), YAM_TAG_INT), "42 → int");
    ASSERT(str_eq(resolve_plain(&s, "3.14"), YAM_TAG_FLOAT), "3.14 → float");

    /* quoted always str */
    ASSERT(str_eq(resolve_quoted(&s, "yes"), YAM_TAG_STR), "quoted yes → str");

    /* unrecognized → str */
    ASSERT(str_eq(resolve_plain(&s, "hello"), YAM_TAG_STR), "hello → str");

    yam_arena_free(a);
}

/* ── Test: Parser integration ────────────────────────────── */

static void test_parser_integration(void) {
    printf("test_parser_integration:\n");

    const char *yaml = "name: Alice\nage: 30\nactive: true\nitems:\n  - one\n  - 2\n";
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);

    yam_schema core = yam_schema_core();
    yam_parser_set_schema(p, &core);

    yam_event evt;
    bool found_name_val = false;
    bool found_age_val = false;
    bool found_active_val = false;
    bool found_seq = false;
    bool found_map = false;
    bool found_item_one = false;
    bool found_item_two = false;
    bool prev_was_name = false;
    bool prev_was_age = false;
    bool prev_was_active = false;

    while (yam_parse_next(p, &evt) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END) break;

        if (evt.type == YAM_EVT_MAPPING_START && !found_map) {
            ASSERT(str_eq(evt.tag, YAM_TAG_MAP), "mapping gets map tag");
            found_map = true;
        }
        if (evt.type == YAM_EVT_SEQUENCE_START && !found_seq) {
            ASSERT(str_eq(evt.tag, YAM_TAG_SEQ), "sequence gets seq tag");
            found_seq = true;
        }
        if (evt.type == YAM_EVT_SCALAR) {
            if (prev_was_name) {
                ASSERT(str_eq(evt.tag, YAM_TAG_STR), "Alice → str");
                found_name_val = true;
                prev_was_name = false;
            } else if (prev_was_age) {
                ASSERT(str_eq(evt.tag, YAM_TAG_INT), "30 → int");
                found_age_val = true;
                prev_was_age = false;
            } else if (prev_was_active) {
                ASSERT(str_eq(evt.tag, YAM_TAG_BOOL), "true → bool");
                found_active_val = true;
                prev_was_active = false;
            } else if (evt.value.len == 3 && memcmp(evt.value.data, "one", 3) == 0) {
                ASSERT(str_eq(evt.tag, YAM_TAG_STR), "one → str");
                found_item_one = true;
            } else if (evt.value.len == 1 && evt.value.data[0] == '2') {
                ASSERT(str_eq(evt.tag, YAM_TAG_INT), "2 → int");
                found_item_two = true;
            }

            if (evt.value.len == 4 && memcmp(evt.value.data, "name", 4) == 0) prev_was_name = true;
            if (evt.value.len == 3 && memcmp(evt.value.data, "age", 3) == 0) prev_was_age = true;
            if (evt.value.len == 6 && memcmp(evt.value.data, "active", 6) == 0) prev_was_active = true;
        }
    }

    ASSERT(found_name_val, "found name value");
    ASSERT(found_age_val, "found age value");
    ASSERT(found_active_val, "found active value");
    ASSERT(found_seq, "found sequence");
    ASSERT(found_map, "found mapping");
    ASSERT(found_item_one, "found item one");
    ASSERT(found_item_two, "found item two");

    yam_parser_free(p);
    yam_arena_free(a);
}

/* ── Test: No schema = no tags ───────────────────────────── */

static void test_no_schema(void) {
    printf("test_no_schema:\n");

    const char *yaml = "key: true\n";
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);
    /* no yam_parser_set_schema call */

    yam_event evt;
    bool found_true = false;
    while (yam_parse_next(p, &evt) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END) break;
        if (evt.type == YAM_EVT_SCALAR && evt.value.len == 4 &&
            memcmp(evt.value.data, "true", 4) == 0) {
            ASSERT(evt.tag.data == NULL, "no schema → no tag on true");
            found_true = true;
        }
    }
    ASSERT(found_true, "found true scalar");

    yam_parser_free(p);
    yam_arena_free(a);
}

/* ── Test: Explicit tag not overwritten ──────────────────── */

static void test_explicit_tag(void) {
    printf("test_explicit_tag:\n");

    const char *yaml = "!!str true\n";
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);

    yam_schema core = yam_schema_core();
    yam_parser_set_schema(p, &core);

    yam_event evt;
    bool found = false;
    while (yam_parse_next(p, &evt) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END) break;
        if (evt.type == YAM_EVT_SCALAR && evt.value.len == 4 &&
            memcmp(evt.value.data, "true", 4) == 0) {
            /* explicit !!str should NOT be overwritten to bool */
            ASSERT(evt.tag.data != NULL, "has explicit tag");
            ASSERT(!str_eq(evt.tag, YAM_TAG_BOOL), "explicit !!str not turned into bool");
            found = true;
        }
    }
    ASSERT(found, "found true scalar");

    yam_parser_free(p);
    yam_arena_free(a);
}

/* ── Test: icase match ───────────────────────────────────── */

static void test_icase(void) {
    printf("test_icase:\n");
    yam_arena *a = yam_arena_new(4096);

    yam_schema_builder *b = yam_schema_builder_new(a);
    yam_schema_builder_add(b, YAM_MATCH_ICASE, "yes", YAM_TAG_BOOL);
    yam_schema_builder_add(b, YAM_MATCH_ICASE, "no", YAM_TAG_BOOL);
    yam_schema s = yam_schema_builder_finish(b);
    yam_schema_builder_free(b);

    ASSERT(str_eq(resolve_plain(&s, "yes"), YAM_TAG_BOOL), "yes → bool");
    ASSERT(str_eq(resolve_plain(&s, "YES"), YAM_TAG_BOOL), "YES → bool");
    ASSERT(str_eq(resolve_plain(&s, "yEs"), YAM_TAG_BOOL), "yEs → bool");
    ASSERT(str_eq(resolve_plain(&s, "No"), YAM_TAG_BOOL), "No → bool");
    ASSERT(str_eq(resolve_plain(&s, "nope"), YAM_TAG_STR), "nope → str");

    yam_arena_free(a);
}

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
    test_failsafe();
    test_json();
    test_core();
    test_int_matcher();
    test_float_matcher();
    test_builder();
    test_parser_integration();
    test_no_schema();
    test_explicit_tag();
    test_icase();

    printf("\n─── Schema tests: %d / %d passed ───\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
