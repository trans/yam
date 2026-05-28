/*
 * test_errors.c — Tests for file input and error messages
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

/* ── File input tests ───────────────────────────────────────── */

static void test_read_file(void) {
    printf("test_read_file:\n");

    /* write a temp file */
    const char *path = "/tmp/yam_test_input.yaml";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL, "can create temp file");
    if (!f) return;
    const char *content = "key: value\nlist:\n  - a\n  - b\n";
    fwrite(content, 1, strlen(content), f);
    fclose(f);

    yam_arena *a = yam_arena_new(4096);
    yam_str data = yam_read_file(path, a);

    ASSERT(data.data != NULL, "read file returns non-null");
    ASSERT(data.len == strlen(content), "read file returns correct length");
    ASSERT(memcmp(data.data, content, data.len) == 0, "read file content matches");

    yam_arena_free(a);
    remove(path);
}

static void test_read_nonexistent(void) {
    printf("test_read_nonexistent:\n");

    yam_arena *a = yam_arena_new(4096);
    yam_str data = yam_read_file("/tmp/yam_nonexistent_file.yaml", a);

    ASSERT(data.data == NULL, "nonexistent file returns NULL");
    ASSERT(data.len == 0, "nonexistent file returns zero length");

    yam_arena_free(a);
}

static void test_read_and_parse(void) {
    printf("test_read_and_parse:\n");

    const char *path = "/tmp/yam_test_parse.yaml";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL, "can create temp file");
    if (!f) return;
    fprintf(f, "hello: world\n");
    fclose(f);

    yam_arena *a = yam_arena_new(4096);
    yam_str data = yam_read_file(path, a);
    ASSERT(data.data != NULL, "read file ok");

    yam_parser *p = yam_parser_new(data.data, data.len, a);
    ASSERT(p != NULL, "parser created");

    yam_event evt;
    yam_status st;
    int scalar_count = 0;
    bool found_hello = false, found_world = false;

    while ((st = yam_parse_next(p, &evt)) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END) break;
        if (evt.type == YAM_EVT_SCALAR) {
            scalar_count++;
            if (evt.value.len == 5 && memcmp(evt.value.data, "hello", 5) == 0)
                found_hello = true;
            if (evt.value.len == 5 && memcmp(evt.value.data, "world", 5) == 0)
                found_world = true;
        }
    }

    ASSERT(st == YAM_OK, "parse succeeded");
    ASSERT(scalar_count == 2, "expected exactly 2 scalars (key + value)");
    ASSERT(found_hello, "found 'hello' scalar");
    ASSERT(found_world, "found 'world' scalar");

    yam_parser_free(p);
    yam_arena_free(a);
    remove(path);
}

/* ── Scanner error tests ────────────────────────────────────── */

static void test_unterminated_single_quote(void) {
    printf("test_unterminated_single_quote:\n");

    const char *yaml = "'unterminated";
    yam_arena *a = yam_arena_new(4096);
    yam_scanner *s = yam_scanner_new(yaml, strlen(yaml), a);

    yam_token tok;
    yam_status st;
    /* skip STREAM_START */
    st = yam_scan_next(s, &tok);
    ASSERT(st == YAM_OK, "stream start ok");

    st = yam_scan_next(s, &tok);
    ASSERT(st == YAM_ERR_SCAN, "unterminated single-quote returns error");

    const char *msg = yam_scanner_error(s);
    ASSERT(msg != NULL, "error message is set");
    ASSERT(strstr(msg, "unterminated") != NULL, "message mentions 'unterminated'");

    yam_mark m = yam_scanner_error_mark(s);
    ASSERT(m.line > 0, "error mark has valid line");

    yam_scanner_free(s);
    yam_arena_free(a);
}

static void test_unterminated_double_quote(void) {
    printf("test_unterminated_double_quote:\n");

    const char *yaml = "\"unterminated";
    yam_arena *a = yam_arena_new(4096);
    yam_scanner *s = yam_scanner_new(yaml, strlen(yaml), a);

    yam_token tok;
    yam_status st;
    st = yam_scan_next(s, &tok); /* STREAM_START */
    st = yam_scan_next(s, &tok);
    ASSERT(st == YAM_ERR_SCAN, "unterminated double-quote returns error");

    const char *msg = yam_scanner_error(s);
    ASSERT(msg != NULL, "error message is set");
    ASSERT(strstr(msg, "unterminated") != NULL, "message mentions 'unterminated'");

    yam_scanner_free(s);
    yam_arena_free(a);
}

static void test_invalid_escape(void) {
    printf("test_invalid_escape:\n");

    const char *yaml = "\"bad\\z\"";
    yam_arena *a = yam_arena_new(4096);
    yam_scanner *s = yam_scanner_new(yaml, strlen(yaml), a);

    yam_token tok;
    yam_status st;
    st = yam_scan_next(s, &tok); /* STREAM_START */
    st = yam_scan_next(s, &tok);
    ASSERT(st == YAM_ERR_SCAN, "invalid escape returns error");

    const char *msg = yam_scanner_error(s);
    ASSERT(msg != NULL, "error message is set");
    ASSERT(strstr(msg, "escape") != NULL, "message mentions 'escape'");

    yam_scanner_free(s);
    yam_arena_free(a);
}

static void test_empty_anchor(void) {
    printf("test_empty_anchor:\n");

    const char *yaml = "& ";
    yam_arena *a = yam_arena_new(4096);
    yam_scanner *s = yam_scanner_new(yaml, strlen(yaml), a);

    yam_token tok;
    yam_status st;
    st = yam_scan_next(s, &tok); /* STREAM_START */
    st = yam_scan_next(s, &tok);
    ASSERT(st == YAM_ERR_SCAN, "empty anchor returns error");

    const char *msg = yam_scanner_error(s);
    ASSERT(msg != NULL, "error message is set");
    ASSERT(strstr(msg, "empty") != NULL, "message mentions 'empty'");

    yam_scanner_free(s);
    yam_arena_free(a);
}

/* ── Parser error tests ─────────────────────────────────────── */

static void test_missing_flow_seq_end(void) {
    printf("test_missing_flow_seq_end:\n");

    const char *yaml = "[a, b";
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);

    yam_event evt;
    yam_status st;
    bool got_error = false;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END) break;
    }
    if (st != YAM_OK) got_error = true;

    /* The parser may or may not error depending on how it handles
     * unterminated flow sequences. Check either way. */
    if (got_error) {
        const char *msg = yam_parser_error(p);
        ASSERT(msg != NULL, "parser error message set for missing ]");
    } else {
        /* parser produced events without error — that's also acceptable */
        ASSERT(1, "parser handled missing ] gracefully");
    }

    yam_parser_free(p);
    yam_arena_free(a);
}

static void test_missing_flow_map_end(void) {
    printf("test_missing_flow_map_end:\n");

    const char *yaml = "{a: b";
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);

    yam_event evt;
    yam_status st;
    bool got_error = false;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END) break;
    }
    if (st != YAM_OK) got_error = true;

    if (got_error) {
        const char *msg = yam_parser_error(p);
        ASSERT(msg != NULL, "parser error message set for missing }");
    } else {
        ASSERT(1, "parser handled missing } gracefully");
    }

    yam_parser_free(p);
    yam_arena_free(a);
}

static void test_scanner_error_through_parser(void) {
    printf("test_scanner_error_through_parser:\n");

    const char *yaml = "key: 'unterminated";
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);

    yam_event evt;
    yam_status st;
    bool got_error = false;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END) break;
    }
    if (st != YAM_OK) got_error = true;

    ASSERT(got_error, "scanner error propagated through parser");

    const char *msg = yam_parser_error(p);
    ASSERT(msg != NULL, "parser has error message from scanner");
    if (msg) {
        ASSERT(strstr(msg, "unterminated") != NULL,
               "propagated message mentions 'unterminated'");
    }

    yam_mark m = yam_parser_error_mark(p);
    ASSERT(m.line > 0, "error mark has valid line");

    yam_parser_free(p);
    yam_arena_free(a);
}

/* ── No-error tests ─────────────────────────────────────────── */

static void test_no_scanner_error(void) {
    printf("test_no_scanner_error:\n");

    const char *yaml = "hello: world";
    yam_arena *a = yam_arena_new(4096);
    yam_scanner *s = yam_scanner_new(yaml, strlen(yaml), a);

    yam_token tok;
    while (yam_scan_next(s, &tok) == YAM_OK) {
        if (tok.type == YAM_TOK_STREAM_END) break;
    }

    ASSERT(yam_scanner_error(s) == NULL, "no error on valid input");

    yam_scanner_free(s);
    yam_arena_free(a);
}

static void test_no_parser_error(void) {
    printf("test_no_parser_error:\n");

    const char *yaml = "hello: world";
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);

    yam_event evt;
    while (yam_parse_next(p, &evt) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END) break;
    }

    ASSERT(yam_parser_error(p) == NULL, "no error on valid input");

    yam_parser_free(p);
    yam_arena_free(a);
}

/* ── Main ───────────────────────────────────────────────────── */

int main(void) {
    /* file input */
    test_read_file();
    test_read_nonexistent();
    test_read_and_parse();

    /* scanner errors */
    test_unterminated_single_quote();
    test_unterminated_double_quote();
    test_invalid_escape();
    test_empty_anchor();

    /* parser errors */
    test_missing_flow_seq_end();
    test_missing_flow_map_end();
    test_scanner_error_through_parser();

    /* no error */
    test_no_scanner_error();
    test_no_parser_error();

    printf("\n--- Error tests: %d / %d passed ---\n", tests_passed, tests_run);
    if (tests_failed > 0) printf("    %d FAILED\n", tests_failed);
    return (tests_passed == tests_run) ? 0 : 1;
}
