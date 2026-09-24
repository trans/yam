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

    const yam_event *evt;
    yam_status st;
    for (int guard = 0; guard < 1000; guard++) {
        st = yam_parse_next(p, &evt);
        if (st != YAM_OK) { n += snprintf(out + n, cap - n, n ? " ERR" : "ERR"); break; }
        if (evt->type == YAM_EVT_NONE || evt->type == YAM_EVT_STREAM_END) break;
        const char *s = NULL;
        switch (evt->type) {
        case YAM_EVT_SEQUENCE_START: s = "["; break;
        case YAM_EVT_SEQUENCE_END:   s = "]"; break;
        case YAM_EVT_MAPPING_START:  s = "{"; break;
        case YAM_EVT_MAPPING_END:    s = "}"; break;
        case YAM_EVT_SCALAR: case YAM_EVT_ALIAS: break;
        default: continue;
        }
        if (n) n += snprintf(out + n, cap - n, " ");
        if (evt->anchor.data)
            n += snprintf(out + n, cap - n, "&%.*s ", (int)evt->anchor.len, evt->anchor.data);
        if (s) {
            n += snprintf(out + n, cap - n, "%s", s);
        } else if (evt->type == YAM_EVT_ALIAS) {
            n += snprintf(out + n, cap - n, "*%.*s", (int)evt->value.len, evt->value.data);
        } else if (evt->value.len == 0) {
            n += snprintf(out + n, cap - n, "~");
        } else {
            n += snprintf(out + n, cap - n, "%.*s", (int)evt->value.len, evt->value.data);
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
    /* a flow key after the first entry is a key of the same mapping */
    check("k: v\n[c, d]: [e]\n", "{ k v [ c d ] [ e ] }");
    check("k: v\n{c: d}: e\n", "{ k v { c d } e }");
    check("k: v\n&a [c]: [e]\nz: 1\n", "{ k v &a [ c ] [ e ] z 1 }");
    /* props before a first flow key; a flow key on the line after ':' */
    check("&x [a]:\n[b]: c\n", "{ &x [ a ] ~ [ b ] c }");
    check("x:\n [b]: c\n", "{ x { [ b ] c } }");
    check("[a]:\n [b]: c\n", "{ [ a ] { [ b ] c } }");
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
    /* a quote after a blank inside a plain scalar doesn't start a string */
    check("[a \"b\", [c]: d]", "[ a \"b\" { [ c ] d } ]");
    check("[ [a]: b, {c: d}: x  \"y]: g ]", "{ [ { [ a ] b } { { c d } x  \"y } ] g ] }");
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
    check("k: &a\n!t :\n", "{ k &a ~ ~ ~ }");   /* next entry: tagged empty key */
    /* props on an empty value, then a sibling entry with props */
    check("k: &a\n&b x: y\n", "{ k &a ~ &b x y }");
    check("a: b\n&k : v\n", "{ a b &k ~ v }");
    /* mapping props, then a key with its own props, then more keys */
    check("&r\n&s a: b\nc: d\n", "&r { &s a b c d }");
    check("a: ? b\n", "{ a ERR");            /* '?' on an implicit key's line */
    check("? []\n[]\n", "{ [ ] ~ ERR");       /* flow key without ':' */
    /* a flow-collection key: indentation counts from where it starts */
    check("!!map {a: b}: |\n  # t\n", "{ { a b } # t\n }");
    check("&r\n&k {a: &n }: |\n  x\n", "&r { &k { a &n ~ } x\n }");  /* props inside */
    /* a key with props on its line: indentation counts from the props */
    check("&r\n&k oo: |\n  a\n", "&r { &k oo a\n }");
    check("&r\n&k oo: a\n  b\nc: d\n", "&r { &k oo a b c d }");
    /* props alone on a line in a mapping's key position */
    check("top:\n  a: b\n  &k\ntop1: c\n", "{ top { a b ERR");
    check("key:\n  &a\n  a: b\n", "{ key &a { a b } }");   /* value props: fine */
    /* props on the line before a bare ':' belong to the mapping */
    check("a: &m\n : &b\n*c : d\n", "{ a &m { ~ &b ~ } *c d }");
    /* an empty sequence entry before a shallower ':' */
    check("?\n  -\n:\n", "{ [ ~ ] ~ }");
    check("a\x01b: c\n", "ERR");           /* control character */
    check("%TAG ! a\x01b\n--- x\n", "ERR"); /* ... in a directive */
    /* an explicit entry's ':' is at the mapping's indentation */
    check("? b\n  : x\n", "{ b ERR");
    check("- ? b\n  : x\n", "[ { b x } ]");
    /* a ':' starting a later line is not an implicit key's */
    check("? &x\n  k: *t\n: v\n", "{ &x { k *t } v }");
    /* props on two lines before a flow collection */
    check("&a\n&b [x]\n", "ERR");                        /* two anchors */
    check("&m\n&k [a]: b\nc: d\n", "&m { &k [ a ] b c d }"); /* key */
    /* a key at the mapping's indentation ends an empty value */
    check("a:\n*b : c\n", "{ a ~ *b c }");
    check("a:\n[x]: y\n", "{ a ~ [ x ] y }");
    check("?\n  []:\n:\n", "{ { [ ] ~ } ~ }");
    /* '?' followed by nothing indented: an empty key */
    check("?\n?\n", "{ ~ ~ ~ ~ }");
    check("?\n- a\n", "{ [ a ] ~ }");   /* zero-indented sequence as key */
    /* props on a block mapping, then an empty key with its own props */
    check("&m\n&k : v\nb: c\n", "&m { &k ~ v b c }");
    check("[&a &b x]", "[ ERR");
    check("!<", "ERR");            /* unterminated verbatim tag */
    check("&a\n*b\n", "ERR");      /* anchor on an alias, next line */
    check("[&a\n *b]", "[ ERR");    /* same, in flow */
    check("!<> x", "ERR");         /* empty verbatim tag */
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
    check("{\"a\": b,:x: 1}", "{ a b :x 1 }");  /* ':' after ',' starts a plain scalar */
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
    const yam_event *evt;
    int scalars = 0;
    while (yam_parse_next(p, &evt) == YAM_OK && evt->type != YAM_EVT_STREAM_END) {
        if (evt->type != YAM_EVT_SCALAR) continue;
        ASSERT(evt->value.data >= yaml && evt->value.data < yaml + strlen(yaml),
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
    yam_parser_set_max_depth(p, 0);
    const yam_event *evt;
    yam_status st;
    int events = 0;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK &&
           evt->type != YAM_EVT_STREAM_END && evt->type != YAM_EVT_NONE)
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

/* ── Safety limits ─────────────────────────────────────────── */

static yam_status parse_all(const char *yaml, size_t len, bool eager,
                            int max_depth, const char **msg) {
    yam_arena  *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, len, a);
    if (eager) yam_parser_set_merge(p, true);
    yam_parser_set_max_events(p, 0);
    if (max_depth >= 0) yam_parser_set_max_depth(p, max_depth);
    const yam_event *evt;
    yam_status st;
    int guard = 0;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK &&
           evt->type != YAM_EVT_STREAM_END && evt->type != YAM_EVT_NONE &&
           ++guard < 10000000)
        ;
    static char buf[256];
    const char *m = yam_parser_error(p);
    snprintf(buf, sizeof buf, "%s", m ? m : "");
    *msg = buf;
    yam_parser_free(p);
    yam_arena_free(a);
    return st;
}

/* Deep nesting stops with YAM_ERR_LIMIT in both parse modes; the eager
 * (recursive) mode used to overflow the stack. */
static void test_depth_limit(void) {
    printf("test_depth_limit:\n");
    int depth = 100000;
    char *buf = malloc((size_t)depth * 4);
    for (int i = 0; i < depth; i++) memcpy(buf + i * 4, "!t [", 4);
    const char *msg;

    for (int eager = 0; eager <= 1; eager++) {
        yam_status st = parse_all(buf, (size_t)depth * 4, eager, -1, &msg);
        ASSERT(st == YAM_ERR_LIMIT, "deep nesting hits the depth limit");
        ASSERT(strstr(msg, "depth") != NULL, "depth limit has an error message");
    }

    /* exactly at the limit is fine */
    char ok[2 * 8 + 2];
    memset(ok, '[', 8);
    memset(ok + 8, ']', 8);
    ok[16] = '\0';
    ASSERT(parse_all(ok, 16, false, 8, &msg) == YAM_OK, "depth == limit parses");
    ASSERT(parse_all(ok, 16, true, 8, &msg) == YAM_OK, "depth == limit parses (eager)");
    ASSERT(parse_all(ok, 16, false, 7, &msg) == YAM_ERR_LIMIT, "depth > limit fails");
    ASSERT(parse_all(ok, 16, true, 7, &msg) == YAM_ERR_LIMIT, "depth > limit fails (eager)");
    free(buf);
}

/* Hitting the event limit reports YAM_ERR_LIMIT with a message. */
static void test_event_limit(void) {
    printf("test_event_limit:\n");
    const char *yaml = "[a, b, c, d, e, f, g, h, i, j]";
    for (int eager = 0; eager <= 1; eager++) {
        yam_arena  *a = yam_arena_new(4096);
        yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);
        if (eager) yam_parser_set_merge(p, true);
        yam_parser_set_max_events(p, 5);
        const yam_event *evt;
        yam_status st;
        while ((st = yam_parse_next(p, &evt)) == YAM_OK &&
               evt->type != YAM_EVT_STREAM_END && evt->type != YAM_EVT_NONE)
            ;
        ASSERT(st == YAM_ERR_LIMIT, "event limit returns YAM_ERR_LIMIT");
        const char *m = yam_parser_error(p);
        ASSERT(m && strstr(m, "event limit"), "event limit has an error message");
        yam_parser_free(p);
        yam_arena_free(a);
    }
}

/* Stray flow indicators outside a flow collection are errors, not an
 * endless stream of empty nodes. */
static void test_stray_flow_indicators(void) {
    printf("test_stray_flow_indicators:\n");
    const char *cases[] = { "]", "}", ",", "{a: b}}", "[a]\n]", "a: b\n}",
                            "!!str,", "&a ,", "- !!str, xxx" };
    const char *msg;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (int eager = 0; eager <= 1; eager++) {
            yam_status st = parse_all(cases[i], strlen(cases[i]), eager, -1, &msg);
            tests_run++;
            if (st != YAM_ERR_PARSE) {
                printf("  FAIL: %s (%s): status %d\n", cases[i],
                       eager ? "eager" : "incremental", st);
                tests_failed++;
            } else {
                tests_passed++;
            }
        }
    }
    check("a ]", "a ]");  /* ] is fine inside a block plain scalar */
}

/* Continuation lines of flow collections and quoted scalars must be
 * indented more than the enclosing block; a line starting with the closing
 * bracket or quote may be at any indentation (a common style). */
static void test_continuation_indent(void) {
    printf("test_continuation_indent:\n");
    check("key: [\n  a,\n  b\n]", "{ key [ a b ] }");
    check("key: {\n  a: 1\n}", "{ key { a 1 } }");
    check("[\na,\nb\n]", "[ a b ]");                  /* top level: any indent */
    check("k: \"a\n  b\n\"", "{ k a b  }");
    check("k: [a,\nb]", "{ k [ a ERR");
    check("k: [a\nb]", "{ k [ a ERR");
    check("k: \"a\nb\"", "{ k ERR");
    check("k: 'a\nb'", "{ k ERR");
    check("- [\n\tfoo\n ]", "[ [ ERR");               /* tab isn't indentation */
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
    test_depth_limit();
    test_event_limit();
    test_stray_flow_indicators();
    test_continuation_indent();

    printf("\n--- Flow tests: %d / %d passed ---\n", tests_passed, tests_run);
    if (tests_failed > 0) printf("    %d FAILED\n", tests_failed);
    return (tests_passed == tests_run) ? 0 : 1;
}
