/*
 * test_resolve.c — Tests for YAML alias resolution
 */

#include "yam/yam.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

/* ── Helpers ─────────────────────────────────────────────── */

typedef struct {
    yam_event events[512];
    int       len;
} event_list;

static event_list parse_with(const char *yaml, bool resolve, bool merge) {
    event_list el = {.len = 0};
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);
    if (resolve) yam_parser_set_resolve(p, true);
    if (merge) yam_parser_set_merge(p, true);

    yam_event evt;
    while (el.len < 512 && yam_parse_next(p, &evt) == YAM_OK) {
        el.events[el.len++] = evt;
        if (evt.type == YAM_EVT_STREAM_END) break;
    }

    yam_parser_free(p);
    yam_arena_free(a);
    return el;
}

static bool has_scalar(const event_list *el, const char *val) {
    size_t vlen = strlen(val);
    for (int i = 0; i < el->len; i++)
        if (el->events[i].type == YAM_EVT_SCALAR &&
            el->events[i].value.len == vlen &&
            memcmp(el->events[i].value.data, val, vlen) == 0)
            return true;
    return false;
}

static bool has_alias(const event_list *el) {
    for (int i = 0; i < el->len; i++)
        if (el->events[i].type == YAM_EVT_ALIAS)
            return true;
    return false;
}

static int count_event_type(const event_list *el, yam_event_type t) {
    int count = 0;
    for (int i = 0; i < el->len; i++)
        if (el->events[i].type == t) count++;
    return count;
}

static int count_scalar(const event_list *el, const char *val) {
    size_t vlen = strlen(val);
    int count = 0;
    for (int i = 0; i < el->len; i++)
        if (el->events[i].type == YAM_EVT_SCALAR &&
            el->events[i].value.len == vlen &&
            memcmp(el->events[i].value.data, val, vlen) == 0)
            count++;
    return count;
}

/* ── Test: Scalar alias ──────────────────────────────────── */

static void test_scalar_alias(void) {
    printf("test_scalar_alias:\n");
    const char *yaml =
        "a: &x hello\n"
        "b: *x\n";

    event_list el = parse_with(yaml, true, false);

    /* *x should be expanded to "hello" */
    ASSERT(!has_alias(&el), "no ALIAS events remain");
    /* "hello" should appear twice: once as &x value, once as *x expansion */
    ASSERT(count_scalar(&el, "hello") == 2, "hello appears twice");
}

/* ── Test: Mapping alias ─────────────────────────────────── */

static void test_mapping_alias(void) {
    printf("test_mapping_alias:\n");
    const char *yaml =
        "orig: &m\n"
        "  a: 1\n"
        "copy: *m\n";

    event_list el = parse_with(yaml, true, false);

    ASSERT(!has_alias(&el), "no ALIAS events");
    /* MAPPING_START should appear 3 times: outer, orig's value, copy's expansion */
    ASSERT(count_event_type(&el, YAM_EVT_MAPPING_START) == 3,
           "3 mapping starts (outer + orig + copy expansion)");
    /* "a" and "1" should each appear twice */
    ASSERT(count_scalar(&el, "a") == 2, "'a' appears twice");
    ASSERT(count_scalar(&el, "1") == 2, "'1' appears twice");
}

/* ── Test: Sequence alias ────────────────────────────────── */

static void test_sequence_alias(void) {
    printf("test_sequence_alias:\n");
    const char *yaml =
        "orig: &s\n"
        "  - one\n"
        "  - two\n"
        "copy: *s\n";

    event_list el = parse_with(yaml, true, false);

    ASSERT(!has_alias(&el), "no ALIAS events");
    ASSERT(count_event_type(&el, YAM_EVT_SEQUENCE_START) == 2,
           "2 sequence starts (orig + copy)");
    ASSERT(count_scalar(&el, "one") == 2, "'one' twice");
    ASSERT(count_scalar(&el, "two") == 2, "'two' twice");
}

/* ── Test: Anchor stripped from copies ───────────────────── */

static void test_anchor_stripped(void) {
    printf("test_anchor_stripped:\n");
    const char *yaml =
        "a: &x hello\n"
        "b: *x\n";

    event_list el = parse_with(yaml, true, false);

    /* count how many events have anchor "x" */
    int anchor_count = 0;
    for (int i = 0; i < el.len; i++) {
        if (el.events[i].anchor.data &&
            el.events[i].anchor.len == 1 &&
            el.events[i].anchor.data[0] == 'x')
            anchor_count++;
    }
    ASSERT(anchor_count == 1, "anchor 'x' appears only once (on original)");
}

/* ── Test: Unknown anchor ────────────────────────────────── */

static void test_unknown_anchor(void) {
    printf("test_unknown_anchor:\n");
    const char *yaml =
        "a: *missing\n";

    event_list el = parse_with(yaml, true, false);

    /* unknown alias should remain as ALIAS event */
    ASSERT(has_alias(&el), "unknown alias kept as ALIAS event");
}

/* ── Test: Nested alias (alias target contains another alias) ─ */

static void test_nested_alias(void) {
    printf("test_nested_alias:\n");
    const char *yaml =
        "x: &a hello\n"
        "y: &b\n"
        "  inner: *a\n"
        "z: *b\n";

    event_list el = parse_with(yaml, true, false);

    /* *b expands to {inner: *a}, then *a expands to "hello" */
    ASSERT(!has_alias(&el), "all aliases resolved");
    /* "hello" should appear 3 times: original, inside &b, inside *b expansion */
    ASSERT(count_scalar(&el, "hello") == 3, "'hello' appears 3 times");
}

/* ── Test: Opt-in (disabled) ─────────────────────────────── */

static void test_opt_in(void) {
    printf("test_opt_in:\n");
    const char *yaml =
        "a: &x hello\n"
        "b: *x\n";

    event_list el = parse_with(yaml, false, false);

    ASSERT(has_alias(&el), "ALIAS preserved when resolve disabled");
}

/* ── Test: Merge + resolve both enabled ──────────────────── */

static void test_merge_and_resolve(void) {
    printf("test_merge_and_resolve:\n");
    const char *yaml =
        "defaults: &defaults\n"
        "  adapter: postgres\n"
        "val: &v hello\n"
        "result:\n"
        "  <<: *defaults\n"
        "  ref: *v\n";

    event_list el = parse_with(yaml, true, true);

    /* merge should expand <<: *defaults */
    ASSERT(has_scalar(&el, "adapter"), "merged 'adapter'");
    ASSERT(has_scalar(&el, "postgres"), "merged 'postgres'");

    /* resolve should expand *v */
    ASSERT(!has_alias(&el), "no alias events remain");
    ASSERT(count_scalar(&el, "hello") >= 2, "'hello' expanded");
}

/* ── Test: Multiple references to same anchor ────────────── */

static void test_multiple_references(void) {
    printf("test_multiple_references:\n");
    const char *yaml =
        "val: &x hello\n"
        "a: *x\n"
        "b: *x\n"
        "c: *x\n";

    event_list el = parse_with(yaml, true, false);

    ASSERT(!has_alias(&el), "all aliases resolved");
    /* "hello" should appear 4 times: original + 3 expansions */
    ASSERT(count_scalar(&el, "hello") == 4, "'hello' appears 4 times");
}

/* ── Test: Alias in mapping value ────────────────────────── */

static void test_alias_in_map_value(void) {
    printf("test_alias_in_map_value:\n");
    const char *yaml =
        "name: &n Alice\n"
        "greeting: *n\n";

    event_list el = parse_with(yaml, true, false);

    ASSERT(!has_alias(&el), "alias resolved");
    ASSERT(count_scalar(&el, "Alice") == 2, "'Alice' appears twice");
}

/* ── Test: Alias in sequence ─────────────────────────────── */

static void test_alias_in_seq(void) {
    printf("test_alias_in_seq:\n");
    const char *yaml =
        "val: &x 42\n"
        "items:\n"
        "  - *x\n"
        "  - *x\n";

    event_list el = parse_with(yaml, true, false);

    ASSERT(!has_alias(&el), "aliases resolved");
    ASSERT(count_scalar(&el, "42") == 3, "'42' appears 3 times");
}

/* ── Test: Circular reference ────────────────────────────── */

static void test_circular(void) {
    printf("test_circular:\n");
    /* This creates a circular reference:
     * &a's mapping contains *b, and &b's mapping contains *a */
    const char *yaml =
        "a: &a\n"
        "  ref: *b\n"
        "b: &b\n"
        "  ref: *a\n";

    event_list el = parse_with(yaml, true, false);

    /* should not hang; circular aliases remain as ALIAS events */
    ASSERT(has_alias(&el), "circular aliases kept as ALIAS events");
    /* should still have the scalar keys */
    ASSERT(has_scalar(&el, "a"), "has 'a'");
    ASSERT(has_scalar(&el, "b"), "has 'b'");
}

/* ── Test: Self-referential anchor ───────────────────────── */

static void test_self_reference(void) {
    printf("test_self_reference:\n");
    const char *yaml =
        "a: &a\n"
        "  self: *a\n";

    event_list el = parse_with(yaml, true, false);

    /* self-reference is circular — alias should remain */
    ASSERT(has_alias(&el), "self-referential alias kept");
}

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
    test_scalar_alias();
    test_mapping_alias();
    test_sequence_alias();
    test_anchor_stripped();
    test_unknown_anchor();
    test_nested_alias();
    test_opt_in();
    test_merge_and_resolve();
    test_multiple_references();
    test_alias_in_map_value();
    test_alias_in_seq();
    test_circular();
    test_self_reference();

    printf("\n--- Resolve tests: %d / %d passed ---\n", tests_passed, tests_run);
    if (tests_failed > 0) printf("    %d FAILED\n", tests_failed);
    return (tests_passed == tests_run) ? 0 : 1;
}
