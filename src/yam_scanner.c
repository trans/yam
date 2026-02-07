/*
 * yam_scanner.c — YAML 1.2 tokenizer
 *
 * Hand-rolled, no recursion in the hot path.
 * SIMD-accelerated where it matters (plain scalar scanning, whitespace skip).
 *
 * Design:
 *   - Direct pointer into source buffer (no refill macro overhead)
 *   - Indent stack for block structure detection
 *   - Flow level counter for context switching
 */

#include "yam/yam.h"
#include "yam/yam_chars.h"
#include "yam/yam_simd.h"

#include <stdlib.h>
#include <string.h>

/* ── Indent stack ────────────────────────────────────────── */

#define INDENT_STACK_INIT 16

typedef struct {
    int  *data;
    int   len;
    int   cap;
} indent_stack;

/* ── Scanner state ───────────────────────────────────────── */

struct yam_scanner {
    /* input */
    const char *buf;
    size_t      len;

    /* cursor */
    size_t      pos;
    size_t      line;
    size_t      col;

    /* state */
    int          flow_level;
    int          indent;
    indent_stack indents;
    bool         stream_started;
    bool         stream_ended;
    bool         implicit_key_allowed;
    bool         simple_key_allowed;
    bool         at_doc_start;

    /* pending tokens (block structure can require lookahead) */
    yam_token    pending[4];
    int          pending_count;

    /* track last token's start column (0-based) for mapping indent detection */
    int          last_token_col;
    /* true if last token was a quoted scalar (for JSON-like key detection) */
    bool         last_was_quoted;

    /* arena for string duplication when needed */
    yam_arena   *arena;
};

/* ── Helpers ─────────────────────────────────────────────── */

#define PEEK(s)       ((s)->pos < (s)->len ? (uint8_t)(s)->buf[(s)->pos] : 0)
#define PEEK_AT(s, n) ((s)->pos + (n) < (s)->len ? (uint8_t)(s)->buf[(s)->pos + (n)] : 0)
#define REMAINING(s)  ((s)->len - (s)->pos)
#define AT_END(s)     ((s)->pos >= (s)->len)
#define BUF_AT(s)     ((s)->buf + (s)->pos)

static uint8_t hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static yam_mark mark(yam_scanner *s) {
    return (yam_mark){s->pos, s->line, s->col};
}

static void advance(yam_scanner *s, size_t n) {
    for (size_t i = 0; i < n && s->pos < s->len; i++) {
        if (s->buf[s->pos] == '\n') {
            s->line++;
            s->col = 1;
        } else {
            s->col++;
        }
        s->pos++;
    }
}

static void skip_break(yam_scanner *s) {
    if (s->pos >= s->len) return;
    if (s->buf[s->pos] == '\r') {
        s->pos++;
        if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;
    } else if (s->buf[s->pos] == '\n') {
        s->pos++;
    }
    s->line++;
    s->col = 1;
}

static void skip_blanks_and_comments(yam_scanner *s) {
    for (;;) {
        /* skip spaces and tabs */
        size_t skip = yam_skip_blanks(BUF_AT(s), REMAINING(s));
        advance(s, skip);

        /* comment? skip to end of line */
        if (PEEK(s) == '#') {
            size_t to_break = yam_scan_to_break(BUF_AT(s), REMAINING(s));
            advance(s, to_break);
        }

        /* line break? */
        if (yam_is_break(PEEK(s))) {
            skip_break(s);
            /* in block context, continue eating whitespace on next line */
            if (s->flow_level == 0) continue;
            /* in flow context, also continue */
            continue;
        }

        break;
    }
}

/* ── Indent management ───────────────────────────────────── */

static bool indent_push(yam_scanner *s, int col) {
    indent_stack *st = &s->indents;
    if (st->len >= st->cap) {
        int new_cap = st->cap * 2;
        int *new_data = (int *)realloc(st->data, new_cap * sizeof(int));
        if (!new_data) return false;
        st->data = new_data;
        st->cap  = new_cap;
    }
    st->data[st->len++] = s->indent;
    s->indent = col;
    return true;
}

static void indent_pop(yam_scanner *s) {
    if (s->indents.len > 0) {
        s->indent = s->indents.data[--s->indents.len];
    }
}

/* ── Token constructors ──────────────────────────────────── */

static yam_token tok_simple(yam_token_type type, yam_mark start, yam_mark end) {
    return (yam_token){
        .type  = type,
        .value = YAM_STR_NULL,
        .start = start,
        .end   = end,
    };
}

static yam_token tok_scalar(yam_str value, yam_scalar_style style,
                            yam_mark start, yam_mark end) {
    return (yam_token){
        .type         = YAM_TOK_SCALAR,
        .value        = value,
        .scalar_style = style,
        .start        = start,
        .end          = end,
    };
}

/* ── Push pending block structure tokens ─────────────────── */

static bool push_pending(yam_scanner *s, yam_token tok) {
    if (s->pending_count >= 4) return false;
    s->pending[s->pending_count++] = tok;
    return true;
}

static bool maybe_unroll_indents(yam_scanner *s, int col) {
    while (s->indent > col) {
        indent_pop(s);
    }
    return true;
}

static bool is_doc_indicator_at(const char *buf, size_t pos, size_t len);

/* ── Scan specific token types ───────────────────────────── */

static yam_status scan_plain_scalar(yam_scanner *s, yam_token *tok) {
    yam_mark start = mark(s);

    /* We use a two-phase approach:
     * Phase 1: scan single-line (zero-copy, fast)
     * Phase 2: if multiline, copy to arena buffer and continue */
    char *buf = NULL;
    size_t buf_cap = 0;
    size_t out = 0;
    const char *single_start = BUF_AT(s);
    size_t single_len = 0;
    bool in_buf = false; /* true once we've switched to arena buffer */

    /* macro to append data to the output (buffer or track single-line) */
    #define APPEND_RUN(src, len) do { \
        if (in_buf) { \
            if (out + (len) >= buf_cap) { \
                buf_cap = (out + (len)) * 2 + 64; \
                char *nb = yam_arena_alloc(s->arena, buf_cap, 1); \
                if (!nb) return YAM_ERR_MEMORY; \
                memcpy(nb, buf, out); \
                buf = nb; \
            } \
            memcpy(buf + out, (src), (len)); \
            out += (len); \
        } else { \
            single_len += (len); \
        } \
    } while(0)

    #define APPEND_CHAR(ch) do { \
        if (in_buf) { \
            if (out + 1 >= buf_cap) { \
                buf_cap = (out + 1) * 2 + 64; \
                char *nb = yam_arena_alloc(s->arena, buf_cap, 1); \
                if (!nb) return YAM_ERR_MEMORY; \
                memcpy(nb, buf, out); \
                buf = nb; \
            } \
            buf[out++] = (ch); \
        } else { \
            single_len++; \
        } \
    } while(0)

    #define SWITCH_TO_BUF() do { \
        if (!in_buf) { \
            buf_cap = single_len + REMAINING(s) + 64; \
            buf = yam_arena_alloc(s->arena, buf_cap, 1); \
            if (!buf) return YAM_ERR_MEMORY; \
            memcpy(buf, single_start, single_len); \
            out = single_len; \
            in_buf = true; \
        } \
    } while(0)

    #define TRIM_TRAILING() do { \
        if (in_buf) { \
            while (out > 0 && (buf[out-1] == ' ' || buf[out-1] == '\t')) out--; \
        } else { \
            while (single_len > 0 && (single_start[single_len-1] == ' ' || single_start[single_len-1] == '\t')) single_len--; \
        } \
    } while(0)

    for (;;) {
        /* SIMD fast scan over "boring" bytes */
        size_t run = yam_scan_plain_scalar(BUF_AT(s), REMAINING(s));
        if (run > 0) {
            if (in_buf) {
                APPEND_RUN(BUF_AT(s), run);
            } else {
                single_len += run;
            }
            s->pos += run;
            s->col += run;
        }

        if (AT_END(s)) break;

        uint8_t c = PEEK(s);

        /* ':' is only value indicator if followed by whitespace (or EOF in flow) */
        if (c == ':') {
            uint8_t next = PEEK_AT(s, 1);
            if (!yam_is_blank_or_break(next) && next != 0
                && !(s->flow_level > 0 && yam_is_flow(next))) {
                if (in_buf) { APPEND_CHAR(':'); } else { single_len++; }
                advance(s, 1);
                continue;
            }
            break;
        }

        /* '#' is only comment if preceded by whitespace */
        if (c == '#') {
            size_t cur_len = in_buf ? out : single_len;
            const char *cur_data = in_buf ? buf : single_start;
            if (cur_len > 0 && (cur_data[cur_len-1] == ' ' || cur_data[cur_len-1] == '\t')) {
                TRIM_TRAILING();
                break;
            }
            if (cur_len == 0) break;
            if (in_buf) { APPEND_CHAR('#'); } else { single_len++; }
            advance(s, 1);
            continue;
        }

        /* flow indicators in flow context end the scalar */
        if (s->flow_level > 0 && yam_is_flow(c)) break;

        /* whitespace — potential end or mid-line */
        if (yam_is_blank(c)) {
            size_t blank_run = yam_skip_blanks(BUF_AT(s), REMAINING(s));
            const char *blank_start = BUF_AT(s);
            s->pos += blank_run;
            s->col += blank_run;

            if (AT_END(s)) break;
            if (yam_is_break(PEEK(s))) {
                /* trailing blanks before break — don't include them,
                 * but continue loop so break handler can check for continuation */
                continue;
            }

            /* blanks mid-line */
            if (in_buf) {
                APPEND_RUN(blank_start, blank_run);
            } else {
                single_len += blank_run;
            }
            continue;
        }

        if (yam_is_break(c)) {
            /* check if next line continues this scalar */
            size_t save = s->pos;
            size_t save_line = s->line;
            size_t save_col = s->col;

            int break_count = 0;
            while (!AT_END(s) && yam_is_break(PEEK(s))) {
                skip_break(s);
                break_count++;
                size_t blanks = yam_skip_blanks(BUF_AT(s), REMAINING(s));
                s->pos += blanks;
                s->col += blanks;
                if (!AT_END(s) && !yam_is_break(PEEK(s))) break;
            }

            if (AT_END(s)) {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }

            int next_indent = (int)s->col - 1;
            int min_indent = s->indent + 1;
            if (min_indent < 0) min_indent = 0;

            if (next_indent < min_indent) {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }

            /* check for indicators that end the scalar */
            uint8_t nc = PEEK(s);
            /* - and ? only start block entries at indent levels where
             * a new block collection could begin (at or below current indent).
             * On deeper continuation lines, they're just content.
             * At top level (indent < 0), always terminate. */
            if ((nc == '-' || nc == '?') &&
                (s->indent < 0 || next_indent <= s->indent)) {
                uint8_t after = PEEK_AT(s, 1);
                if (yam_is_blank_or_break(after) || after == 0) {
                    s->pos = save; s->line = save_line; s->col = save_col;
                    break;
                }
            }
            /* : followed by whitespace always ends (it introduces a mapping value) */
            if (nc == ':') {
                uint8_t after = PEEK_AT(s, 1);
                if (yam_is_blank_or_break(after) || after == 0) {
                    s->pos = save; s->line = save_line; s->col = save_col;
                    break;
                }
            }
            if (nc == '#') {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }
            /* & and * could be anchors/aliases — they terminate the
             * scalar only at indent levels where a new node could start
             * (at or below current indent). On deeper continuation lines
             * they're just plain text. At top level, terminate at col 0. */
            if ((nc == '&' || nc == '*') &&
                next_indent <= s->indent + (s->indent < 0 ? 1 : 0)) {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }
            if (s->col == 1 && REMAINING(s) >= 3) {
                if ((nc == '-' && PEEK_AT(s,1) == '-' && PEEK_AT(s,2) == '-') ||
                    (nc == '.' && PEEK_AT(s,1) == '.' && PEEK_AT(s,2) == '.')) {
                    if (REMAINING(s) == 3 || yam_is_blank_or_break(PEEK_AT(s,3))) {
                        s->pos = save; s->line = save_line; s->col = save_col;
                        break;
                    }
                }
            }

            /* it IS a continuation — switch to buffer mode */
            TRIM_TRAILING();
            SWITCH_TO_BUF();

            /* fold breaks: 1 break → space, N breaks → (N-1) newlines */
            if (break_count == 1) {
                APPEND_CHAR(' ');
            } else {
                for (int i = 1; i < break_count; i++) {
                    APPEND_CHAR('\n');
                }
            }
            continue;
        }

        /* anything else is part of the scalar */
        if (in_buf) {
            APPEND_CHAR(s->buf[s->pos]);
        } else {
            single_len++;
        }
        advance(s, 1);
    }

    TRIM_TRAILING();

    yam_mark end = mark(s);
    s->last_token_col = (int)start.col - 1;  /* 0-based col of this scalar */
    if (in_buf) {
        buf[out] = '\0';
        *tok = tok_scalar((yam_str){buf, out}, YAM_SCALAR_PLAIN, start, end);
    } else {
        *tok = tok_scalar((yam_str){single_start, single_len}, YAM_SCALAR_PLAIN, start, end);
    }
    return YAM_OK;

    #undef APPEND_RUN
    #undef APPEND_CHAR
    #undef SWITCH_TO_BUF
    #undef TRIM_TRAILING
}

static yam_status scan_single_quoted(yam_scanner *s, yam_token *tok) {
    yam_mark start = mark(s);
    advance(s, 1); /* skip opening ' */

    /* Allocate buffer for the result (handles escapes and line folding) */
    size_t buf_cap = 64;
    char *buf = yam_arena_alloc(s->arena, buf_cap, 1);
    if (!buf) return YAM_ERR_MEMORY;
    size_t out = 0;

    #define SQ_ENSURE(n) do { \
        if (out + (n) >= buf_cap) { \
            buf_cap = (out + (n)) * 2 + 64; \
            char *nb = yam_arena_alloc(s->arena, buf_cap, 1); \
            if (!nb) return YAM_ERR_MEMORY; \
            memcpy(nb, buf, out); \
            buf = nb; \
        } \
    } while(0)

    while (!AT_END(s)) {
        if (PEEK(s) == '\'') {
            if (s->pos + 1 < s->len && s->buf[s->pos + 1] == '\'') {
                /* escaped single quote '' → ' */
                SQ_ENSURE(1);
                buf[out++] = '\'';
                advance(s, 2);
                continue;
            }
            break; /* closing quote */
        }
        if (yam_is_break(PEEK(s))) {
            /* line folding: trim trailing blanks, fold breaks */
            while (out > 0 && (buf[out-1] == ' ' || buf[out-1] == '\t')) out--;
            int break_count = 0;
            while (!AT_END(s) && yam_is_break(PEEK(s))) {
                skip_break(s);
                break_count++;
                while (!AT_END(s) && yam_is_blank(PEEK(s))) advance(s, 1);
                if (!AT_END(s) && !yam_is_break(PEEK(s))) break;
            }
            /* document indicators at start of line in multi-line quoted scalar → error */
            if (s->col == 1 && is_doc_indicator_at(s->buf, s->pos, s->len))
                return YAM_ERR_SCAN;
            SQ_ENSURE(break_count);
            if (break_count == 1) {
                buf[out++] = ' ';
            } else {
                for (int i = 1; i < break_count; i++)
                    buf[out++] = '\n';
            }
        } else {
            SQ_ENSURE(1);
            buf[out++] = s->buf[s->pos];
            advance(s, 1);
        }
    }

    if (PEEK(s) != '\'') return YAM_ERR_SCAN; /* unterminated */
    advance(s, 1); /* skip closing ' */

    buf[out] = '\0';
    s->last_token_col = (int)start.col - 1;
    s->last_was_quoted = true;
    *tok = tok_scalar((yam_str){buf, out}, YAM_SCALAR_SINGLE_QUOTED, start, mark(s));
    return YAM_OK;

    #undef SQ_ENSURE
}

static yam_status scan_double_quoted(yam_scanner *s, yam_token *tok) {
    yam_mark start = mark(s);
    advance(s, 1); /* skip opening " */

    /* double-quoted always needs processing for escape sequences */
    /* pre-scan for length */
    size_t max_len = 0;
    size_t scan = s->pos;
    while (scan < s->len && s->buf[scan] != '"') {
        if (s->buf[scan] == '\\') scan++; /* skip escaped char */
        scan++;
        max_len++;
    }

    char *buf = yam_arena_alloc(s->arena, max_len + 1, 1);
    if (!buf) return YAM_ERR_MEMORY;

    size_t out = 0;
    size_t content_end = 0; /* tracks end of non-literal-whitespace content for trimming */
    while (!AT_END(s) && PEEK(s) != '"') {
        if (PEEK(s) == '\\') {
            advance(s, 1);
            if (AT_END(s)) return YAM_ERR_SCAN;

            uint8_t esc = PEEK(s);
            advance(s, 1);
            switch (esc) {
                case '0':  buf[out++] = '\0'; break;
                case 'a':  buf[out++] = '\a'; break;
                case 'b':  buf[out++] = '\b'; break;
                case 't':  buf[out++] = '\t'; break;
                case '\t': buf[out++] = '\t'; break;
                case 'n':  buf[out++] = '\n'; break;
                case 'v':  buf[out++] = '\v'; break;
                case 'f':  buf[out++] = '\f'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 'e':  buf[out++] = '\x1B'; break;
                case ' ':  buf[out++] = ' '; break;
                case '"':  buf[out++] = '"'; break;
                case '/':  buf[out++] = '/'; break;
                case '\\': buf[out++] = '\\'; break;
                case 'N':  /* NEL U+0085 — encode as UTF-8 */
                    buf[out++] = (char)0xC2;
                    buf[out++] = (char)0x85;
                    break;
                case '_':  /* NBSP U+00A0 */
                    buf[out++] = (char)0xC2;
                    buf[out++] = (char)0xA0;
                    break;
                case 'x': { /* \xNN */
                    if (REMAINING(s) < 2) return YAM_ERR_SCAN;
                    uint8_t hi = PEEK(s), lo = PEEK_AT(s, 1);
                    if (!yam_is_hex(hi) || !yam_is_hex(lo)) return YAM_ERR_SCAN;
                    uint8_t byte = (hex_digit(hi) << 4) | hex_digit(lo);
                    advance(s, 2);
                    buf[out++] = (char)byte;
                    break;
                }
                case 'u': /* \uNNNN */
                case 'U': { /* \UNNNNNNNN */
                    int ndigits = (esc == 'u') ? 4 : 8;
                    if ((int)REMAINING(s) < ndigits) return YAM_ERR_SCAN;
                    uint32_t cp = 0;
                    for (int i = 0; i < ndigits; i++) {
                        uint8_t c = PEEK_AT(s, i);
                        if (!yam_is_hex(c)) return YAM_ERR_SCAN;
                        cp = (cp << 4) | hex_digit(c);
                    }
                    advance(s, ndigits);
                    if (cp > 0x10FFFF) return YAM_ERR_SCAN;
                    if (cp >= 0xD800 && cp <= 0xDFFF) return YAM_ERR_SCAN;
                    /* encode as UTF-8 */
                    if (cp <= 0x7F) {
                        buf[out++] = (char)cp;
                    } else if (cp <= 0x7FF) {
                        buf[out++] = (char)(0xC0 | (cp >> 6));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp <= 0xFFFF) {
                        buf[out++] = (char)(0xE0 | (cp >> 12));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[out++] = (char)(0xF0 | (cp >> 18));
                        buf[out++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                case '\n': /* escaped newline — line folding */
                case '\r':
                    /* skip the break and any leading whitespace on next line */
                    if (esc == '\r' && PEEK(s) == '\n') advance(s, 1);
                    while (!AT_END(s) && yam_is_blank(PEEK(s))) advance(s, 1);
                    content_end = out; /* escaped newline doesn't affect trim */
                    continue; /* skip content_end update below */
                default:
                    return YAM_ERR_SCAN; /* invalid escape */
            }
            /* all escape sequences produce content (not trimmable whitespace) */
            content_end = out;
        } else if (yam_is_break(PEEK(s))) {
            /* line folding: 1 break→space, empty lines→\n each */
            /* trim trailing literal whitespace (escapes are content) */
            out = content_end;
            int break_count = 0;
            while (!AT_END(s) && yam_is_break(PEEK(s))) {
                skip_break(s);
                break_count++;
                while (!AT_END(s) && yam_is_blank(PEEK(s))) advance(s, 1);
                if (!AT_END(s) && !yam_is_break(PEEK(s))) break;
            }
            /* document indicators at start of line in multi-line quoted scalar → error */
            if (s->col == 1 && is_doc_indicator_at(s->buf, s->pos, s->len))
                return YAM_ERR_SCAN;
            if (break_count == 1) {
                buf[out++] = ' ';
            } else {
                for (int i = 1; i < break_count; i++)
                    buf[out++] = '\n';
            }
        } else {
            uint8_t ch = (uint8_t)s->buf[s->pos];
            buf[out++] = s->buf[s->pos];
            advance(s, 1);
            if (!yam_is_blank(ch)) content_end = out;
        }
    }

    if (PEEK(s) != '"') return YAM_ERR_SCAN;
    advance(s, 1);

    buf[out] = '\0';
    s->last_token_col = (int)start.col - 1;
    s->last_was_quoted = true;
    *tok = tok_scalar((yam_str){buf, out}, YAM_SCALAR_DOUBLE_QUOTED, start, mark(s));
    return YAM_OK;
}

static yam_status scan_tag(yam_scanner *s, yam_token *tok) {
    yam_mark start = mark(s);
    const char *tag_start = BUF_AT(s);
    advance(s, 1); /* skip ! */

    if (PEEK(s) == '!') {
        advance(s, 1); /* !! secondary tag */
    } else if (PEEK(s) == '<') {
        /* verbatim tag !<...> */
        advance(s, 1);
        while (!AT_END(s) && PEEK(s) != '>') advance(s, 1);
        if (PEEK(s) == '>') advance(s, 1);
        size_t len = BUF_AT(s) - tag_start;
        s->last_token_col = (int)start.col - 1;
        *tok = (yam_token){
            .type  = YAM_TOK_TAG,
            .value = {tag_start, len},
            .start = start,
            .end   = mark(s),
        };
        return YAM_OK;
    }

    /* consume tag characters */
    while (!AT_END(s) && !yam_is_blank_or_break(PEEK(s))
           && !yam_is_flow(PEEK(s))) {
        advance(s, 1);
    }

    size_t len = BUF_AT(s) - tag_start;
    s->last_token_col = (int)start.col - 1;
    *tok = (yam_token){
        .type  = YAM_TOK_TAG,
        .value = {tag_start, len},
        .start = start,
        .end   = mark(s),
    };
    return YAM_OK;
}

static yam_status scan_anchor_or_alias(yam_scanner *s, yam_token *tok) {
    yam_mark start = mark(s);
    bool is_anchor = (PEEK(s) == '&');
    advance(s, 1); /* skip & or * */

    const char *name_start = BUF_AT(s);
    while (!AT_END(s)) {
        uint8_t ch = PEEK(s);
        /* anchor name: any non-whitespace, non-flow indicator character */
        if (yam_is_blank_or_break(ch) || ch == 0) break;
        if (ch == ',' || ch == '[' || ch == ']' || ch == '{' || ch == '}') break;
        advance(s, 1);
    }
    size_t name_len = BUF_AT(s) - name_start;
    if (name_len == 0) return YAM_ERR_SCAN;

    s->last_token_col = (int)start.col - 1;
    *tok = (yam_token){
        .type  = is_anchor ? YAM_TOK_ANCHOR : YAM_TOK_ALIAS,
        .value = {name_start, name_len},
        .start = start,
        .end   = mark(s),
    };
    return YAM_OK;
}

/* Check for --- or ... at line start */
/* check for document indicator (--- or ...) at a buffer position */
static bool is_doc_indicator_at(const char *buf, size_t pos, size_t len) {
    if (pos + 3 > len) return false;
    char c = buf[pos];
    if (c != '-' && c != '.') return false;
    if (buf[pos + 1] != c || buf[pos + 2] != c) return false;
    if (pos + 3 == len) return true;
    return yam_is_blank_or_break((uint8_t)buf[pos + 3]);
}

static bool at_doc_indicator(yam_scanner *s, char ch) {
    if (s->col != 1) return false;
    if (REMAINING(s) < 3) return false;
    return s->buf[s->pos]     == ch
        && s->buf[s->pos + 1] == ch
        && s->buf[s->pos + 2] == ch
        && (REMAINING(s) == 3 || yam_is_blank_or_break(PEEK_AT(s, 3)));
}

/* ── Main scan function ──────────────────────────────────── */

yam_scanner *yam_scanner_new(const char *input, size_t len, yam_arena *a) {
    yam_scanner *s = (yam_scanner *)malloc(sizeof(yam_scanner));
    if (!s) return NULL;

    *s = (yam_scanner){
        .buf   = input,
        .len   = len,
        .pos   = 0,
        .line  = 1,
        .col   = 1,
        .flow_level = 0,
        .indent     = -1,
        .stream_started = false,
        .stream_ended   = false,
        .implicit_key_allowed = true,
        .simple_key_allowed   = true,
        .at_doc_start         = true,
        .pending_count        = 0,
        .last_token_col       = 0,
        .arena = a,
    };

    s->indents.cap  = INDENT_STACK_INIT;
    s->indents.len  = 0;
    s->indents.data = (int *)malloc(INDENT_STACK_INIT * sizeof(int));
    if (!s->indents.data) { free(s); return NULL; }

    return s;
}

yam_status yam_scan_next(yam_scanner *s, yam_token *tok) {
    /* drain pending tokens first */
    if (s->pending_count > 0) {
        *tok = s->pending[0];
        memmove(&s->pending[0], &s->pending[1],
                (--s->pending_count) * sizeof(yam_token));
        return YAM_OK;
    }

    /* stream start */
    if (!s->stream_started) {
        s->stream_started = true;
        *tok = tok_simple(YAM_TOK_STREAM_START, mark(s), mark(s));
        return YAM_OK;
    }

    /* skip whitespace and comments */
    skip_blanks_and_comments(s);

    /* stream end */
    if (AT_END(s)) {
        if (!s->stream_ended) {
            s->stream_ended = true;
            /* unwind all indents */
            maybe_unroll_indents(s, -1);
            *tok = tok_simple(YAM_TOK_STREAM_END, mark(s), mark(s));
            return YAM_OK;
        }
        *tok = tok_simple(YAM_TOK_NONE, mark(s), mark(s));
        return YAM_OK;
    }

    /* unroll indent stack to current column (block context only).
     * This ensures s->indent reflects the actual nesting depth,
     * which is needed for plain scalar continuation checks. */
    if (s->flow_level == 0) {
        int cur_col = (int)s->col - 1;  /* 0-based */
        maybe_unroll_indents(s, cur_col);
    }

    yam_mark start = mark(s);
    uint8_t c = PEEK(s);

    /* document indicators at column 1 */
    if (at_doc_indicator(s, '-')) {
        maybe_unroll_indents(s, -1);
        advance(s, 3);
        *tok = tok_simple(YAM_TOK_DOC_START, start, mark(s));
        return YAM_OK;
    }
    if (at_doc_indicator(s, '.')) {
        maybe_unroll_indents(s, -1);
        advance(s, 3);
        *tok = tok_simple(YAM_TOK_DOC_END, start, mark(s));
        return YAM_OK;
    }

    /* directive lines: % at column 1 in block context → consume whole line as scalar */
    if (c == '%' && s->col == 1 && s->flow_level == 0) {
        const char *scalar_start = BUF_AT(s);
        size_t len = 0;
        while (!AT_END(s) && !yam_is_break(PEEK(s))) {
            advance(s, 1);
            len++;
        }
        tok->type = YAM_TOK_SCALAR;
        tok->start = start;
        tok->end = mark(s);
        tok->value = (yam_str){scalar_start, len};
        tok->scalar_style = YAM_SCALAR_PLAIN;
        return YAM_OK;
    }

    /* flow indicators */
    switch (c) {
    case '[':
        s->flow_level++;
        advance(s, 1);
        *tok = tok_simple(YAM_TOK_FLOW_SEQ_START, start, mark(s));
        return YAM_OK;
    case ']':
        if (s->flow_level > 0) s->flow_level--;
        advance(s, 1);
        s->last_was_quoted = true; /* allow ]:value like "key":value */
        *tok = tok_simple(YAM_TOK_FLOW_SEQ_END, start, mark(s));
        return YAM_OK;
    case '{':
        s->flow_level++;
        advance(s, 1);
        *tok = tok_simple(YAM_TOK_FLOW_MAP_START, start, mark(s));
        return YAM_OK;
    case '}':
        if (s->flow_level > 0) s->flow_level--;
        advance(s, 1);
        s->last_was_quoted = true; /* allow }:value like "key":value */
        *tok = tok_simple(YAM_TOK_FLOW_MAP_END, start, mark(s));
        return YAM_OK;
    case ',':
        advance(s, 1);
        *tok = tok_simple(YAM_TOK_FLOW_ENTRY, start, mark(s));
        return YAM_OK;
    }

    /* block sequence entry: - followed by whitespace */
    if (c == '-' && s->flow_level == 0) {
        uint8_t next = PEEK_AT(s, 1);
        if (yam_is_blank_or_break(next) || next == 0) {
            int col = (int)s->col - 1;
            if (col > s->indent) {
                indent_push(s, col);
            }
            advance(s, 1);
            *tok = tok_simple(YAM_TOK_BLOCK_SEQ_ENTRY, start, mark(s));
            return YAM_OK;
        }
    }

    /* explicit mapping key: ? followed by whitespace (both block and flow) */
    if (c == '?' && s->flow_level == 0) {
        uint8_t next = PEEK_AT(s, 1);
        if (yam_is_blank_or_break(next) || next == 0) {
            int qcol = (int)s->col - 1;
            if (qcol > s->indent) {
                indent_push(s, qcol);
            }
            advance(s, 1);
            *tok = tok_simple(YAM_TOK_BLOCK_MAP_KEY, start, mark(s));
            return YAM_OK;
        }
    }
    /* ? in flow context */
    if (c == '?' && s->flow_level > 0) {
        uint8_t next = PEEK_AT(s, 1);
        if (yam_is_blank_or_break(next) || next == 0) {
            advance(s, 1);
            *tok = tok_simple(YAM_TOK_BLOCK_MAP_KEY, start, mark(s));
            return YAM_OK;
        }
    }

    /* block mapping value: : followed by whitespace */
    if (c == ':') {
        uint8_t next = PEEK_AT(s, 1);
        if (s->flow_level == 0 && (yam_is_blank_or_break(next) || next == 0)) {
            int colon_col = (int)s->col - 1;
            int key_col = s->last_token_col;  /* 0-based col of the key */
            /* use minimum of key and colon column — handles explicit key
             * (? key\n: val) where colon_col is the mapping indent */
            int map_col = key_col < colon_col ? key_col : colon_col;
            if (map_col > s->indent) {
                indent_push(s, map_col);
            }
            advance(s, 1);
            *tok = tok_simple(YAM_TOK_BLOCK_MAP_VALUE, start, mark(s));
            return YAM_OK;
        }
        if (s->flow_level > 0 &&
            (yam_is_blank_or_break(next) || next == 0 ||
             next == ',' || next == ']' || next == '}' ||
             /* `:` after a JSON-like key (quoted scalar) */
             s->last_was_quoted)) {
            s->last_was_quoted = false; /* consumed */
            advance(s, 1);
            *tok = tok_simple(YAM_TOK_BLOCK_MAP_VALUE, start, mark(s));
            return YAM_OK;
        }
    }

    /* reset quoted-key tracker — the : check above already consumed it */
    s->last_was_quoted = false;

    /* tag */
    if (c == '!') return scan_tag(s, tok);

    /* anchor / alias */
    if (c == '&' || c == '*') return scan_anchor_or_alias(s, tok);

    /* quoted scalars */
    if (c == '\'') return scan_single_quoted(s, tok);
    if (c == '"')  return scan_double_quoted(s, tok);

    /* block scalar (literal | or folded >) */
    if ((c == '|' || c == '>') && s->flow_level == 0) {
        yam_scalar_style style = (c == '|') ? YAM_SCALAR_LITERAL : YAM_SCALAR_FOLDED;
        advance(s, 1); /* skip | or > */

        /* parse optional chomping and indent indicators (any order) */
        int chomp = 0;  /* 0=clip, -1=strip, 1=keep */
        int explicit_indent = 0;

        for (int i = 0; i < 2 && !AT_END(s) && !yam_is_break(PEEK(s)); i++) {
            uint8_t ch = PEEK(s);
            if (ch == '-') { chomp = -1; advance(s, 1); }
            else if (ch == '+') { chomp = 1; advance(s, 1); }
            else if (ch >= '1' && ch <= '9') { explicit_indent = ch - '0'; advance(s, 1); }
            else break;
        }

        /* skip any trailing blanks and comment on indicator line */
        while (!AT_END(s) && yam_is_blank(PEEK(s))) advance(s, 1);
        if (!AT_END(s) && PEEK(s) == '#') {
            while (!AT_END(s) && !yam_is_break(PEEK(s))) advance(s, 1);
        }
        if (!AT_END(s) && !yam_is_break(PEEK(s))) return YAM_ERR_SCAN;
        if (!AT_END(s)) skip_break(s);

        /* determine content indent level */
        int base_indent;
        if (explicit_indent > 0) {
            base_indent = s->indent + explicit_indent;
            if (base_indent < explicit_indent) base_indent = explicit_indent;
        } else {
            /* auto-detect from first non-empty content line */
            base_indent = -1; /* sentinel: not yet detected */
            size_t probe = s->pos;
            while (probe < s->len) {
                int li = 0;
                while (probe + li < s->len && s->buf[probe + li] == ' ') li++;
                if (probe + li >= s->len) break;
                /* document indicators terminate block scalar */
                if (li == 0 && is_doc_indicator_at(s->buf, probe, s->len)) break;
                if (yam_is_break((uint8_t)s->buf[probe + li])) {
                    /* empty line — skip */
                    probe += li;
                    if (probe < s->len && s->buf[probe] == '\r') probe++;
                    if (probe < s->len && s->buf[probe] == '\n') probe++;
                    continue;
                }
                /* content must be indented deeper than the current block level */
                int min_indent = s->indent + 1;
                if (min_indent < 0) min_indent = 0;
                if (li >= min_indent) {
                    base_indent = li;
                }
                break;
            }
            if (base_indent < 0) {
                /* no content lines found — use default */
                base_indent = s->indent + 1;
                if (base_indent < 1) base_indent = 1;
            }
        }

        /* collect raw lines into arena buffer */
        /* first pass: measure size needed */
        size_t save_pos = s->pos, save_line = s->line, save_col = s->col;
        size_t needed = 0;
        while (s->pos < s->len) {
            int li = 0;
            while (s->pos + li < s->len && s->buf[s->pos + li] == ' ') li++;
            if (s->pos + li >= s->len) {
                /* trailing blank line at end */
                needed++;
                s->pos += li;
                break;
            }
            if (yam_is_break((uint8_t)s->buf[s->pos + li])) {
                if (li > base_indent) {
                    /* whitespace-only line with extra spaces beyond base indent:
                     * treat as content line (extra spaces are preserved) */
                    size_t rest = li - base_indent;
                    needed += rest + 1;
                    s->pos += li;
                    if (s->pos < s->len && s->buf[s->pos] == '\r') s->pos++;
                    if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;
                } else {
                    needed++; /* empty line → newline */
                    s->pos += li;
                    if (s->pos < s->len && s->buf[s->pos] == '\r') s->pos++;
                    if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;
                }
                continue;
            }
            /* document indicators at col 0 always terminate block scalar */
            if (li == 0 && is_doc_indicator_at(s->buf, s->pos, s->len)) break;
            if (li < base_indent) break;
            size_t line_start = s->pos + base_indent;
            size_t rest = 0;
            while (line_start + rest < s->len && !yam_is_break((uint8_t)s->buf[line_start + rest]))
                rest++;
            needed += rest + 1; /* content + newline */
            s->pos = line_start + rest;
            if (s->pos < s->len && s->buf[s->pos] == '\r') s->pos++;
            if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;
        }
        size_t end_pos = s->pos;
        /* restore and do real pass */
        s->pos = save_pos; s->line = save_line; s->col = save_col;

        char *buf = yam_arena_alloc(s->arena, needed + 1, 1);
        if (!buf) return YAM_ERR_MEMORY;
        size_t out = 0;

        /* track line structure for folded mode */
        typedef struct { size_t start; size_t len; bool empty; int extra_indent; } bsline;
        bsline *lines = NULL;
        int nlines = 0, lines_cap = 0;
        if (style == YAM_SCALAR_FOLDED) {
            /* count lines for allocation */
            size_t tp = s->pos;
            int cnt = 0;
            while (tp < end_pos) {
                while (tp < end_pos && !yam_is_break((uint8_t)s->buf[tp])) tp++;
                cnt++;
                if (tp < end_pos && s->buf[tp] == '\r') tp++;
                if (tp < end_pos && s->buf[tp] == '\n') tp++;
            }
            lines_cap = cnt + 1;
            lines = (bsline *)yam_arena_alloc(s->arena, lines_cap * sizeof(bsline), sizeof(void*));
            if (!lines) return YAM_ERR_MEMORY;
        }

        while (s->pos < end_pos) {
            int li = 0;
            while (s->pos + li < s->len && s->buf[s->pos + li] == ' ') li++;
            bool line_is_break = (s->pos + li >= end_pos) ||
                                 (s->pos + li < s->len && yam_is_break((uint8_t)s->buf[s->pos + li]));
            if (line_is_break && li <= base_indent) {
                /* empty line (indent at or below base, no extra content spaces) */
                if (style == YAM_SCALAR_FOLDED && lines) {
                    lines[nlines++] = (bsline){out, 0, true, 0};
                }
                buf[out++] = '\n';
                advance(s, li);
                if (!AT_END(s) && yam_is_break(PEEK(s))) skip_break(s);
                continue;
            }
            /* whitespace-only lines ABOVE base indent (li > base_indent)
             * fall through to content handler (extra spaces are content) */
            if (!line_is_break && li < base_indent) break;
            int extra = li - base_indent;
            advance(s, base_indent);

            size_t line_content_start = out;
            /* copy extra indent spaces */
            for (int j = 0; j < extra; j++) {
                buf[out++] = ' ';
                advance(s, 1);
            }
            /* check if content starts with whitespace (tab counts as more-indented) */
            bool starts_with_ws = (extra > 0) ||
                (!AT_END(s) && !yam_is_break(PEEK(s)) && yam_is_blank(PEEK(s)));
            /* copy line content */
            while (!AT_END(s) && !yam_is_break(PEEK(s))) {
                buf[out++] = s->buf[s->pos];
                advance(s, 1);
            }

            if (style == YAM_SCALAR_FOLDED && lines) {
                lines[nlines++] = (bsline){line_content_start, out - line_content_start, false,
                                           starts_with_ws ? 1 : 0};
            }

            buf[out++] = '\n';
            if (!AT_END(s) && yam_is_break(PEEK(s))) skip_break(s);
        }

        /* apply folding for '>' style */
        if (style == YAM_SCALAR_FOLDED && lines && nlines > 0) {
            char *fbuf = yam_arena_alloc(s->arena, out + 1, 1);
            if (!fbuf) return YAM_ERR_MEMORY;
            size_t fout = 0;

            int i = 0;
            while (i < nlines) {
                if (lines[i].empty) {
                    /* leading empty lines (before any content) → preserve */
                    fbuf[fout++] = '\n';
                    i++;
                    continue;
                }

                /* non-empty line: emit its content */
                memcpy(fbuf + fout, buf + lines[i].start, lines[i].len);
                fout += lines[i].len;

                bool more_indented = lines[i].extra_indent > 0;

                /* count following empty lines */
                int empty_count = 0;
                int j = i + 1;
                while (j < nlines && lines[j].empty) {
                    empty_count++;
                    j++;
                }

                if (j >= nlines) {
                    /* no more content after this — final break + any trailing empties */
                    fbuf[fout++] = '\n';
                    for (int k = 0; k < empty_count; k++)
                        fbuf[fout++] = '\n';
                } else if (more_indented) {
                    /* more-indented: always preserve break */
                    fbuf[fout++] = '\n';
                    /* plus any empty lines */
                    for (int k = 0; k < empty_count; k++)
                        fbuf[fout++] = '\n';
                } else if (empty_count > 0) {
                    /* normal line → empty line(s):
                     * if next content is more-indented, fold break is preserved
                     * (empty_count + 1), otherwise absorbed (empty_count) */
                    int n = empty_count;
                    if (j < nlines && lines[j].extra_indent > 0) n++;
                    for (int k = 0; k < n; k++)
                        fbuf[fout++] = '\n';
                } else if (lines[j].extra_indent > 0) {
                    /* next is more-indented: preserve break */
                    fbuf[fout++] = '\n';
                } else {
                    /* next is non-empty, same indent: fold to space */
                    fbuf[fout++] = ' ';
                }
                i = j; /* skip past empty lines we already counted */
            }
            buf = fbuf;
            out = fout;
        }

        /* apply chomping to trailing newlines */
        if (chomp == -1) {
            /* strip: remove all trailing newlines */
            while (out > 0 && buf[out - 1] == '\n') out--;
        } else if (chomp == 0) {
            /* clip: keep at most one trailing newline after content.
             * If there is no non-newline content, result is empty. */
            /* check if there's any non-newline content */
            bool has_content = false;
            for (size_t ci = 0; ci < out; ci++) {
                if (buf[ci] != '\n') { has_content = true; break; }
            }
            if (!has_content) {
                out = 0;
            } else {
                while (out > 1 && buf[out - 1] == '\n' && buf[out - 2] == '\n') out--;
                if (out > 0 && buf[out - 1] != '\n') buf[out++] = '\n';
            }
        }
        /* keep (chomp == 1): preserve all trailing newlines as-is */

        buf[out] = '\0';
        *tok = tok_scalar((yam_str){buf, out}, style, start, mark(s));
        return YAM_OK;
    }

    /* plain scalar — the common case, SIMD-accelerated */
    return scan_plain_scalar(s, tok);
}

void yam_scanner_free(yam_scanner *s) {
    if (!s) return;
    free(s->indents.data);
    free(s);
}

/* ── String utilities ────────────────────────────────────── */

const char *yam_status_str(yam_status s) {
    switch (s) {
        case YAM_OK:         return "ok";
        case YAM_ERR_MEMORY: return "memory error";
        case YAM_ERR_INPUT:  return "input error";
        case YAM_ERR_SCAN:   return "scan error";
        case YAM_ERR_PARSE:  return "parse error";
        case YAM_ERR_EMIT:   return "emit error";
    }
    return "unknown";
}

const char *yam_token_type_str(yam_token_type t) {
    switch (t) {
        case YAM_TOK_NONE:            return "NONE";
        case YAM_TOK_STREAM_START:    return "STREAM_START";
        case YAM_TOK_STREAM_END:      return "STREAM_END";
        case YAM_TOK_DOC_START:       return "DOC_START";
        case YAM_TOK_DOC_END:         return "DOC_END";
        case YAM_TOK_BLOCK_SEQ_ENTRY: return "BLOCK_SEQ_ENTRY";
        case YAM_TOK_BLOCK_MAP_KEY:   return "BLOCK_MAP_KEY";
        case YAM_TOK_BLOCK_MAP_VALUE: return "BLOCK_MAP_VALUE";
        case YAM_TOK_FLOW_SEQ_START:  return "FLOW_SEQ_START";
        case YAM_TOK_FLOW_SEQ_END:    return "FLOW_SEQ_END";
        case YAM_TOK_FLOW_MAP_START:  return "FLOW_MAP_START";
        case YAM_TOK_FLOW_MAP_END:    return "FLOW_MAP_END";
        case YAM_TOK_FLOW_ENTRY:      return "FLOW_ENTRY";
        case YAM_TOK_SCALAR:          return "SCALAR";
        case YAM_TOK_TAG:             return "TAG";
        case YAM_TOK_ANCHOR:          return "ANCHOR";
        case YAM_TOK_ALIAS:           return "ALIAS";
    }
    return "UNKNOWN";
}

const char *yam_event_type_str(yam_event_type t) {
    switch (t) {
        case YAM_EVT_NONE:           return "NONE";
        case YAM_EVT_STREAM_START:   return "STREAM_START";
        case YAM_EVT_STREAM_END:     return "STREAM_END";
        case YAM_EVT_DOC_START:      return "DOC_START";
        case YAM_EVT_DOC_END:        return "DOC_END";
        case YAM_EVT_MAPPING_START:  return "MAPPING_START";
        case YAM_EVT_MAPPING_END:    return "MAPPING_END";
        case YAM_EVT_SEQUENCE_START: return "SEQUENCE_START";
        case YAM_EVT_SEQUENCE_END:   return "SEQUENCE_END";
        case YAM_EVT_SCALAR:         return "SCALAR";
        case YAM_EVT_ALIAS:          return "ALIAS";
    }
    return "UNKNOWN";
}
