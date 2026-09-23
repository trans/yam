/*
 * test_emitter.c — Tests for YAML emitter
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

/* ── Helper: emit events from a YAML string, return output ── */

static yam_str roundtrip(const char *yaml, yam_emit_style opts, yam_arena *a) {
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);
    yam_emitter *e = yam_emitter_new(a);
    yam_emitter_set_style(e, opts);
    const yam_event *evt;

    while (yam_parse_next(p, &evt) == YAM_OK) {
        yam_emit(e, evt);
        if (evt->type == YAM_EVT_STREAM_END) break;
    }

    yam_str out = yam_emitter_output(e);
    yam_emitter_free(e);
    yam_parser_free(p);
    return out;
}

static bool str_contains(yam_str s, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen > s.len) return false;
    for (size_t i = 0; i <= s.len - nlen; i++) {
        if (memcmp(s.data + i, needle, nlen) == 0) return true;
    }
    return false;
}

static bool str_eq(yam_str s, const char *expected) {
    size_t elen = strlen(expected);
    return s.len == elen && memcmp(s.data, expected, elen) == 0;
}

/* ── Test: Simple flow output ────────────────────────────── */

static void test_flow_simple(void) {
    printf("test_flow_simple:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_FLOW;

    yam_str out = roundtrip("key: value\n", opts, a);
    ASSERT(str_contains(out, "{"), "flow mapping has {");
    ASSERT(str_contains(out, "}"), "flow mapping has }");
    ASSERT(str_contains(out, "key"), "flow has key");
    ASSERT(str_contains(out, "value"), "flow has value");

    yam_arena_free(a);
}

/* ── Test: Simple minimal output ─────────────────────────── */

static void test_minimal_simple(void) {
    printf("test_minimal_simple:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_MINIMAL;

    yam_str out = roundtrip("key: value\n", opts, a);
    ASSERT(str_contains(out, "{"), "minimal has {");
    ASSERT(str_contains(out, "}"), "minimal has }");
    /* minimal compacts entry separator (no space after comma) */
    ASSERT(!str_contains(out, ", "), "minimal has no ', '");

    yam_arena_free(a);
}

/* ── Test: Block mapping ─────────────────────────────────── */

static void test_block_mapping(void) {
    printf("test_block_mapping:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("name: Alice\nage: \"30\"\n", opts, a);
    ASSERT(str_contains(out, "name: Alice"), "block map key: value");
    ASSERT(str_contains(out, "age: \"30\"") || str_contains(out, "age: '30'"),
           "quoted value preserved");

    yam_arena_free(a);
}

/* ── Test: Block sequence ────────────────────────────────── */

static void test_block_sequence(void) {
    printf("test_block_sequence:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("- one\n- two\n- three\n", opts, a);
    ASSERT(str_contains(out, "- one"), "seq item one");
    ASSERT(str_contains(out, "- two"), "seq item two");
    ASSERT(str_contains(out, "- three"), "seq item three");

    yam_arena_free(a);
}

/* ── Test: Nested mapping in sequence ────────────────────── */

static void test_nested_map_in_seq(void) {
    printf("test_nested_map_in_seq:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("- name: Alice\n  age: \"30\"\n- name: Bob\n", opts, a);
    ASSERT(str_contains(out, "- name:"), "compact mapping in sequence");
    ASSERT(str_contains(out, "Alice"), "has Alice");
    ASSERT(str_contains(out, "Bob"), "has Bob");

    yam_arena_free(a);
}

/* ── Test: Sequence in mapping ───────────────────────────── */

static void test_seq_in_map(void) {
    printf("test_seq_in_map:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("items:\n  - one\n  - two\n", opts, a);
    ASSERT(str_contains(out, "items:"), "has key");
    ASSERT(str_contains(out, "- one"), "has item one");
    ASSERT(str_contains(out, "- two"), "has item two");

    yam_arena_free(a);
}

/* ── Test: Flow sequence in block ────────────────────────── */

static void test_flow_in_block(void) {
    printf("test_flow_in_block:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("key: [a, b, c]\n", opts, a);
    ASSERT(str_contains(out, "["), "has [");
    ASSERT(str_contains(out, "]"), "has ]");

    yam_arena_free(a);
}

/* ── Test: Scalar quoting ────────────────────────────────── */

static void test_scalar_quoting(void) {
    printf("test_scalar_quoting:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    /* plain scalars stay plain, so they keep their type on reload */
    yam_str out = roundtrip("port: 8080\nok: true\nnone:\nz: ~\n", opts, a);
    ASSERT(str_eq(out, "port: 8080\nok: true\nnone:\nz: ~\n"),
           "plain numbers, bools and nulls are not quoted");

    /* quoted scalars stay strings */
    yam_arena_reset(a);
    out = roundtrip("s: \"8080\"\nt: 'true'\ne: ''\n", opts, a);
    ASSERT(str_eq(out, "s: \"8080\"\nt: 'true'\ne: ''\n"), "quoted scalars keep quotes");

    /* a !!str-tagged plain scalar that would resolve to another type is
     * quoted to stay a string */
    yam_arena_reset(a);
    out = roundtrip("t: !!str 42\n", opts, a);
    ASSERT(str_eq(out, "t: !!str \"42\"\n"), "!!str number is quoted");

    /* an empty (null) entry in a flow sequence is written as ~ */
    yam_arena_reset(a);
    out = roundtrip("- a\n-\n", YAM_EMIT_FLOW, a);
    ASSERT(str_eq(out, "[a, ~]\n"), "null flow sequence entry is ~");

    /* syntax still forces quotes */
    yam_arena_reset(a);
    out = roundtrip("k: \"a: b\"\nl: \"#x\"\n", opts, a);
    ASSERT(str_eq(out, "k: \"a: b\"\nl: \"#x\"\n"), "unsafe plain text stays quoted");

    yam_arena_free(a);
}

/* ── Test: Document markers ──────────────────────────────── */

static void test_doc_markers(void) {
    printf("test_doc_markers:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("---\nkey: value\n...\n", opts, a);
    ASSERT(str_contains(out, "---"), "has doc start");
    ASSERT(str_contains(out, "..."), "has doc end");

    yam_arena_free(a);
}

/* ── Test: Anchor and alias ──────────────────────────────── */

static void test_anchor_alias(void) {
    printf("test_anchor_alias:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("a: &anchor hello\nb: *anchor\n", opts, a);
    ASSERT(str_contains(out, "&anchor"), "has anchor");
    ASSERT(str_contains(out, "*anchor"), "has alias");

    yam_arena_free(a);
}

/* ── Test: Tag emission ──────────────────────────────────── */

static void test_tags(void) {
    printf("test_tags:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("!!str true\n", opts, a);
    ASSERT(str_contains(out, "!!str"), "has !!str tag");

    yam_arena_free(a);
}

/* ── Test: Empty collections ─────────────────────────────── */

static void test_empty_collections(void) {
    printf("test_empty_collections:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("map: {}\nseq: []\n", opts, a);
    ASSERT(str_contains(out, "{}"), "empty map as {}");
    ASSERT(str_contains(out, "[]"), "empty seq as []");

    yam_arena_free(a);
}

/* ── Test: Round-trip (parse → emit → re-parse → compare) ── */

static bool events_match(const char *yaml, yam_emit_style opts) {
    yam_arena *a1 = yam_arena_new(8192);
    yam_arena *a2 = yam_arena_new(8192);

    /* First pass: parse original */
    yam_parser *p1 = yam_parser_new(yaml, strlen(yaml), a1);
    yam_event evts1[256];
    int n1 = 0;
    while (n1 < 256) {
        const yam_event *ev;
        if (yam_parse_next(p1, &ev) != YAM_OK) break;
        evts1[n1] = *ev;
        if (evts1[n1].type == YAM_EVT_STREAM_END) { n1++; break; }
        n1++;
    }
    yam_parser_free(p1);

    /* Emit */
    yam_emitter *e = yam_emitter_new(a1);
    yam_emitter_set_style(e, opts);
    for (int i = 0; i < n1; i++) yam_emit(e, &evts1[i]);
    yam_str out = yam_emitter_output(e);
    yam_emitter_free(e);

    if (!out.data) { yam_arena_free(a1); yam_arena_free(a2); return false; }

    /* Second pass: parse emitted output */
    yam_parser *p2 = yam_parser_new(out.data, out.len, a2);
    yam_event evts2[256];
    int n2 = 0;
    while (n2 < 256) {
        const yam_event *ev;
        if (yam_parse_next(p2, &ev) != YAM_OK) break;
        evts2[n2] = *ev;
        if (evts2[n2].type == YAM_EVT_STREAM_END) { n2++; break; }
        n2++;
    }
    yam_parser_free(p2);

    /* Compare event types and scalar values */
    bool match = (n1 == n2);
    if (match) {
        for (int i = 0; i < n1; i++) {
            if (evts1[i].type != evts2[i].type) { match = false; break; }
            if (evts1[i].type == YAM_EVT_SCALAR) {
                if (evts1[i].value.len != evts2[i].value.len ||
                    memcmp(evts1[i].value.data, evts2[i].value.data,
                           evts1[i].value.len) != 0) {
                    match = false; break;
                }
            }
            if (evts1[i].type == YAM_EVT_ALIAS) {
                if (evts1[i].value.len != evts2[i].value.len ||
                    memcmp(evts1[i].value.data, evts2[i].value.data,
                           evts1[i].value.len) != 0) {
                    match = false; break;
                }
            }
        }
    }

    if (!match && out.data) {
        printf("    emitted:\n---\n%.*s---\n", (int)out.len, out.data);
        printf("    events: %d vs %d\n", n1, n2);
        for (int i = 0; i < (n1 > n2 ? n1 : n2); i++) {
            const char *t1 = i < n1 ? yam_event_type_str(evts1[i].type) : "(none)";
            const char *t2 = i < n2 ? yam_event_type_str(evts2[i].type) : "(none)";
            if (i < n1 && i < n2 && evts1[i].type == evts2[i].type) continue;
            printf("    [%d] %s vs %s\n", i, t1, t2);
        }
    }

    yam_arena_free(a1);
    yam_arena_free(a2);
    return match;
}

static void test_roundtrip(void) {
    printf("test_roundtrip:\n");
    yam_emit_style block = YAM_EMIT_BLOCK;
    yam_emit_style flow = YAM_EMIT_FLOW;
    yam_emit_style minimal = YAM_EMIT_MINIMAL;

    /* simple mapping */
    ASSERT(events_match("name: Alice\nage: \"30\"\n", block), "rt: block mapping");
    ASSERT(events_match("name: Alice\nage: \"30\"\n", flow), "rt: flow mapping");
    ASSERT(events_match("name: Alice\nage: \"30\"\n", minimal), "rt: minimal mapping");

    /* simple sequence */
    ASSERT(events_match("- one\n- two\n- three\n", block), "rt: block sequence");
    ASSERT(events_match("- one\n- two\n- three\n", flow), "rt: flow sequence");

    /* nested */
    ASSERT(events_match("items:\n  - one\n  - two\n", block), "rt: seq in map");
    ASSERT(events_match("items:\n  - one\n  - two\n", flow), "rt: seq in map (flow)");

    /* compact mapping in sequence */
    ASSERT(events_match("- name: Alice\n- name: Bob\n", block), "rt: map in seq");

    /* flow collections in block */
    ASSERT(events_match("key: [a, b, c]\n", block), "rt: flow seq in block");
    ASSERT(events_match("key: {a: 1, b: 2}\n", block), "rt: flow map in block");

    /* document markers */
    ASSERT(events_match("---\nkey: value\n...\n", block), "rt: doc markers");

    /* anchor/alias */
    ASSERT(events_match("a: &x hello\nb: *x\n", block), "rt: anchor/alias");

    /* empty collections */
    ASSERT(events_match("map: {}\nseq: []\n", block), "rt: empty collections");

    /* multi-level nesting */
    ASSERT(events_match("a:\n  b:\n    c: deep\n", block), "rt: deep nesting");
    ASSERT(events_match("a:\n  b:\n    c: deep\n", flow), "rt: deep nesting (flow)");

    /* single scalar document */
    ASSERT(events_match("hello\n", block), "rt: bare scalar");
    ASSERT(events_match("hello\n", flow), "rt: bare scalar (flow)");

    /* keywords */
    ASSERT(events_match("val: true\n", block), "rt: keyword true");
    ASSERT(events_match("val: null\n", block), "rt: keyword null");
    ASSERT(events_match("val: \"42\"\n", block), "rt: quoted number");
}

/* ── Test: Deeply nested block ───────────────────────────── */

/* A block collection value's properties go on the key's line, and a flow
 * collection value stays on it; nothing may land at column 0 under a key
 * (which the parser rejects). */
static void test_block_value_placement(void) {
    printf("test_block_value_placement:\n");
    yam_arena *a = yam_arena_new(4096);
    const char *yaml =
        "a: &x\n  b: c\n"
        "k: [one, two]\n"
        "s: !!seq\n- u\n- &m\n  v: w\n";
    yam_str out = roundtrip(yaml, YAM_EMIT_BLOCK, a);
    ASSERT(str_eq(out,
        "a: &x\n  b: c\n"
        "k: [one, two]\n"
        "s: !!seq\n  - u\n  - &m\n    v: w\n"),
        "props and flow values stay on the key's line");

    /* the output parses back */
    yam_parser *p = yam_parser_new(out.data, out.len, a);
    const yam_event *evt;
    yam_status st;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK && evt->type != YAM_EVT_STREAM_END)
        ;
    ASSERT(st == YAM_OK, "emitted block output reparses");
    yam_parser_free(p);
    yam_arena_free(a);
}

/* Building output with the event functions, and the option setters */
static void test_build_events(void) {
    printf("test_build_events:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emitter *e = yam_emitter_new(a);
    yam_emitter_set_indent(e, 4);

    yam_emit_stream_start(e);
    yam_emit_document_start(e, true);
    yam_emit_mapping_start(e, YAM_STR_NULL, YAM_STR_NULL, false);
    yam_emit_scalar(e, YAM_STR_LIT("name"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
    yam_emit_scalar(e, YAM_STR_LIT("yam"), YAM_SCALAR_PLAIN, YAM_STR_LIT("n"), YAM_STR_NULL);
    yam_emit_scalar(e, YAM_STR_LIT("port"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
    yam_emit_scalar(e, YAM_STR_LIT("8080"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_TAG_STR);
    yam_emit_scalar(e, YAM_STR_LIT("list"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
    yam_emit_sequence_start(e, YAM_STR_NULL, YAM_STR_NULL, false);
    yam_emit_alias(e, YAM_STR_LIT("n"));
    yam_emit_scalar(e, YAM_STR_LIT("two words"), YAM_SCALAR_SINGLE_QUOTED, YAM_STR_NULL, YAM_STR_NULL);
    yam_emit_sequence_end(e);
    yam_emit_scalar(e, YAM_STR_LIT("flow"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
    yam_emit_sequence_start(e, YAM_STR_NULL, YAM_STR_NULL, true);
    yam_emit_scalar(e, YAM_STR_LIT("a"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
    yam_emit_sequence_end(e);
    yam_emit_mapping_end(e);
    yam_emit_document_end(e, true);
    ASSERT(yam_emit_stream_end(e) == YAM_OK, "event functions succeed");

    yam_str out = yam_emitter_output(e);
    ASSERT(str_eq(out,
        "name: &n yam\n"
        "port: !!str \"8080\"\n"
        "list:\n"
        "    - *n\n"
        "    - 'two words'\n"
        "flow: [a]\n"), "built document");

    yam_emitter_free(e);
    e = yam_emitter_new(a);
    yam_emitter_set_style(e, YAM_EMIT_MINIMAL);
    yam_emit_stream_start(e);
    yam_emit_document_start(e, true);
    yam_emit_sequence_start(e, YAM_STR_NULL, YAM_STR_NULL, false);
    yam_emit_scalar(e, YAM_STR_LIT("x"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
    yam_emit_scalar(e, YAM_STR_LIT("y"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
    yam_emit_sequence_end(e);
    yam_emit_document_end(e, true);
    yam_emit_stream_end(e);
    ASSERT(str_eq(yam_emitter_output(e), "[x,y]\n"), "minimal style via setter");
    yam_emitter_free(e);
    yam_arena_free(a);
}

/* Tags are written in a form that reads back as the same tag */
static void test_tag_forms(void) {
    printf("test_tag_forms:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emitter *e = yam_emitter_new(a);
    yam_emit_stream_start(e);
    yam_emit_document_start(e, true);
    yam_emit_sequence_start(e, YAM_STR_NULL, YAM_STR_NULL, false);
    yam_emit_scalar(e, YAM_STR_LIT("a"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_TAG_INT);
    yam_emit_scalar(e, YAM_STR_LIT("b"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_LIT("!local"));
    yam_emit_scalar(e, YAM_STR_LIT("c"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_LIT("!a b"));
    yam_emit_scalar(e, YAM_STR_LIT("d"), YAM_SCALAR_PLAIN, YAM_STR_NULL,
                    YAM_STR_LIT("tag:example.com,2000:x!y"));
    yam_emit_sequence_end(e);
    yam_emit_document_end(e, true);
    yam_emit_stream_end(e);
    yam_str out = yam_emitter_output(e);
    ASSERT(str_eq(out, "- !!int a\n- !local b\n- !<!a%20b> c\n"
                       "- !<tag:example.com,2000:x!y> d\n"), "tag forms");

    /* and they read back as the same tags */
    const char *want[] = { "tag:yaml.org,2002:int", "!local", "!a b",
                           "tag:example.com,2000:x!y" };
    yam_parser *p = yam_parser_new(out.data, out.len, a);
    const yam_event *evt;
    int i = 0;
    bool same = true;
    while (yam_parse_next(p, &evt) == YAM_OK && evt->type != YAM_EVT_STREAM_END)
        if (evt->type == YAM_EVT_SCALAR)
            same = same && i < 4 && evt->tag.len == strlen(want[i]) &&
                   memcmp(evt->tag.data, want[i], evt->tag.len) == 0 && ++i;
    ASSERT(same && i == 4, "tags read back unchanged");
    yam_parser_free(p);
    yam_emitter_free(e);
    yam_arena_free(a);
}

static void test_deep_block(void) {
    printf("test_deep_block:\n");
    yam_arena *a = yam_arena_new(4096);
    yam_emit_style opts = YAM_EMIT_BLOCK;

    yam_str out = roundtrip("a:\n  b:\n    c: deep\n", opts, a);
    ASSERT(str_contains(out, "a:"), "has a:");
    ASSERT(str_contains(out, "b:"), "has b:");
    ASSERT(str_contains(out, "c: deep"), "has c: deep");

    yam_arena_free(a);
}

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
    test_flow_simple();
    test_minimal_simple();
    test_block_mapping();
    test_block_sequence();
    test_nested_map_in_seq();
    test_seq_in_map();
    test_flow_in_block();
    test_scalar_quoting();
    test_doc_markers();
    test_anchor_alias();
    test_tags();
    test_empty_collections();
    test_deep_block();
    test_block_value_placement();
    test_build_events();
    test_tag_forms();
    test_roundtrip();

    printf("\n--- Emitter tests: %d / %d passed ---\n", tests_passed, tests_run);
    if (tests_failed > 0) printf("    %d FAILED\n", tests_failed);
    return (tests_passed == tests_run) ? 0 : 1;
}
