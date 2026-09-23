/*
 * test_flow.c — Tests for flow collections in the incremental parser
 *
 * Each case is checked against an expected event string, and also against
 * the eager parser (forced by enabling merge keys), which is the reference
 * implementation for the incremental state machine.
 */

#define _POSIX_C_SOURCE 199309L

#include "yam/yam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Render the event stream as a compact string, e.g. "[ a { b c } ]".
 * Stream/document events are omitted; &anchor prefixes the node.
 * A parse error appends "ERR". */
static void render(const char *yaml, bool eager, char *out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    yam_arena  *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);
    if (eager) yam_parser_set_merge(p, true);

    yam_event evt;
    yam_status st;
    for (int guard = 0; guard < 1000; guard++) {
        st = yam_parse_next(p, &evt);
        if (st != YAM_OK) { n += snprintf(out + n, cap - n, n ? " ERR" : "ERR"); break; }
        if (evt.type == YAM_EVT_NONE || evt.type == YAM_EVT_STREAM_END) break;
        const char *s = NULL;
        switch (evt.type) {
        case YAM_EVT_SEQUENCE_START: s = "["; break;
        case YAM_EVT_SEQUENCE_END:   s = "]"; break;
        case YAM_EVT_MAPPING_START:  s = "{"; break;
        case YAM_EVT_MAPPING_END:    s = "}"; break;
        case YAM_EVT_SCALAR: case YAM_EVT_ALIAS: break;
        default: continue;
        }
        if (n) n += snprintf(out + n, cap - n, " ");
        if (evt.anchor.data)
            n += snprintf(out + n, cap - n, "&%.*s ", (int)evt.anchor.len, evt.anchor.data);
        if (s) {
            n += snprintf(out + n, cap - n, "%s", s);
        } else if (evt.type == YAM_EVT_ALIAS) {
            n += snprintf(out + n, cap - n, "*%.*s", (int)evt.value.len, evt.value.data);
        } else if (evt.value.len == 0) {
            n += snprintf(out + n, cap - n, "~");
        } else {
            n += snprintf(out + n, cap - n, "%.*s", (int)evt.value.len, evt.value.data);
        }
        if (n >= cap) break;
    }
    yam_parser_free(p);
    yam_arena_free(a);
}

/* On error, the eager parser delivers no events at all (it parses the whole
 * stream up front), so for error cases it only has to fail too. */
static void check(const char *yaml, const char *expected) {
    char inc[1024], eag[1024];
    render(yaml, false, inc, sizeof inc);
    render(yaml, true, eag, sizeof eag);
    size_t elen = strlen(expected);
    bool is_err = elen >= 3 && strcmp(expected + elen - 3, "ERR") == 0;
    bool eager_ok = is_err ? strcmp(eag, "ERR") == 0 : strcmp(eag, expected) == 0;
    tests_run++;
    if (strcmp(inc, expected) != 0 || !eager_ok) {
        printf("  FAIL: %s\n    expected:    %s\n    incremental: %s\n    eager:       %s\n",
               yaml, expected, inc, eag);
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* ── Flow collections as implicit keys ─────────────────────── */

static void test_flow_keys(void) {
    printf("test_flow_keys:\n");
    check("[a]: b", "{ [ a ] b }");
    check("- [a]: b", "[ { [ a ] b } ]");
    check("[ [a]: b ]", "[ { [ a ] b } ]");
    check("[ [a]: b, {c: d}: e, [[f]]: g ]",
          "[ { [ a ] b } { { c d } e } { [ [ f ] ] g } ]");
    check("[ x, [y] ]", "[ x [ y ] ]");
    check("k: [a, {b: c}]", "{ k [ a { b c } ] }");
}

/* Plain scalars containing quote or comment characters must not confuse
 * the flow-key lookahead. */
static void test_flow_keys_plain_quotes(void) {
    printf("test_flow_keys_plain_quotes:\n");
    check("[ [a'b]: c ]", "[ { [ a'b ] c } ]");
    check("- [a'b]: c", "[ { [ a'b ] c } ]");
    check("[ [a#b]: c ]", "[ { [ a#b ] c } ]");
    check("[ [a\"b]: c ]", "[ { [ a\"b ] c } ]");
    check("[ don't, [x] ]", "[ don't [ x ] ]");
    check("[ ['it''s', \"q\\\"]\"]: v ]", "[ { [ it's q\"] ] v } ]");
    check("[ [a] # c: d\n]", "[ [ a ] ]");
}

/* ── Properties on empty flow nodes ────────────────────────── */

static void test_flow_empty_props(void) {
    printf("test_flow_empty_props:\n");
    check("[&a, &b]", "[ &a ~ &b ~ ]");   /* used to loop forever */
    check("[&a, b]", "[ &a ~ b ]");       /* used to drop an entry */
    check("[&a ]", "[ &a ~ ]");
    check("[&a x: y]", "[ { &a x y } ]");
    check("{a: &b , c: d}", "{ a &b ~ c d }");
    check("[&a &b x]", "[ ERR");
}

/* ── JSON-style input ──────────────────────────────────────── */

static void test_json(void) {
    printf("test_json:\n");
    check("{\"a\": 1, \"b\": [true, null], \"c\": {\"d\": \"e\"}}",
          "{ a 1 b [ true null ] c { d e } }");
    check("[{\"a\": 1}, {\"b\": [2, 3]}, [[]], {}]",
          "[ { a 1 } { b [ 2 3 ] } [ [ ] ] { } ]");
    check("{\"a\":\"x\",\"b\":'y'}", "{ a x b y }");
    check("[1, 2,]", "[ 1 2 ]");
    check("{\"a\" : 1 , \"b\":2}", "{ a 1 b 2 }");
}

/* Quoted scalars without escapes are returned as slices of the input;
 * ones with escapes or line breaks are still decoded. */
static void test_quoted_values(void) {
    printf("test_quoted_values:\n");
    check("[\"plain\", 'single', \"\"]", "[ plain single ~ ]");
    check("[\"a\\tb\", 'it''s', \"x\\u00e9\"]", "[ a\tb it's x\xc3\xa9 ]");
    check("[\"line\n  fold\", 'one\n\n  two']", "[ line fold one\ntwo ]");

    const char *yaml = "{\"key\": \"value\"}";
    yam_arena  *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);
    yam_event evt;
    int scalars = 0;
    while (yam_parse_next(p, &evt) == YAM_OK && evt.type != YAM_EVT_STREAM_END) {
        if (evt.type != YAM_EVT_SCALAR) continue;
        ASSERT(evt.value.data >= yaml && evt.value.data < yaml + strlen(yaml),
               "unescaped double-quoted scalar points into the input");
        scalars++;
    }
    ASSERT(scalars == 2, "two scalars parsed");
    yam_parser_free(p);
    yam_arena_free(a);
}

/* ── Scanner error after buffered events ───────────────────── */

/* Events produced before a scan error are delivered before the error, and
 * nothing is invented after it. */
static void test_error_after_events(void) {
    printf("test_error_after_events:\n");
    check("{\"a\": 1, \"b\": \"\\q\"}", "{ a 1 b ERR");
    check("--- \"\\.\"\n", "ERR");
}

/* ── Deep nesting ──────────────────────────────────────────── */

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The flow-key lookahead used to rescan every nested collection, which is
 * quadratic in nesting depth (~1s at 40k levels, minutes at 1M). */
static void test_deep_nesting_linear(void) {
    printf("test_deep_nesting_linear:\n");
    int depth = 200000;
    char *buf = malloc((size_t)depth * 2 + 1);
    memset(buf, '[', depth);
    memset(buf + depth, ']', depth);
    buf[depth * 2] = '\n';

    double t0 = now();
    yam_arena  *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(buf, (size_t)depth * 2 + 1, a);
    yam_parser_set_max_events(p, 0);
    yam_event evt;
    yam_status st;
    int events = 0;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK &&
           evt.type != YAM_EVT_STREAM_END && evt.type != YAM_EVT_NONE)
        events++;
    double elapsed = now() - t0;

    ASSERT(st == YAM_OK, "deeply nested sequence parses");
    /* STREAM_START, DOC_START, a start and end per level, DOC_END */
    ASSERT(events == depth * 2 + 3, "one start and end event per level");
    ASSERT(elapsed < 2.0, "deep nesting parses in linear time");

    yam_parser_free(p);
    yam_arena_free(a);
    free(buf);
}

/* ── Main ───────────────────────────────────────────────────── */

int main(void) {
    test_flow_keys();
    test_flow_keys_plain_quotes();
    test_flow_empty_props();
    test_json();
    test_quoted_values();
    test_error_after_events();
    test_deep_nesting_linear();

    printf("\n--- Flow tests: %d / %d passed ---\n", tests_passed, tests_run);
    if (tests_failed > 0) printf("    %d FAILED\n", tests_failed);
    return (tests_passed == tests_run) ? 0 : 1;
}
