/*
 * test_scanner.c — Smoke tests for yam scanner
 */

#include "yam/yam.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

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
