/*
 * test_merge.c — Tests for YAML merge key (<<) resolution
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

static event_list parse_yaml(const char *yaml, bool merge) {
    event_list el = {.len = 0};
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);
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

static bool has_event_type(const event_list *el, yam_event_type t) {
    for (int i = 0; i < el->len; i++)
        if (el->events[i].type == t) return true;
    return false;
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

/* Find scalar key in a mapping, return the index of its value event.
 * Searches after the given start index. Returns -1 if not found. */
static int find_key_value(const event_list *el, int start, const char *key) {
    size_t klen = strlen(key);
    int depth = 0;
    for (int i = start; i < el->len; i++) {
        yam_event_type t = el->events[i].type;
        if (t == YAM_EVT_MAPPING_START || t == YAM_EVT_SEQUENCE_START)
            depth++;
        else if (t == YAM_EVT_MAPPING_END || t == YAM_EVT_SEQUENCE_END)
            depth--;

        /* only look at top-level keys in the mapping at depth 1 */
        if (depth == 1 && t == YAM_EVT_SCALAR &&
            el->events[i].value.len == klen &&
            memcmp(el->events[i].value.data, key, klen) == 0) {
            return i + 1; /* the value follows the key */
        }
    }
    return -1;
}

/* Count scalar events with a given value */
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

/* ── Test: Basic merge ───────────────────────────────────── */

static void test_basic_merge(void) {
    printf("test_basic_merge:\n");
    const char *yaml =
        "defaults: &defaults\n"
        "  adapter: postgres\n"
        "  host: localhost\n"
        "result:\n"
        "  <<: *defaults\n"
        "  database: mydb\n";

    event_list el = parse_yaml(yaml, true);

    /* merged keys should be present */
    ASSERT(has_scalar(&el, "adapter"), "merged key 'adapter'");
    ASSERT(has_scalar(&el, "postgres"), "merged value 'postgres'");
    ASSERT(has_scalar(&el, "host"), "merged key 'host'");
    ASSERT(has_scalar(&el, "localhost"), "merged value 'localhost'");
    ASSERT(has_scalar(&el, "database"), "explicit key 'database'");
    ASSERT(has_scalar(&el, "mydb"), "explicit value 'mydb'");

    /* << key and ALIAS events should be gone from the result mapping */
    /* (the source mapping still has its keys normally) */
    /* count << appearances — should be 0 */
    bool found_merge_in_result = false;
    int depth = 0;
    bool in_result = false;
    for (int i = 0; i < el.len; i++) {
        if (el.events[i].type == YAM_EVT_SCALAR &&
            el.events[i].value.len == 6 &&
            memcmp(el.events[i].value.data, "result", 6) == 0) {
            in_result = true;
            depth = 0;
            continue;
        }
        if (in_result) {
            if (el.events[i].type == YAM_EVT_MAPPING_START) depth++;
            if (el.events[i].type == YAM_EVT_MAPPING_END) {
                depth--;
                if (depth == 0) in_result = false;
            }
            if (depth == 1 && el.events[i].type == YAM_EVT_SCALAR &&
                el.events[i].value.len == 2 &&
                memcmp(el.events[i].value.data, "<<", 2) == 0)
                found_merge_in_result = true;
        }
    }
    ASSERT(!found_merge_in_result, "<< key removed from result mapping");
}

/* ── Test: Override ──────────────────────────────────────── */

static void test_override(void) {
    printf("test_override:\n");
    const char *yaml =
        "base: &base\n"
        "  x: 1\n"
        "  y: 2\n"
        "child:\n"
        "  <<: *base\n"
        "  y: 99\n";

    event_list el = parse_yaml(yaml, true);

    /* find the child mapping */
    int child_val = find_key_value(&el, 0, "child");
    ASSERT(child_val > 0, "found 'child' key");

    /* y should appear only once in the child mapping (the override) */
    int y_count = 0;
    int depth = 0;
    for (int i = child_val; i < el.len; i++) {
        if (el.events[i].type == YAM_EVT_MAPPING_START) depth++;
        if (el.events[i].type == YAM_EVT_MAPPING_END) {
            depth--;
            if (depth == 0) break;
        }
        if (depth == 1 && el.events[i].type == YAM_EVT_SCALAR &&
            el.events[i].value.len == 1 && el.events[i].value.data[0] == 'y')
            y_count++;
    }
    ASSERT(y_count == 1, "overridden key 'y' appears once");

    /* verify the value is the override, not the base */
    int y_val = find_key_value(&el, child_val, "y");
    ASSERT(y_val > 0, "found 'y' in child");
    ASSERT(el.events[y_val].type == YAM_EVT_SCALAR, "y value is scalar");
    ASSERT(el.events[y_val].value.len == 2 &&
           memcmp(el.events[y_val].value.data, "99", 2) == 0,
           "y value is '99' (override)");
}

/* ── Test: Sequence merge ────────────────────────────────── */

static void test_sequence_merge(void) {
    printf("test_sequence_merge:\n");
    const char *yaml =
        "a: &a\n"
        "  x: 1\n"
        "b: &b\n"
        "  y: 2\n"
        "  x: 9\n"
        "result:\n"
        "  <<: [*a, *b]\n"
        "  z: 3\n";

    event_list el = parse_yaml(yaml, true);

    /* result should have: z=3 (explicit), x=1 (from *a, higher priority), y=2 (from *b) */
    int result_val = find_key_value(&el, 0, "result");
    ASSERT(result_val > 0, "found 'result'");

    int x_val = find_key_value(&el, result_val, "x");
    ASSERT(x_val > 0, "found 'x' in result");
    ASSERT(el.events[x_val].value.len == 1 &&
           el.events[x_val].value.data[0] == '1',
           "x=1 (from *a, higher priority)");

    int y_val = find_key_value(&el, result_val, "y");
    ASSERT(y_val > 0, "found 'y' in result");

    int z_val = find_key_value(&el, result_val, "z");
    ASSERT(z_val > 0, "found 'z' in result");
}

/* ── Test: Multiple << keys ──────────────────────────────── */

static void test_multiple_merge_keys(void) {
    printf("test_multiple_merge_keys:\n");
    const char *yaml =
        "a: &a\n"
        "  x: 1\n"
        "b: &b\n"
        "  y: 2\n"
        "  x: 9\n"
        "result:\n"
        "  <<: *a\n"
        "  <<: *b\n"
        "  z: 3\n";

    event_list el = parse_yaml(yaml, true);

    int result_val = find_key_value(&el, 0, "result");
    ASSERT(result_val > 0, "found 'result'");

    /* x should be 1 (from first <<, higher priority) */
    int x_val = find_key_value(&el, result_val, "x");
    ASSERT(x_val > 0, "found 'x'");
    ASSERT(el.events[x_val].value.len == 1 &&
           el.events[x_val].value.data[0] == '1',
           "x=1 (from first <<)");

    ASSERT(find_key_value(&el, result_val, "y") > 0, "found 'y' from second <<");
    ASSERT(find_key_value(&el, result_val, "z") > 0, "found 'z' explicit");
}

/* ── Test: Quoted << is not a merge key ──────────────────── */

static void test_quoted_not_merge(void) {
    printf("test_quoted_not_merge:\n");
    const char *yaml =
        "base: &base\n"
        "  x: 1\n"
        "result:\n"
        "  \"<<\": *base\n";

    event_list el = parse_yaml(yaml, true);

    /* quoted << should be treated as normal key, ALIAS should remain */
    bool found_alias = false;
    int result_val = find_key_value(&el, 0, "result");
    ASSERT(result_val > 0, "found 'result'");

    int depth = 0;
    for (int i = result_val; i < el.len; i++) {
        if (el.events[i].type == YAM_EVT_MAPPING_START) depth++;
        if (el.events[i].type == YAM_EVT_MAPPING_END) {
            depth--;
            if (depth == 0) break;
        }
        if (el.events[i].type == YAM_EVT_ALIAS) found_alias = true;
    }
    ASSERT(found_alias, "alias preserved for quoted <<");
}

/* ── Test: Nested merge ──────────────────────────────────── */

static void test_nested_merge(void) {
    printf("test_nested_merge:\n");
    const char *yaml =
        "a: &a\n"
        "  x: 1\n"
        "b: &b\n"
        "  <<: *a\n"
        "  y: 2\n"
        "c:\n"
        "  <<: *b\n"
        "  z: 3\n";

    event_list el = parse_yaml(yaml, true);

    /* c should have: z=3 (explicit), y=2 (from *b), x=1 (from *a via *b) */
    int c_val = find_key_value(&el, 0, "c");
    ASSERT(c_val > 0, "found 'c'");

    ASSERT(find_key_value(&el, c_val, "z") > 0, "c has 'z'");
    ASSERT(find_key_value(&el, c_val, "y") > 0, "c has 'y' (from *b)");
    ASSERT(find_key_value(&el, c_val, "x") > 0, "c has 'x' (from *a via *b)");
}

/* ── Test: Merge non-mapping (silently ignored) ──────────── */

static void test_merge_non_mapping(void) {
    printf("test_merge_non_mapping:\n");
    const char *yaml =
        "scalar: &s hello\n"
        "result:\n"
        "  <<: *s\n"
        "  x: 1\n";

    event_list el = parse_yaml(yaml, true);

    /* merge of non-mapping is silently ignored; result should just have x */
    int result_val = find_key_value(&el, 0, "result");
    ASSERT(result_val > 0, "found 'result'");
    ASSERT(find_key_value(&el, result_val, "x") > 0, "has explicit 'x'");

    /* the hello scalar should NOT be injected into result */
    int depth = 0;
    int scalar_count = 0;
    for (int i = result_val; i < el.len; i++) {
        if (el.events[i].type == YAM_EVT_MAPPING_START) depth++;
        if (el.events[i].type == YAM_EVT_MAPPING_END) {
            depth--;
            if (depth == 0) break;
        }
        if (depth == 1 && el.events[i].type == YAM_EVT_SCALAR)
            scalar_count++;
    }
    /* should be exactly 2 scalars: "x" and "1" */
    ASSERT(scalar_count == 2, "only explicit key-value in result");
}

/* ── Test: Opt-in behavior ───────────────────────────────── */

static void test_opt_in(void) {
    printf("test_opt_in:\n");
    const char *yaml =
        "base: &base\n"
        "  x: 1\n"
        "result:\n"
        "  <<: *base\n"
        "  y: 2\n";

    /* without merge enabled */
    event_list el = parse_yaml(yaml, false);

    /* << should still be present as a normal key */
    bool found_merge = false;
    bool found_alias = false;
    for (int i = 0; i < el.len; i++) {
        if (el.events[i].type == YAM_EVT_SCALAR &&
            el.events[i].value.len == 2 &&
            memcmp(el.events[i].value.data, "<<", 2) == 0)
            found_merge = true;
        if (el.events[i].type == YAM_EVT_ALIAS)
            found_alias = true;
    }
    ASSERT(found_merge, "<< present when merge disabled");
    ASSERT(found_alias, "ALIAS present when merge disabled");
}

/* ── Test: Deep values (merged mapping has collection values) ─ */

static void test_deep_values(void) {
    printf("test_deep_values:\n");
    const char *yaml =
        "base: &base\n"
        "  items:\n"
        "    - one\n"
        "    - two\n"
        "  name: Alice\n"
        "result:\n"
        "  <<: *base\n"
        "  extra: yes\n";

    event_list el = parse_yaml(yaml, true);

    int result_val = find_key_value(&el, 0, "result");
    ASSERT(result_val > 0, "found 'result'");

    ASSERT(find_key_value(&el, result_val, "items") > 0, "merged 'items'");
    ASSERT(find_key_value(&el, result_val, "name") > 0, "merged 'name'");
    ASSERT(find_key_value(&el, result_val, "extra") > 0, "explicit 'extra'");

    /* verify sequence was properly merged */
    ASSERT(has_scalar(&el, "one"), "has 'one'");
    ASSERT(has_scalar(&el, "two"), "has 'two'");
}

/* ── Test: Empty merged mapping ──────────────────────────── */

static void test_empty_merge(void) {
    printf("test_empty_merge:\n");
    const char *yaml =
        "empty: &empty {}\n"
        "result:\n"
        "  <<: *empty\n"
        "  x: 1\n";

    event_list el = parse_yaml(yaml, true);

    int result_val = find_key_value(&el, 0, "result");
    ASSERT(result_val > 0, "found 'result'");
    ASSERT(find_key_value(&el, result_val, "x") > 0, "has explicit 'x'");
}

/* ── Test: No ALIAS events in resolved output ────────────── */

static void test_no_alias_in_resolved(void) {
    printf("test_no_alias_in_resolved:\n");
    const char *yaml =
        "base: &base\n"
        "  x: 1\n"
        "result:\n"
        "  <<: *base\n";

    event_list el = parse_yaml(yaml, true);

    /* the result mapping should have no ALIAS events */
    int result_val = find_key_value(&el, 0, "result");
    ASSERT(result_val > 0, "found 'result'");

    bool found_alias = false;
    int depth = 0;
    for (int i = result_val; i < el.len; i++) {
        if (el.events[i].type == YAM_EVT_MAPPING_START) depth++;
        if (el.events[i].type == YAM_EVT_MAPPING_END) {
            depth--;
            if (depth == 0) break;
        }
        if (el.events[i].type == YAM_EVT_ALIAS) found_alias = true;
    }
    ASSERT(!found_alias, "no ALIAS events in resolved mapping");
}

/* ── Test: Merge preserves non-merge aliases ─────────────── */

static void test_non_merge_alias_preserved(void) {
    printf("test_non_merge_alias_preserved:\n");
    const char *yaml =
        "base: &base\n"
        "  x: 1\n"
        "ref: *base\n";

    event_list el = parse_yaml(yaml, true);

    /* non-merge alias should be preserved */
    bool found_alias = false;
    for (int i = 0; i < el.len; i++) {
        if (el.events[i].type == YAM_EVT_ALIAS) found_alias = true;
    }
    ASSERT(found_alias, "non-merge alias preserved");
}

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
    test_basic_merge();
    test_override();
    test_sequence_merge();
    test_multiple_merge_keys();
    test_quoted_not_merge();
    test_nested_merge();
    test_merge_non_mapping();
    test_opt_in();
    test_deep_values();
    test_empty_merge();
    test_no_alias_in_resolved();
    test_non_merge_alias_preserved();

    printf("\n--- Merge tests: %d / %d passed ---\n", tests_passed, tests_run);
    if (tests_failed > 0) printf("    %d FAILED\n", tests_failed);
    return (tests_passed == tests_run) ? 0 : 1;
}
