/*
 * test_yaml_suite.c — YAML Test Suite runner for yam parser
 *
 * Reads test cases from yaml-test-suite/src/<id>.yaml,
 * runs the parser, and compares output events against expected trees.
 */

#include "yam/yam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdbool.h>
#include <ctype.h>

/* ── Colors ──────────────────────────────────────────────── */

#define GREEN  "\033[32m"
#define RED    "\033[31m"
#define YELLOW "\033[33m"
#define DIM    "\033[2m"
#define RESET  "\033[0m"

/* ── Test case structure ─────────────────────────────────── */

typedef struct {
    char id[16];      /* test ID from filename */
    char name[256];
    char yaml[8192];  /* input YAML */
    char tree[8192];  /* expected event tree */
    bool fail;        /* expect parse error */
    bool skip;        /* explicitly skipped by test suite */
    int  index;       /* sub-test index (0-based) */
} test_case;

/* ── Stats ───────────────────────────────────────────────── */

static int total = 0, passed = 0, failed = 0, skipped = 0, errors = 0;
static int xfailed = 0, xpassed = 0;

/* ── Unicode unescaping ──────────────────────────────────── */

/*
 * The YAML test suite uses special unicode characters as placeholders:
 *   ␣ (U+2423, 3 bytes: E2 90 A3) → space
 *   » (U+00BB, 2 bytes: C2 BB) → tab
 *   — (U+2014, 3 bytes: E2 80 94) followed by » → tab (EM DASH combo)
 *   ← (U+2190, 3 bytes: E2 86 90) → CR (\r)
 *   ⇔ (U+21D4, 3 bytes: E2 87 94) → BOM (U+FEFF, 3 bytes: EF BB BF)
 *   ∎ (U+220E, 3 bytes: E2 88 8E) at end → signals end, no trailing newline
 */

static size_t unescape_yaml_special(const char *src, size_t srclen, char *dst, size_t dstcap) {
    size_t si = 0, di = 0;

    /* check for trailing ∎ — may have trailing newlines after it from block extraction.
     * Search backwards past any trailing newlines to find ∎ */
    bool has_end_marker = false;
    size_t check_end = srclen;
    while (check_end > 0 && src[check_end-1] == '\n') check_end--;
    if (check_end >= 3 &&
        (unsigned char)src[check_end-3] == 0xE2 &&
        (unsigned char)src[check_end-2] == 0x88 &&
        (unsigned char)src[check_end-1] == 0x8E) {
        srclen = check_end - 3;
        has_end_marker = true;
    }

    while (si < srclen && di < dstcap - 4) {
        unsigned char c = (unsigned char)src[si];

        /* 3-byte UTF-8 sequences */
        if (c == 0xE2 && si + 2 < srclen) {
            unsigned char b1 = (unsigned char)src[si+1];
            unsigned char b2 = (unsigned char)src[si+2];

            /* ␣ (U+2423) → space */
            if (b1 == 0x90 && b2 == 0xA3) {
                dst[di++] = ' ';
                si += 3;
                continue;
            }
            /* ↵ (U+21B5) → visual newline marker, strip it (the actual
             * newline is already present from the line break) */
            if (b1 == 0x86 && b2 == 0xB5) {
                si += 3;
                continue;
            }
            /* ⇔ (U+21D4) → BOM */
            if (b1 == 0x87 && b2 == 0x94) {
                dst[di++] = (char)0xEF;
                dst[di++] = (char)0xBB;
                dst[di++] = (char)0xBF;
                si += 3;
                continue;
            }
            /* — (U+2014) optionally repeated, followed by » → tab
             * Handles: —» (5 bytes), ——» (8 bytes), ———» (11 bytes) */
            if (b1 == 0x80 && b2 == 0x94) {
                /* count consecutive em dashes */
                size_t skip = si + 3;
                while (skip + 2 < srclen &&
                       (unsigned char)src[skip] == 0xE2 &&
                       (unsigned char)src[skip+1] == 0x80 &&
                       (unsigned char)src[skip+2] == 0x94) {
                    skip += 3;
                }
                /* check for trailing » */
                if (skip + 1 < srclen &&
                    (unsigned char)src[skip] == 0xC2 &&
                    (unsigned char)src[skip+1] == 0xBB) {
                    dst[di++] = '\t';
                    si = skip + 2;
                    continue;
                }
                /* standalone em dash — just copy */
            }
            /* ∎ mid-string — just copy */
        }

        /* 2-byte: » (U+00BB) → tab */
        if (c == 0xC2 && si + 1 < srclen && (unsigned char)src[si+1] == 0xBB) {
            dst[di++] = '\t';
            si += 2;
            continue;
        }

        dst[di++] = src[si++];
    }

    /* if ∎ was present, strip trailing newline */
    if (has_end_marker && di > 0 && dst[di-1] == '\n') {
        di--;
    }

    dst[di] = '\0';
    return di;
}

/* ── Extract indented block content ──────────────────────── */

/*
 * Given a position past "yaml: |" or "tree: |" + newline,
 * extract the indented block content (stripping leading indent).
 */
static size_t extract_block(const char *text, size_t pos, size_t len,
                            int indent, char *out, size_t outcap) {
    size_t oi = 0;
    while (pos < len && oi < outcap - 2) {
        /* determine line indent */
        int li = 0;
        while (pos + li < len && text[pos + li] == ' ') li++;

        if (pos + li >= len) break;

        if (text[pos + li] == '\n') {
            /* empty line */
            out[oi++] = '\n';
            pos += li + 1;
            continue;
        }

        if (li < indent) break; /* de-indented — end of block */

        /* skip indent spaces */
        pos += indent;

        /* copy rest of line */
        while (pos < len && text[pos] != '\n' && oi < outcap - 2) {
            out[oi++] = text[pos++];
        }
        out[oi++] = '\n';
        if (pos < len && text[pos] == '\n') pos++;
    }

    out[oi] = '\0';
    return oi;
}

/* ── Find a key in YAML-ish text ─────────────────────────── */

static bool find_key_value(const char *text, size_t len, const char *key,
                           int base_indent, size_t start, size_t *value_pos) {
    size_t klen = strlen(key);
    size_t pos = start;

    while (pos < len) {
        /* count leading spaces */
        int li = 0;
        while (pos + li < len && text[pos + li] == ' ') li++;

        if (li == base_indent &&
            pos + li + klen <= len &&
            memcmp(text + pos + li, key, klen) == 0) {
            *value_pos = pos + li + klen;
            return true;
        }

        /* also match "- key" at col 0 (continuation entries) */
        if (li == 0 && base_indent == 2 &&
            pos + 2 + klen <= len &&
            text[pos] == '-' && text[pos + 1] == ' ' &&
            memcmp(text + pos + 2, key, klen) == 0) {
            *value_pos = pos + 2 + klen;
            return true;
        }

        /* skip to next line */
        while (pos < len && text[pos] != '\n') pos++;
        if (pos < len) pos++;
    }
    return false;
}

/* ── Parse test cases from a .yaml file ──────────────────── */

static int parse_test_file(const char *path, const char *id, test_case *cases, int maxcases) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 65536) { fclose(f); return 0; }

    char *text = malloc(fsize + 1);
    if (!text) { fclose(f); return 0; }
    size_t nread = fread(text, 1, fsize, f);
    fclose(f);
    text[nread] = '\0';

    int count = 0;
    size_t pos = 0;
    size_t len = nread;

    /* skip leading --- */
    if (len > 3 && text[0] == '-' && text[1] == '-' && text[2] == '-') {
        pos = 3;
        while (pos < len && text[pos] != '\n') pos++;
        if (pos < len) pos++;
    }

    /* find list entries (lines starting with "- ") */
    while (pos < len && count < maxcases) {
        /* find next "- " at indent 0 */
        size_t entry_start = pos;
        bool found_entry = false;
        while (pos < len) {
            if (text[pos] == '-' && pos + 1 < len && text[pos + 1] == ' ') {
                /* check it's at col 0 */
                if (pos == 0 || text[pos - 1] == '\n') {
                    entry_start = pos;
                    found_entry = true;
                    break;
                }
            }
            /* skip to next line */
            while (pos < len && text[pos] != '\n') pos++;
            if (pos < len) pos++;
        }
        if (!found_entry) break;

        /* find end of this entry (next "- " at col 0 or EOF) */
        size_t entry_end = pos + 1;
        while (entry_end < len) {
            if (text[entry_end] == '-' && entry_end + 1 < len && text[entry_end + 1] == ' ') {
                if (entry_end == 0 || text[entry_end - 1] == '\n') break;
            }
            while (entry_end < len && text[entry_end] != '\n') entry_end++;
            if (entry_end < len) entry_end++;
        }

        test_case *tc = &cases[count];
        memset(tc, 0, sizeof(test_case));
        snprintf(tc->id, sizeof(tc->id), "%s", id);
        tc->index = count;

        int base_indent = 2; /* entries are indented 2 spaces */

        /* check for fail: true */
        size_t vpos;
        if (find_key_value(text, entry_end, "fail: true", base_indent, entry_start, &vpos)) {
            tc->fail = true;
        }
        /* also check for "- fail: true" on the entry line itself */
        if (!tc->fail && entry_start + 13 <= entry_end &&
            memcmp(text + entry_start, "- fail: true", 12) == 0 &&
            (text[entry_start + 12] == '\n' || text[entry_start + 12] == '\r')) {
            tc->fail = true;
        }

        /* check for skip: true (applies to all cases in the file) */
        if (find_key_value(text, entry_end, "skip: true", base_indent, entry_start, &vpos)) {
            tc->skip = true;
        }
        if (!tc->skip && entry_start + 13 <= entry_end &&
            memcmp(text + entry_start, "- skip: true", 12) == 0 &&
            (text[entry_start + 12] == '\n' || text[entry_start + 12] == '\r')) {
            tc->skip = true;
        }
        /* propagate skip from first entry to all subsequent entries */
        if (count > 0 && cases[0].skip) {
            tc->skip = true;
        }

        /* extract name */
        if (find_key_value(text, entry_end, "name: ", base_indent, entry_start, &vpos)) {
            size_t ni = 0;
            while (vpos < entry_end && text[vpos] != '\n' && ni < sizeof(tc->name) - 1) {
                tc->name[ni++] = text[vpos++];
            }
            tc->name[ni] = '\0';
        }

        /* extract yaml: block */
        if (find_key_value(text, entry_end, "yaml: ", base_indent, entry_start, &vpos)) {
            /* check for "yaml: |" style */
            while (vpos < entry_end && text[vpos] == ' ') vpos++;

            if (text[vpos] == '|') {
                /* check for |N (explicit indent) */
                int yaml_indent = base_indent + 2;
                vpos++;
                if (vpos < entry_end && text[vpos] >= '0' && text[vpos] <= '9') {
                    yaml_indent = base_indent + (text[vpos] - '0');
                    vpos++;
                }
                /* skip to next line */
                while (vpos < entry_end && text[vpos] != '\n') vpos++;
                if (vpos < entry_end) vpos++;

                char raw[8192];
                extract_block(text, vpos, entry_end, yaml_indent, raw, sizeof(raw));
                unescape_yaml_special(raw, strlen(raw), tc->yaml, sizeof(tc->yaml));
            }
        }

        /* extract tree: block */
        if (find_key_value(text, entry_end, "tree: ", base_indent, entry_start, &vpos)) {
            while (vpos < entry_end && text[vpos] == ' ') vpos++;
            if (text[vpos] == '|') {
                vpos++;
                while (vpos < entry_end && text[vpos] != '\n') vpos++;
                if (vpos < entry_end) vpos++;

                char raw_tree[8192];
                extract_block(text, vpos, entry_end, base_indent + 2, raw_tree, sizeof(raw_tree));
                unescape_yaml_special(raw_tree, strlen(raw_tree), tc->tree, sizeof(tc->tree));
            }
        }

        /* an entry without its own expected outcome reuses the previous
         * entry's, as the suite's own tooling does (MUS6:4 ... MUS6:6) */
        if (count > 0 && !tc->fail && tc->tree[0] == '\0') {
            const test_case *prev = &cases[count - 1];
            tc->fail = prev->fail;
            memcpy(tc->tree, prev->tree, sizeof(tc->tree));
        }

        count++;
        pos = entry_end;
    }

    free(text);
    return count;
}

/* ── Event formatter ─────────────────────────────────────── */

/*
 * Format a yam_event into test suite notation:
 *   +STR, -STR, +DOC, +DOC ---, -DOC, -DOC ...,
 *   +MAP, +MAP {}, +SEQ, +SEQ [],
 *   -MAP, -SEQ,
 *   =VAL <props> <style><text>
 *   =ALI *name
 */

static size_t escape_scalar(const char *src, size_t srclen, char *dst, size_t dstcap) {
    size_t di = 0;
    for (size_t i = 0; i < srclen && di < dstcap - 5; i++) {
        switch (src[i]) {
        case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
        case '\n': dst[di++] = '\\'; dst[di++] = 'n'; break;
        case '\r': dst[di++] = '\\'; dst[di++] = 'r'; break;
        case '\t': dst[di++] = '\\'; dst[di++] = 't'; break;
        case '\0': dst[di++] = '\\'; dst[di++] = '0'; break;
        case '\a': dst[di++] = '\\'; dst[di++] = 'a'; break;
        case '\b': dst[di++] = '\\'; dst[di++] = 'b'; break;
        default:   dst[di++] = src[i]; break;
        }
    }
    dst[di] = '\0';
    return di;
}

static char scalar_style_char(yam_scalar_style s) {
    switch (s) {
    case YAM_SCALAR_PLAIN:         return ':';
    case YAM_SCALAR_SINGLE_QUOTED: return '\'';
    case YAM_SCALAR_DOUBLE_QUOTED: return '"';
    case YAM_SCALAR_LITERAL:       return '|';
    case YAM_SCALAR_FOLDED:        return '>';
    }
    return ':';
}

static size_t format_event(const yam_event *evt, char *buf, size_t cap) {
    size_t n = 0;

    switch (evt->type) {
    case YAM_EVT_STREAM_START:
        n = snprintf(buf, cap, "+STR\n");
        break;
    case YAM_EVT_STREAM_END:
        n = snprintf(buf, cap, "-STR\n");
        break;
    case YAM_EVT_DOC_START:
        if (evt->implicit)
            n = snprintf(buf, cap, "+DOC\n");
        else
            n = snprintf(buf, cap, "+DOC ---\n");
        break;
    case YAM_EVT_DOC_END:
        if (evt->implicit)
            n = snprintf(buf, cap, "-DOC\n");
        else
            n = snprintf(buf, cap, "-DOC ...\n");
        break;
    case YAM_EVT_MAPPING_START: {
        char props[512] = "";
        size_t pi = 0;
        /* flow indicator comes first in test suite format */
        if (evt->flow) {
            pi += snprintf(props + pi, sizeof(props) - pi, " {}");
        }
        if (evt->anchor.data && evt->anchor.len > 0) {
            pi += snprintf(props + pi, sizeof(props) - pi, " &%.*s",
                           (int)evt->anchor.len, evt->anchor.data);
        }
        if (evt->tag.data && evt->tag.len > 0) {
            pi += snprintf(props + pi, sizeof(props) - pi, " <%.*s>",
                           (int)evt->tag.len, evt->tag.data);
        }
        n = snprintf(buf, cap, "+MAP%s\n", props);
        break;
    }
    case YAM_EVT_MAPPING_END:
        n = snprintf(buf, cap, "-MAP\n");
        break;
    case YAM_EVT_SEQUENCE_START: {
        char props[512] = "";
        size_t pi = 0;
        /* flow indicator comes first in test suite format */
        if (evt->flow) {
            pi += snprintf(props + pi, sizeof(props) - pi, " []");
        }
        if (evt->anchor.data && evt->anchor.len > 0) {
            pi += snprintf(props + pi, sizeof(props) - pi, " &%.*s",
                           (int)evt->anchor.len, evt->anchor.data);
        }
        if (evt->tag.data && evt->tag.len > 0) {
            pi += snprintf(props + pi, sizeof(props) - pi, " <%.*s>",
                           (int)evt->tag.len, evt->tag.data);
        }
        n = snprintf(buf, cap, "+SEQ%s\n", props);
        break;
    }
    case YAM_EVT_SEQUENCE_END:
        n = snprintf(buf, cap, "-SEQ\n");
        break;
    case YAM_EVT_SCALAR: {
        char props[512] = "";
        size_t pi = 0;
        if (evt->anchor.data && evt->anchor.len > 0) {
            pi += snprintf(props + pi, sizeof(props) - pi, " &%.*s",
                           (int)evt->anchor.len, evt->anchor.data);
        }
        if (evt->tag.data && evt->tag.len > 0) {
            pi += snprintf(props + pi, sizeof(props) - pi, " <%.*s>",
                           (int)evt->tag.len, evt->tag.data);
        }
        char escaped[4096];
        if (evt->value.data && evt->value.len > 0) {
            escape_scalar(evt->value.data, evt->value.len, escaped, sizeof(escaped));
        } else {
            escaped[0] = '\0';
        }
        n = snprintf(buf, cap, "=VAL%s %c%s\n", props,
                     scalar_style_char(evt->scalar_style), escaped);
        break;
    }
    case YAM_EVT_ALIAS:
        n = snprintf(buf, cap, "=ALI *%.*s\n",
                     (int)evt->value.len, evt->value.data);
        break;
    case YAM_EVT_NONE:
        break;
    }
    return n;
}

/* ── Run a single test case ──────────────────────────────── */

typedef enum { RESULT_PASS, RESULT_FAIL, RESULT_ERROR, RESULT_SKIP } test_result;

/* Invalid-YAML tests (fail: true) that yam currently accepts instead of
 * rejecting. They are reported as XFAIL rather than FAIL so the suite can
 * gate regressions; once one is fixed the runner reports XPASS (and fails)
 * until it is removed from this list. Keep the list shrinking. */
static const char *known_accepted_invalid[] = {
    NULL /* all invalid cases are currently rejected */
};

static bool is_known_accepted(const char *label) {
    for (size_t i = 0; known_accepted_invalid[i]; i++)
        if (strcmp(known_accepted_invalid[i], label) == 0) return true;
    return false;
}

static test_result run_test(test_case *tc, bool verbose, bool eager) {
    size_t input_len = strlen(tc->yaml);

    /* skip tests with no expected tree (unless fail test) */
    if (!tc->fail && strlen(tc->tree) == 0) return RESULT_SKIP;

    yam_arena *arena = yam_arena_new(4096);
    if (!arena) return RESULT_ERROR;

    yam_parser *parser = yam_parser_new(tc->yaml, input_len, arena);
    /* Merge keys force the eager (whole-stream) parser; no suite case
     * relies on merge semantics, so both modes must produce the same
     * events and reject the same inputs. */
    if (parser && eager) yam_parser_set_merge(parser, true);
    if (!parser) {
        yam_arena_free(arena);
        return RESULT_ERROR;
    }

    /* collect events */
    char actual[16384];
    size_t actual_len = 0;
    const yam_event *evt;
    yam_status st;
    bool parse_error = false;
    int evt_count = 0;

    while (evt_count < 500) {
        st = yam_parse_next(parser, &evt);
        if (st != YAM_OK) {
            parse_error = true;
            break;
        }
        if (evt->type == YAM_EVT_NONE) break;

        actual_len += format_event(evt, actual + actual_len,
                                   sizeof(actual) - actual_len);
        evt_count++;

        if (evt->type == YAM_EVT_STREAM_END) break;
    }

    yam_parser_free(parser);
    yam_arena_free(arena);

    if (tc->fail) {
        /* invalid YAML: the parser must report an error */
        return parse_error ? RESULT_PASS : RESULT_FAIL;
    }

    if (parse_error) {
        if (verbose) {
            printf("    " RED "Parse error" RESET "\n");
        }
        return RESULT_FAIL;
    }

    /* compare actual vs expected tree */
    /* strip leading/trailing whitespace from both for comparison */
    char expected[16384];
    strncpy(expected, tc->tree, sizeof(expected) - 1);
    expected[sizeof(expected) - 1] = '\0';

    /* trim trailing whitespace */
    size_t elen = strlen(expected);
    while (elen > 0 && (expected[elen-1] == '\n' || expected[elen-1] == ' ')) elen--;
    expected[elen] = '\0';

    size_t alen = actual_len;
    while (alen > 0 && (actual[alen-1] == '\n' || actual[alen-1] == ' ')) alen--;
    actual[alen] = '\0';

    /* line-by-line comparison, ignoring leading whitespace (tree uses indentation for readability) */
    const char *ep = expected, *ap = actual;

    while (*ep && *ap) {
        /* skip leading whitespace in expected */
        while (*ep == ' ') ep++;
        /* skip leading whitespace in actual */
        while (*ap == ' ') ap++;

        /* find end of line */
        const char *ee = ep;
        while (*ee && *ee != '\n') ee++;
        const char *ae = ap;
        while (*ae && *ae != '\n') ae++;

        size_t el = ee - ep;
        size_t al = ae - ap;

        if (el != al || memcmp(ep, ap, el) != 0) {
            if (verbose) {
                printf("    " RED "MISMATCH" RESET "\n");
                printf("      expected: %.*s\n", (int)el, ep);
                printf("      actual:   %.*s\n", (int)al, ap);
            }
            return RESULT_FAIL;
        }

        ep = ee;
        ap = ae;
        if (*ep == '\n') ep++;
        if (*ap == '\n') ap++;
    }

    /* check remaining content */
    while (*ep == ' ' || *ep == '\n') ep++;
    while (*ap == ' ' || *ap == '\n') ap++;

    if (*ep || *ap) {
        if (verbose) {
            printf("    " RED "EXTRA CONTENT" RESET "\n");
            if (*ep) printf("      remaining expected: %.60s...\n", ep);
            if (*ap) printf("      remaining actual:   %.60s...\n", ap);
        }
        return RESULT_FAIL;
    }

    return RESULT_PASS;
}

/* ── Emitter round trip ──────────────────────────────────── */

/* Every valid case must survive parse -> emit -> parse with the same
 * meaning: event types, scalar values, anchors, tags, alias names, and
 * for untagged scalars the type they resolve to under the core schema
 * (plain 42 is an int, quoted "42" a string). Quoting style, flow vs block
 * and positions may differ. An empty plain scalar and a plain "~" are
 * both null and compare equal (the emitter writes ~ for an empty entry in
 * a flow sequence). */

static size_t canon_str(char *out, size_t cap, yam_str s) {
    size_t n = 0;
    for (size_t i = 0; i < s.len && n + 4 < cap; i++) {
        unsigned char c = (unsigned char)s.data[i];
        if (c == '\\') { out[n++] = '\\'; out[n++] = '\\'; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c < 0x20) n += (size_t)snprintf(out + n, cap - n, "\\x%02x", c);
        else out[n++] = (char)c;
    }
    return n;
}

static size_t canon_event(const yam_event *e, char *out, size_t cap) {
    size_t n = 0;
    if (cap < 64) return 0;
    switch (e->type) {
    case YAM_EVT_STREAM_START: case YAM_EVT_STREAM_END: return 0;
    case YAM_EVT_DOC_START:      n += (size_t)snprintf(out + n, cap - n, "+DOC"); break;
    case YAM_EVT_DOC_END:        n += (size_t)snprintf(out + n, cap - n, "-DOC"); break;
    case YAM_EVT_MAPPING_START:  n += (size_t)snprintf(out + n, cap - n, "+MAP"); break;
    case YAM_EVT_MAPPING_END:    n += (size_t)snprintf(out + n, cap - n, "-MAP"); break;
    case YAM_EVT_SEQUENCE_START: n += (size_t)snprintf(out + n, cap - n, "+SEQ"); break;
    case YAM_EVT_SEQUENCE_END:   n += (size_t)snprintf(out + n, cap - n, "-SEQ"); break;
    case YAM_EVT_SCALAR:         n += (size_t)snprintf(out + n, cap - n, "=VAL"); break;
    case YAM_EVT_ALIAS:          n += (size_t)snprintf(out + n, cap - n, "=ALI *"); break;
    default: return 0;
    }
    if (e->anchor.data && e->anchor.len) {
        n += (size_t)snprintf(out + n, cap - n, " &");
        n += canon_str(out + n, cap - n, e->anchor);
    }
    if (e->tag.data && e->tag.len) {
        n += (size_t)snprintf(out + n, cap - n, " <");
        n += canon_str(out + n, cap - n, e->tag);
        n += (size_t)snprintf(out + n, cap - n, ">");
    }
    if (e->type == YAM_EVT_SCALAR) {
        bool plain = e->scalar_style == YAM_SCALAR_PLAIN;
        bool untagged = !(e->tag.data && e->tag.len);
        bool null_text = untagged && plain && (e->value.len == 0 ||
                                   (e->value.len == 1 && e->value.data[0] == '~'));
        if (null_text) {
            n += (size_t)snprintf(out + n, cap - n, " :~");
        } else {
            if (!(e->tag.data && e->tag.len)) {
                /* untagged: the core schema type is part of the meaning */
                yam_str t = yam_schema_resolve(yam_schema_core(), e->value, e->scalar_style);
                const char *short_tag = strrchr(t.data, ':');
                n += (size_t)snprintf(out + n, cap - n, " (%.*s)",
                                      (int)(t.len - (size_t)(short_tag + 1 - t.data)),
                                      short_tag + 1);
            }
            n += (size_t)snprintf(out + n, cap - n, " :");
            n += canon_str(out + n, cap - n, e->value);
        }
    } else if (e->type == YAM_EVT_ALIAS) {
        n += canon_str(out + n, cap - n, e->value);
    }
    if (n + 2 < cap) out[n++] = '\n';
    out[n] = '\0';
    return n;
}

/* Parse `yaml` and write its canonical event stream to `out`. When `e` is
 * given, also feed every event to it. Returns false on a parse error. */
static bool canon_parse(const char *yaml, size_t len, bool eager, yam_emitter *e,
                        char *out, size_t cap) {
    yam_arena *a = yam_arena_new(4096);
    yam_parser *p = yam_parser_new(yaml, len, a);
    if (eager) yam_parser_set_merge(p, true);
    yam_parser_set_max_events(p, 0);
    const yam_event *evt;
    yam_status st;
    size_t n = 0;
    out[0] = '\0';
    while ((st = yam_parse_next(p, &evt)) == YAM_OK && evt->type != YAM_EVT_NONE) {
        if (e) yam_emit(e, evt);
        n += canon_event(evt, out + n, cap - n);
        if (evt->type == YAM_EVT_STREAM_END) break;
    }
    yam_parser_free(p);
    yam_arena_free(a);
    return st == YAM_OK;
}

static const char *style_name(yam_emit_style st) {
    return st == YAM_EMIT_BLOCK ? "block" : st == YAM_EMIT_FLOW ? "flow" : "minimal";
}

/* Round-trip one case in one mode and style; on failure describe it. */
static bool roundtrip_ok(const test_case *tc, bool eager, yam_emit_style style,
                         bool verbose) {
    static char before[65536], after[65536];
    size_t len = strlen(tc->yaml);
    yam_arena *a = yam_arena_new(4096);
    yam_emitter *e = yam_emitter_new(a);
    yam_emitter_set_style(e, style);

    bool ok = canon_parse(tc->yaml, len, eager, e, before, sizeof before);
    yam_str out = yam_emitter_output(e);
    const char *why = NULL;
    if (ok) {
        if (!canon_parse(out.data ? out.data : "", out.len, eager, NULL, after, sizeof after))
            why = "emitted YAML does not parse";
        else if (strcmp(before, after) != 0)
            why = "emitted YAML parses to different events";
    }
    if (why && verbose) {
        printf("    round trip (%s, %s): %s\n", eager ? "eager" : "incremental",
               style_name(style), why);
        printf("      emitted: %.*s\n", (int)(out.len > 300 ? 300 : out.len),
               out.data ? out.data : "");
        if (strcmp(why, "emitted YAML parses to different events") == 0) {
            /* show the first differing event */
            const char *b = before, *c = after;
            while (*b && *b == *c) {
                const char *nb = strchr(b, '\n'), *nc = strchr(c, '\n');
                if (!nb || !nc || (nb - b) != (nc - c) || memcmp(b, c, (size_t)(nb - b))) break;
                b = nb + 1; c = nc + 1;
            }
            printf("      expected: %.*s\n", (int)strcspn(b, "\n"), b);
            printf("      got:      %.*s\n", (int)strcspn(c, "\n"), c);
        }
    }
    yam_emitter_free(e);
    yam_arena_free(a);
    return why == NULL;
}

/* Valid cases whose round trip is known to fail (label:style). Listed
 * failures report XFAIL; one that starts passing reports XPASS and fails
 * the run until removed. */
static const char *known_roundtrip_failures[] = {
    NULL
};

static bool is_known_roundtrip_failure(const char *label, yam_emit_style style) {
    char key[400];
    snprintf(key, sizeof key, "%s:%s", label, style_name(style));
    for (size_t i = 0; known_roundtrip_failures[i]; i++)
        if (strcmp(known_roundtrip_failures[i], key) == 0) return true;
    return false;
}

static int rt_passed = 0, rt_failed = 0, rt_xfailed = 0, rt_xpassed = 0;

/* ── Main ────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *suite_dir = "yaml-test-suite/src";
    bool verbose = false;
    const char *filter = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else {
            filter = argv[i];
        }
    }

    printf("\nyam parser — YAML Test Suite\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    DIR *dir = opendir(suite_dir);
    if (!dir) {
        fprintf(stderr, "Cannot open %s\n", suite_dir);
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".yaml") != 0)
            continue;

        /* extract test ID */
        char id[16];
        if (nlen - 5 >= sizeof(id)) continue; /* not a suite ID */
        memcpy(id, ent->d_name, nlen - 5);
        id[nlen - 5] = '\0';

        if (filter && strstr(id, filter) == NULL) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", suite_dir, ent->d_name);

        test_case cases[16];
        int ncases = parse_test_file(path, id, cases, 16);

        for (int i = 0; i < ncases; i++) {
            test_case *tc = &cases[i];
            /* skip: true cases are excluded from the official suite */
            if (tc->skip) continue;
            total++;

            /* run in both parse modes; the worse result counts */
            test_result inc = run_test(tc, verbose, false);
            test_result eag = run_test(tc, verbose, true);
            test_result result = inc;
            if (eag == RESULT_ERROR || (eag == RESULT_FAIL && inc != RESULT_ERROR))
                result = eag;
            if (inc != eag && result == RESULT_FAIL && verbose)
                printf("    (%s mode only)\n", inc == RESULT_FAIL ? "incremental" : "eager");

            char label[320];
            if (ncases > 1)
                snprintf(label, sizeof(label), "%s:%d", tc->id, tc->index);
            else
                snprintf(label, sizeof(label), "%s", tc->id);

            if (tc->fail && is_known_accepted(label)) {
                if (result == RESULT_FAIL) {
                    xfailed++;
                    if (verbose)
                        printf("  %-8s %-50.50s " DIM "XFAIL (accepted invalid)" RESET "\n", label, tc->name);
                } else {
                    xpassed++;
                    printf("  %-8s %-50.50s " YELLOW "XPASS (now rejected: remove from known list)" RESET "\n",
                           label, tc->name);
                }
                continue;
            }

            /* valid cases must also survive an emitter round trip */
            if (result == RESULT_PASS && !tc->fail && strlen(tc->tree) > 0) {
                const yam_emit_style styles[] = { YAM_EMIT_BLOCK, YAM_EMIT_FLOW, YAM_EMIT_MINIMAL };
                for (int si = 0; si < 3; si++) {
                    bool ok = roundtrip_ok(tc, false, styles[si], verbose) &&
                              roundtrip_ok(tc, true, styles[si], verbose);
                    bool known = is_known_roundtrip_failure(label, styles[si]);
                    if (ok && !known) rt_passed++;
                    else if (!ok && known) rt_xfailed++;
                    else if (ok && known) {
                        rt_xpassed++;
                        printf("  %-8s round trip %-8s " YELLOW "XPASS (now passes: remove from known list)" RESET "\n",
                               label, style_name(styles[si]));
                    } else {
                        rt_failed++;
                        printf("  %-8s round trip %-8s " RED "FAIL" RESET "  %s\n",
                               label, style_name(styles[si]), tc->name);
                    }
                }
            }

            switch (result) {
            case RESULT_PASS:
                passed++;
                if (verbose)
                    printf("  %-8s %-50.50s " GREEN "PASS" RESET "\n", label, tc->name);
                break;
            case RESULT_FAIL:
                failed++;
                printf("  %-8s %-50.50s " RED "FAIL" RESET "\n", label, tc->name);
                break;
            case RESULT_ERROR:
                errors++;
                printf("  %-8s %-50.50s " YELLOW "ERROR" RESET "\n", label, tc->name);
                break;
            case RESULT_SKIP:
                skipped++;
                if (verbose)
                    printf("  %-8s %-50.50s " DIM "SKIP" RESET "\n", label, tc->name);
                break;
            }
        }
    }

    closedir(dir);

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  Total: %d  ", total);
    printf(GREEN "Pass: %d  " RESET, passed);
    printf(RED "Fail: %d  " RESET, failed);
    printf(YELLOW "Error: %d  " RESET, errors);
    printf(DIM "Skip: %d" RESET, skipped);
    printf("\n  Known accepted invalid (XFAIL): %d", xfailed);
    if (xpassed) printf(YELLOW "  XPASS: %d" RESET, xpassed);
    printf("\n  Emitter round trip: " GREEN "Pass: %d  " RESET RED "Fail: %d  " RESET
           "Known (XFAIL): %d", rt_passed, rt_failed, rt_xfailed);
    if (rt_xpassed) printf(YELLOW "  XPASS: %d" RESET, rt_xpassed);
    printf("\n\n");

    return (failed > 0 || errors > 0 || xpassed > 0 ||
            rt_failed > 0 || rt_xpassed > 0) ? 1 : 0;
}
