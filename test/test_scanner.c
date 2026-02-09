/*
 * test_scanner.c — Smoke tests for yam scanner
 */

#include "yam/yam.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#define GREEN "\033[32m"
#define RED   "\033[31m"
#define RESET "\033[0m"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  %-50s", name); \
    } while(0)

#define PASS() \
    do { tests_passed++; printf(GREEN "PASS" RESET "\n"); } while(0)

#define FAIL(msg) \
    do { printf(RED "FAIL" RESET " — %s\n", msg); } while(0)

/* ── Helpers ─────────────────────────────────────────────── */

static void dump_tokens(const char *input) {
    yam_arena *a = yam_arena_new(4096);
    yam_scanner *s = yam_scanner_new(input, strlen(input), a);

    yam_token tok;
    while (yam_scan_next(s, &tok) == YAM_OK && tok.type != YAM_TOK_NONE) {
        if (tok.type == YAM_TOK_STREAM_END) break;
        printf("    %-20s", yam_token_type_str(tok.type));
        if (tok.value.data && tok.value.len > 0) {
            printf(" \"%.*s\"", (int)tok.value.len, tok.value.data);
        }
        printf("  [%zu:%zu]\n", tok.start.line, tok.start.col);
    }

    yam_scanner_free(s);
    yam_arena_free(a);
}

/*
 * Persistent arena for tests — kept alive so arena-allocated
 * token values (quoted scalars) remain valid for assertions.
 */
static yam_arena *test_arena = NULL;

static void test_arena_init(void) {
    if (test_arena) yam_arena_reset(test_arena);
    else test_arena = yam_arena_new(4096);
}

/* Collect tokens from input into an array, return count */
static int scan_all(const char *input, yam_token *out, int max_tokens) {
    test_arena_init();
    yam_scanner *s = yam_scanner_new(input, strlen(input), test_arena);

    int count = 0;
    yam_status st;
    while (count < max_tokens) {
        st = yam_scan_next(s, &out[count]);
        if (st != YAM_OK) break;
        if (out[count].type == YAM_TOK_NONE) break;
        count++;
        if (out[count-1].type == YAM_TOK_STREAM_END) break;
    }

    yam_scanner_free(s);
    /* arena stays alive — token values remain valid */
    return count;
}

static int scan_all_debug(const char *input, yam_token *out, int max_tokens) {
    int n = scan_all(input, out, max_tokens);
    for (int i = 0; i < n; i++) {
        printf("      [%d] %-20s", i, yam_token_type_str(out[i].type));
        if (out[i].value.data && out[i].value.len > 0 && out[i].value.len < 40) {
            printf(" \"%.*s\"", (int)out[i].value.len, out[i].value.data);
        }
        printf("\n");
    }
    return n;
}

/* Collect events from parser into an array, return count */
static int parse_all(const char *input, yam_event *out, int max_events) {
    test_arena_init();
    yam_parser *p = yam_parser_new(input, strlen(input), test_arena);
    if (!p) return 0;

    int count = 0;
    while (count < max_events) {
        yam_status st = yam_parse_next(p, &out[count]);
        if (st != YAM_OK) break;
        if (out[count].type == YAM_EVT_STREAM_END) { count++; break; }
        count++;
    }

    yam_parser_free(p);
    return count;
}

/* ── Tests ───────────────────────────────────────────────── */

static void test_empty_stream(void) {
    TEST("empty stream");
    yam_token toks[8];
    int n = scan_all("", toks, 8);
    if (n >= 2 && toks[0].type == YAM_TOK_STREAM_START
               && toks[1].type == YAM_TOK_STREAM_END) {
        PASS();
    } else {
        FAIL("expected STREAM_START, STREAM_END");
    }
}

static void test_plain_scalar(void) {
    TEST("plain scalar");
    yam_token toks[8];
    int n = scan_all("hello", toks, 8);
    if (n >= 3 && toks[1].type == YAM_TOK_SCALAR
               && toks[1].value.len == 5
               && memcmp(toks[1].value.data, "hello", 5) == 0) {
        PASS();
    } else {
        FAIL("expected SCALAR 'hello'");
    }
}

static void test_mapping(void) {
    TEST("simple mapping (key: value)");
    yam_token toks[16];
    int n = scan_all("name: yam", toks, 16);

    /* expect: STREAM_START, SCALAR(name), BLOCK_MAP_VALUE, SCALAR(yam), STREAM_END */
    bool ok = n >= 5
        && toks[1].type == YAM_TOK_SCALAR
        && memcmp(toks[1].value.data, "name", 4) == 0
        && toks[2].type == YAM_TOK_BLOCK_MAP_VALUE
        && toks[3].type == YAM_TOK_SCALAR
        && memcmp(toks[3].value.data, "yam", 3) == 0;

    if (ok) PASS(); else FAIL("unexpected token sequence");
}

static void test_sequence(void) {
    TEST("block sequence");
    const char *input = "- one\n- two\n- three\n";
    yam_token toks[16];
    int n = scan_all_debug(input, toks, 16);

    bool ok = n >= 7
        && toks[1].type == YAM_TOK_BLOCK_SEQ_ENTRY
        && toks[2].type == YAM_TOK_SCALAR
        && toks[3].type == YAM_TOK_BLOCK_SEQ_ENTRY
        && toks[4].type == YAM_TOK_SCALAR
        && toks[5].type == YAM_TOK_BLOCK_SEQ_ENTRY
        && toks[6].type == YAM_TOK_SCALAR;

    if (ok) PASS(); else FAIL("unexpected token sequence");
}

static void test_flow_sequence(void) {
    TEST("flow sequence [a, b, c]");
    yam_token toks[16];
    int n = scan_all("[a, b, c]", toks, 16);

    bool ok = n >= 8
        && toks[1].type == YAM_TOK_FLOW_SEQ_START
        && toks[2].type == YAM_TOK_SCALAR
        && toks[3].type == YAM_TOK_FLOW_ENTRY
        && toks[4].type == YAM_TOK_SCALAR
        && toks[5].type == YAM_TOK_FLOW_ENTRY
        && toks[6].type == YAM_TOK_SCALAR
        && toks[7].type == YAM_TOK_FLOW_SEQ_END;

    if (ok) PASS(); else FAIL("unexpected token sequence");
}

static void test_flow_mapping(void) {
    TEST("flow mapping {a: 1, b: 2}");
    yam_token toks[16];
    int n = scan_all("{a: 1, b: 2}", toks, 16);

    bool ok = n >= 10
        && toks[1].type == YAM_TOK_FLOW_MAP_START
        && toks[2].type == YAM_TOK_SCALAR
        && toks[3].type == YAM_TOK_BLOCK_MAP_VALUE
        && toks[4].type == YAM_TOK_SCALAR;

    if (ok) PASS(); else FAIL("unexpected token sequence");
}

static void test_single_quoted(void) {
    TEST("single-quoted scalar with escape");
    yam_token toks[8];
    int n = scan_all("'it''s'", toks, 8);

    bool ok = n >= 3
        && toks[1].type == YAM_TOK_SCALAR
        && toks[1].scalar_style == YAM_SCALAR_SINGLE_QUOTED
        && toks[1].value.len == 4;
        /* value should be "it's" */

    if (ok) PASS(); else FAIL("expected unescaped single-quoted scalar");
}

static void test_double_quoted(void) {
    TEST("double-quoted scalar with escapes");
    yam_token toks[8];
    int n = scan_all_debug("\"hello\\nworld\"", toks, 8);

    bool ok = n >= 3
        && toks[1].type == YAM_TOK_SCALAR
        && toks[1].scalar_style == YAM_SCALAR_DOUBLE_QUOTED;

    /* check that \n was unescaped */
    if (ok && toks[1].value.len == 11) {
        ok = toks[1].value.data[5] == '\n';
    }

    if (ok) PASS(); else FAIL("expected unescaped double-quoted scalar");
}

static void test_doc_indicators(void) {
    TEST("document start/end indicators");
    yam_token toks[16];
    int n = scan_all("---\nhello\n...\n", toks, 16);

    bool ok = n >= 5
        && toks[1].type == YAM_TOK_DOC_START
        && toks[2].type == YAM_TOK_SCALAR
        && toks[3].type == YAM_TOK_DOC_END;

    if (ok) PASS(); else FAIL("unexpected token sequence");
}

static void test_tag(void) {
    TEST("tag !!str");
    yam_token toks[8];
    int n = scan_all("!!str hello", toks, 8);

    bool ok = n >= 4
        && toks[1].type == YAM_TOK_TAG
        && toks[2].type == YAM_TOK_SCALAR;

    if (ok) PASS(); else FAIL("expected TAG then SCALAR");
}

static void test_anchor_alias(void) {
    TEST("anchor and alias");
    yam_token toks[16];
    int n = scan_all("&ref hello\n*ref", toks, 16);

    bool ok = n >= 4
        && toks[1].type == YAM_TOK_ANCHOR
        && memcmp(toks[1].value.data, "ref", 3) == 0
        && toks[2].type == YAM_TOK_SCALAR;

    /* find the alias */
    bool found_alias = false;
    for (int i = 0; i < n; i++) {
        if (toks[i].type == YAM_TOK_ALIAS) {
            found_alias = (memcmp(toks[i].value.data, "ref", 3) == 0);
        }
    }

    if (ok && found_alias) PASS(); else FAIL("expected ANCHOR and ALIAS");
}

static void test_colon_in_scalar(void) {
    TEST("colon mid-word (http://example.com)");
    yam_token toks[8];
    int n = scan_all("http://example.com", toks, 8);

    bool ok = n >= 3
        && toks[1].type == YAM_TOK_SCALAR
        && toks[1].value.len == 18;

    if (ok) PASS(); else FAIL("colon should not split the scalar");
}

static void test_comment(void) {
    TEST("inline comment");
    yam_token toks[8];
    int n = scan_all("hello # world", toks, 8);

    bool ok = n >= 3
        && toks[1].type == YAM_TOK_SCALAR
        && toks[1].value.len == 5
        && memcmp(toks[1].value.data, "hello", 5) == 0;

    if (ok) PASS(); else FAIL("comment should be stripped");
}

static void test_yaml12_bool(void) {
    TEST("YAML 1.2: 'yes' is NOT a boolean");
    yam_token toks[8];
    int n = scan_all("yes", toks, 8);

    /* In 1.2, 'yes' is a plain scalar — the scanner shouldn't interpret it.
     * Tag resolution happens at a higher layer per core schema. */
    bool ok = n >= 3
        && toks[1].type == YAM_TOK_SCALAR
        && toks[1].value.len == 3
        && memcmp(toks[1].value.data, "yes", 3) == 0;

    if (ok) PASS(); else FAIL("scanner should preserve 'yes' as plain scalar");
}

static void test_long_double_escape(void) {
    TEST("long double-quoted \n escapes");
    const int esc_count = 512; /* keeps test quick while stressing growth */
    size_t in_len = (size_t)esc_count * 2 + 2; /* opening + closing quote */
    char *input = malloc(in_len + 1);
    if (!input) { FAIL("alloc"); return; }
    input[0] = '"';
    for (int i = 0; i < esc_count; i++) {
        input[1 + i * 2] = '\\';
        input[1 + i * 2 + 1] = 'N';
    }
    input[in_len - 1] = '"';
    input[in_len] = '\0';

    yam_token toks[4];
    int n = scan_all(input, toks, 4);
    bool ok = n >= 3 && toks[1].type == YAM_TOK_SCALAR;
    size_t expected_len = (size_t)esc_count * 2; /* each \N → two UTF-8 bytes */
    if (ok) ok = toks[1].value.len == expected_len;

    free(input);
    if (ok) PASS(); else FAIL("double-quoted growth failed");
}

/* ── Event mark tests ────────────────────────────────────── */

static void test_event_marks_scalar(void) {
    TEST("event marks: scalar");
    /* "hello" → +STR +DOC =VAL -DOC -STR */
    yam_event evts[8];
    int n = parse_all("hello", evts, 8);
    /* events: STREAM_START DOC_START SCALAR DOC_END STREAM_END */
    bool ok = n == 5;
    if (ok) ok = evts[0].type == YAM_EVT_STREAM_START;
    if (ok) ok = evts[0].start.line == 1 && evts[0].start.col == 1;
    if (ok) ok = evts[2].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[2].start.line == 1 && evts[2].start.col == 1;
    if (ok) PASS(); else FAIL("scalar marks");
}

static void test_event_marks_mapping(void) {
    TEST("event marks: mapping");
    /* "key: val" → +STR +DOC +MAP =VAL =VAL -MAP -DOC -STR */
    yam_event evts[16];
    int n = parse_all("key: val", evts, 16);
    bool ok = n == 8;
    /* MAP_START at 1:1 (where the key starts) */
    if (ok) ok = evts[2].type == YAM_EVT_MAPPING_START;
    if (ok) ok = evts[2].start.line == 1 && evts[2].start.col == 1;
    /* key scalar at 1:1 */
    if (ok) ok = evts[3].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[3].start.line == 1 && evts[3].start.col == 1;
    /* value scalar at 1:6 */
    if (ok) ok = evts[4].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[4].start.line == 1 && evts[4].start.col == 6;
    if (ok) PASS(); else FAIL("mapping marks");
}

static void test_event_marks_sequence(void) {
    TEST("event marks: sequence");
    /* "- a\n- b" → +STR +DOC +SEQ =VAL =VAL -SEQ -DOC -STR */
    yam_event evts[16];
    int n = parse_all("- a\n- b", evts, 16);
    bool ok = n == 8;
    /* SEQ_START at 1:1 (where first - is) */
    if (ok) ok = evts[2].type == YAM_EVT_SEQUENCE_START;
    if (ok) ok = evts[2].start.line == 1 && evts[2].start.col == 1;
    /* first value "a" at 1:3 */
    if (ok) ok = evts[3].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[3].start.line == 1 && evts[3].start.col == 3;
    /* second value "b" at 2:3 */
    if (ok) ok = evts[4].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[4].start.line == 2 && evts[4].start.col == 3;
    if (ok) PASS(); else FAIL("sequence marks");
}

static void test_event_marks_flow(void) {
    TEST("event marks: flow");
    /* "[a, b]" → +STR +DOC +SEQ =VAL =VAL -SEQ -DOC -STR */
    yam_event evts[16];
    int n = parse_all("[a, b]", evts, 16);
    bool ok = n == 8;
    /* SEQ_START at 1:1 (the [ character) */
    if (ok) ok = evts[2].type == YAM_EVT_SEQUENCE_START;
    if (ok) ok = evts[2].start.line == 1 && evts[2].start.col == 1;
    /* first value "a" at 1:2 */
    if (ok) ok = evts[3].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[3].start.line == 1 && evts[3].start.col == 2;
    /* SEQ_END at 1:6 (the ] character) */
    if (ok) ok = evts[5].type == YAM_EVT_SEQUENCE_END;
    if (ok) ok = evts[5].start.line == 1 && evts[5].start.col == 6;
    if (ok) PASS(); else FAIL("flow marks");
}

static void test_event_marks_doc(void) {
    TEST("event marks: doc indicators");
    /* "---\nhello\n..." → +STR +DOC =VAL -DOC -STR */
    yam_event evts[8];
    int n = parse_all("---\nhello\n...", evts, 8);
    bool ok = n == 5;
    /* DOC_START at 1:1 */
    if (ok) ok = evts[1].type == YAM_EVT_DOC_START;
    if (ok) ok = evts[1].start.line == 1 && evts[1].start.col == 1;
    /* SCALAR at 2:1 */
    if (ok) ok = evts[2].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[2].start.line == 2 && evts[2].start.col == 1;
    /* DOC_END at 3:1 */
    if (ok) ok = evts[3].type == YAM_EVT_DOC_END;
    if (ok) ok = evts[3].start.line == 3 && evts[3].start.col == 1;
    if (ok) PASS(); else FAIL("doc marks");
}

static void test_event_marks_anchor(void) {
    TEST("event marks: anchor");
    /* "&ref hi" → +STR +DOC =VAL -DOC -STR — scalar start at 1:1 (anchor pos) */
    yam_event evts[8];
    int n = parse_all("&ref hi", evts, 8);
    bool ok = n == 5;
    if (ok) ok = evts[2].type == YAM_EVT_SCALAR;
    /* start should be at anchor position 1:1 (attach_props moves start) */
    if (ok) ok = evts[2].start.line == 1 && evts[2].start.col == 1;
    if (ok) PASS(); else FAIL("anchor marks");
}

static void test_event_marks_multiline(void) {
    TEST("event marks: multiline mapping");
    /* "a: 1\nb: 2" → +STR +DOC +MAP =VAL =VAL =VAL =VAL -MAP -DOC -STR */
    yam_event evts[16];
    int n = parse_all("a: 1\nb: 2", evts, 16);
    bool ok = n == 10;
    /* key "b" at 2:1 */
    if (ok) ok = evts[5].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[5].start.line == 2 && evts[5].start.col == 1;
    /* val "2" at 2:4 */
    if (ok) ok = evts[6].type == YAM_EVT_SCALAR;
    if (ok) ok = evts[6].start.line == 2 && evts[6].start.col == 4;
    if (ok) PASS(); else FAIL("multiline marks");
}

/* ── Main ────────────────────────────────────────────────── */

int main(void) {
    printf("\nyam scanner tests\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    test_empty_stream();
    test_plain_scalar();
    test_mapping();
    test_sequence();
    test_flow_sequence();
    test_flow_mapping();
    test_single_quoted();
    test_double_quoted();
    test_doc_indicators();
    test_tag();
    test_anchor_alias();
    test_colon_in_scalar();
    test_comment();
    test_yaml12_bool();
    test_long_double_escape();
    test_event_marks_scalar();
    test_event_marks_mapping();
    test_event_marks_sequence();
    test_event_marks_flow();
    test_event_marks_doc();
    test_event_marks_anchor();
    test_event_marks_multiline();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  %d/%d passed", tests_passed, tests_run);
    if (tests_passed == tests_run)
        printf("  " GREEN "✓ all clear" RESET);
    else
        printf("  " RED "✗ failures" RESET);
    printf("\n\n");

    /* bonus: dump tokens for a realistic document */
    printf("Token dump for a sample document:\n");
    printf("───────────────────────────────────────────────────────────\n");
    dump_tokens(
        "---\n"
        "name: yam\n"
        "version: 0.1.0\n"
        "features:\n"
        "  - yaml-1.2\n"
        "  - simd\n"
        "  - zero-copy\n"
        "config: {indent: 2, unicode: true}\n"
        "...\n"
    );

    return (tests_passed == tests_run) ? 0 : 1;
}
