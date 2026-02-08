/*
 * yam_emitter.c — YAML 1.2 event emitter
 *
 * Consumes parser events and produces YAML text in three styles:
 *   - BLOCK: standard indented YAML (default)
 *   - FLOW:  JSON-like inline with spaces
 *   - MINIMAL: compact flow, no optional whitespace
 */

#include "yam/yam.h"
#include "yam/yam_chars.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Context types ───────────────────────────────────────── */

typedef enum {
    EMIT_CTX_BLOCK_MAP,
    EMIT_CTX_BLOCK_SEQ,
    EMIT_CTX_FLOW_MAP,
    EMIT_CTX_FLOW_SEQ,
} emit_ctx_type;

typedef struct {
    emit_ctx_type type;
    int           indent;     /* absolute column for this level */
    int           count;      /* entries emitted so far */
    bool          expect_key; /* in mappings: true=key, false=value */
} emit_ctx;

/* ── Emitter state ───────────────────────────────────────── */

struct yam_emitter {
    yam_emit_opts opts;
    yam_arena    *arena;

    /* output buffer (malloc'd) */
    char   *buf;
    size_t  len;
    size_t  cap;
    int     column;

    /* context stack */
    emit_ctx *stack;
    int       stack_len;
    int       stack_cap;

    /* state */
    bool doc_open;
    bool first_doc;
    bool wrote_block_key; /* just wrote a block map key, value next */
    bool after_seq_dash;  /* just wrote "- " for a block seq entry */
};

/* ── Buffer management ───────────────────────────────────── */

#define EMIT_INIT_CAP 4096

static yam_status buf_ensure(yam_emitter *e, size_t need) {
    if (e->len + need <= e->cap) return YAM_OK;
    size_t new_cap = e->cap * 2;
    if (new_cap < e->len + need) new_cap = e->len + need;
    char *nb = realloc(e->buf, new_cap);
    if (!nb) return YAM_ERR_MEMORY;
    e->buf = nb;
    e->cap = new_cap;
    return YAM_OK;
}

static yam_status buf_put(yam_emitter *e, char c) {
    yam_status st = buf_ensure(e, 1);
    if (st != YAM_OK) return st;
    e->buf[e->len++] = c;
    if (c == '\n') e->column = 0;
    else e->column++;
    return YAM_OK;
}

static yam_status buf_puts(yam_emitter *e, const char *s, size_t slen) {
    yam_status st = buf_ensure(e, slen);
    if (st != YAM_OK) return st;
    memcpy(e->buf + e->len, s, slen);
    e->len += slen;
    for (size_t i = slen; i > 0; i--) {
        if (s[i - 1] == '\n') {
            e->column = (int)(slen - i);
            return YAM_OK;
        }
    }
    e->column += (int)slen;
    return YAM_OK;
}

#define PUTS(e, lit) buf_puts(e, lit, sizeof(lit) - 1)

static yam_status buf_indent(yam_emitter *e, int spaces) {
    if (spaces <= 0) return YAM_OK;
    yam_status st = buf_ensure(e, (size_t)spaces);
    if (st != YAM_OK) return st;
    memset(e->buf + e->len, ' ', (size_t)spaces);
    e->len += (size_t)spaces;
    e->column += spaces;
    return YAM_OK;
}

/* ── Context stack ───────────────────────────────────────── */

static yam_status push_ctx(yam_emitter *e, emit_ctx_type type, int indent) {
    if (e->stack_len >= e->stack_cap) {
        int new_cap = e->stack_cap * 2;
        if (new_cap < 8) new_cap = 8;
        emit_ctx *ns = realloc(e->stack, (size_t)new_cap * sizeof(emit_ctx));
        if (!ns) return YAM_ERR_MEMORY;
        e->stack = ns;
        e->stack_cap = new_cap;
    }
    e->stack[e->stack_len++] = (emit_ctx){
        .type = type,
        .indent = indent,
        .count = 0,
        .expect_key = (type == EMIT_CTX_BLOCK_MAP || type == EMIT_CTX_FLOW_MAP),
    };
    return YAM_OK;
}

static emit_ctx *top_ctx(yam_emitter *e) {
    if (e->stack_len == 0) return NULL;
    return &e->stack[e->stack_len - 1];
}

static void pop_ctx(yam_emitter *e) {
    if (e->stack_len > 0) e->stack_len--;
}

static bool in_any_flow(yam_emitter *e) {
    for (int i = e->stack_len - 1; i >= 0; i--) {
        if (e->stack[i].type == EMIT_CTX_FLOW_MAP ||
            e->stack[i].type == EMIT_CTX_FLOW_SEQ)
            return true;
    }
    return false;
}

/* ── Separator strings ───────────────────────────────────── */

static const char *entry_sep(yam_emitter *e) {
    return (e->opts.style == YAM_EMIT_MINIMAL) ? "," : ", ";
}

static int entry_sep_len(yam_emitter *e) {
    return (e->opts.style == YAM_EMIT_MINIMAL) ? 1 : 2;
}

static const char *kv_sep(yam_emitter *e) {
    (void)e;
    return ": "; /* always space after : for safe round-tripping */
}

static int kv_sep_len(yam_emitter *e) {
    (void)e;
    return 2;
}

/* ── Scalar quoting decision ─────────────────────────────── */

static bool is_yaml_keyword(const char *s, size_t len) {
    if (len == 0) return false;
    /* null variants */
    if (len == 4 && memcmp(s, "null", 4) == 0) return true;
    if (len == 4 && memcmp(s, "Null", 4) == 0) return true;
    if (len == 4 && memcmp(s, "NULL", 4) == 0) return true;
    if (len == 1 && s[0] == '~') return true;
    /* bool variants */
    if (len == 4 && memcmp(s, "true", 4) == 0) return true;
    if (len == 4 && memcmp(s, "True", 4) == 0) return true;
    if (len == 4 && memcmp(s, "TRUE", 4) == 0) return true;
    if (len == 5 && memcmp(s, "false", 5) == 0) return true;
    if (len == 5 && memcmp(s, "False", 5) == 0) return true;
    if (len == 5 && memcmp(s, "FALSE", 5) == 0) return true;
    /* special floats */
    if (len == 4 && memcmp(s, ".inf", 4) == 0) return true;
    if (len == 5 && memcmp(s, "+.inf", 5) == 0) return true;
    if (len == 5 && memcmp(s, "-.inf", 5) == 0) return true;
    if (len == 4 && memcmp(s, ".nan", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".Inf", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".NaN", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".Nan", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".INF", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".NAN", 4) == 0) return true;
    return false;
}

/* Forward declarations for int/float matchers from yam_schema.c */
/* We duplicate the logic here to avoid exposing internal functions */
static bool looks_like_number(const char *s, size_t len) {
    if (len == 0) return false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') { i = 1; if (i >= len) return false; }
    /* hex/octal */
    if (i + 1 < len && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'o'))
        return true; /* close enough — if it starts 0x/0o, quote it */
    /* digits */
    bool all_digit = true;
    bool has_dot = false;
    bool has_e = false;
    for (size_t j = i; j < len; j++) {
        if (s[j] >= '0' && s[j] <= '9') continue;
        if (s[j] == '.' && !has_dot) { has_dot = true; continue; }
        if ((s[j] == 'e' || s[j] == 'E') && !has_e) { has_e = true; continue; }
        if ((s[j] == '+' || s[j] == '-') && j > 0 && (s[j-1] == 'e' || s[j-1] == 'E')) continue;
        all_digit = false;
        break;
    }
    return all_digit;
}

static bool has_break(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (yam_is_break((uint8_t)s[i])) return true;
    }
    return false;
}

static bool needs_escape(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x20 && c != '\t') return true; /* control chars */
        if (c == 0x7F) return true;
    }
    return false;
}

static bool needs_quoting(const char *s, size_t len, bool flow_ctx) {
    if (len == 0) return true;

    /* starts with indicator */
    uint8_t first = (uint8_t)s[0];
    if (yam_is_indicator(first)) return true;
    /* space or tab at start */
    if (first == ' ' || first == '\t') return true;

    /* document markers */
    if (len >= 3 && (memcmp(s, "---", 3) == 0 || memcmp(s, "...", 3) == 0))
        return true;

    /* keyword or number */
    if (is_yaml_keyword(s, len)) return true;
    if (looks_like_number(s, len)) return true;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        /* ": " or " #" */
        if (c == ':' && i + 1 < len && s[i + 1] == ' ') return true;
        if (c == ' ' && i + 1 < len && s[i + 1] == '#') return true;
        /* line breaks */
        if (yam_is_break(c)) return true;
        /* flow indicators in flow context */
        if (flow_ctx && yam_is_flow(c)) return true;
        /* trailing space/tab */
        if ((c == ' ' || c == '\t') && i + 1 == len) return true;
    }
    return false;
}

static yam_scalar_style choose_style(yam_scalar_style requested,
                                     const char *val, size_t len,
                                     bool flow_ctx) {
    /* literal/folded only in block context */
    if (requested == YAM_SCALAR_LITERAL || requested == YAM_SCALAR_FOLDED) {
        if (flow_ctx) return YAM_SCALAR_DOUBLE_QUOTED;
        return requested;
    }
    if (requested == YAM_SCALAR_SINGLE_QUOTED) return requested;
    if (requested == YAM_SCALAR_DOUBLE_QUOTED) return requested;

    /* PLAIN requested — auto-detect */
    if (!needs_quoting(val, len, flow_ctx))
        return YAM_SCALAR_PLAIN;

    /* needs quoting — pick style */
    if (needs_escape(val, len))
        return YAM_SCALAR_DOUBLE_QUOTED;

    /* multiline in block context → literal */
    if (!flow_ctx && has_break(val, len))
        return YAM_SCALAR_LITERAL;

    return YAM_SCALAR_DOUBLE_QUOTED;
}

/* ── Scalar emission ─────────────────────────────────────── */

static yam_status emit_plain(yam_emitter *e, const char *s, size_t len) {
    return buf_puts(e, s, len);
}

static yam_status emit_single_quoted(yam_emitter *e, const char *s, size_t len) {
    yam_status st = buf_put(e, '\'');
    if (st != YAM_OK) return st;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            st = PUTS(e, "''");
            if (st != YAM_OK) return st;
        } else {
            st = buf_put(e, s[i]);
            if (st != YAM_OK) return st;
        }
    }
    return buf_put(e, '\'');
}

static yam_status emit_double_quoted(yam_emitter *e, const char *s, size_t len) {
    yam_status st = buf_put(e, '"');
    if (st != YAM_OK) return st;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        switch (c) {
        case '\\': st = PUTS(e, "\\\\"); break;
        case '"':  st = PUTS(e, "\\\""); break;
        case '\n': st = PUTS(e, "\\n"); break;
        case '\r': st = PUTS(e, "\\r"); break;
        case '\t': st = PUTS(e, "\\t"); break;
        case '\0': st = PUTS(e, "\\0"); break;
        case '\a': st = PUTS(e, "\\a"); break;
        case '\b': st = PUTS(e, "\\b"); break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char esc[5];
                snprintf(esc, sizeof(esc), "\\x%02X", c);
                st = buf_puts(e, esc, 4);
            } else {
                st = buf_put(e, (char)c);
            }
            break;
        }
        if (st != YAM_OK) return st;
    }
    return buf_put(e, '"');
}

static yam_status emit_block_scalar(yam_emitter *e, char indicator,
                                    const char *s, size_t len, int indent) {
    yam_status st;

    /* determine chomping */
    char chomp = 0;
    if (len == 0 || s[len - 1] != '\n') {
        chomp = '-'; /* strip */
    } else if (len >= 2 && s[len - 2] == '\n') {
        chomp = '+'; /* keep */
    }
    /* else clip (default) */

    /* check if first content line starts with space (needs indent indicator) */
    bool needs_indent_indicator = (len > 0 && s[0] == ' ');

    st = buf_put(e, indicator);
    if (st != YAM_OK) return st;
    if (chomp) {
        st = buf_put(e, chomp);
        if (st != YAM_OK) return st;
    }
    if (needs_indent_indicator) {
        /* explicit indent = opts.indent */
        char dig = '0' + (char)(e->opts.indent > 9 ? 9 : e->opts.indent);
        st = buf_put(e, dig);
        if (st != YAM_OK) return st;
    }
    st = buf_put(e, '\n');
    if (st != YAM_OK) return st;

    /* emit each line with indentation */
    size_t pos = 0;
    while (pos < len) {
        /* find end of line */
        size_t eol = pos;
        while (eol < len && s[eol] != '\n') eol++;

        if (eol > pos || (eol < len && s[eol] == '\n')) {
            st = buf_indent(e, indent);
            if (st != YAM_OK) return st;
            if (eol > pos) {
                st = buf_puts(e, s + pos, eol - pos);
                if (st != YAM_OK) return st;
            }
            st = buf_put(e, '\n');
            if (st != YAM_OK) return st;
        }

        pos = eol + 1;
        if (eol == len) break; /* no trailing newline */
    }

    return YAM_OK;
}

static yam_status emit_scalar(yam_emitter *e, const yam_event *evt) {
    const char *val = evt->value.data;
    size_t vlen = evt->value.len;
    bool flow_ctx = in_any_flow(e) || e->opts.style != YAM_EMIT_BLOCK;

    yam_scalar_style style = choose_style(evt->scalar_style, val, vlen, flow_ctx);

    emit_ctx *ctx = top_ctx(e);
    int indent = ctx ? ctx->indent + e->opts.indent : e->opts.indent;

    switch (style) {
    case YAM_SCALAR_PLAIN:
        return emit_plain(e, val, vlen);
    case YAM_SCALAR_SINGLE_QUOTED:
        return emit_single_quoted(e, val, vlen);
    case YAM_SCALAR_DOUBLE_QUOTED:
        return emit_double_quoted(e, val, vlen);
    case YAM_SCALAR_LITERAL:
        return emit_block_scalar(e, '|', val, vlen, indent);
    case YAM_SCALAR_FOLDED:
        return emit_block_scalar(e, '>', val, vlen, indent);
    }
    return YAM_ERR_EMIT;
}

/* ── Property emission (anchor, tag) ─────────────────────── */

static yam_status emit_anchor(yam_emitter *e, yam_str anchor) {
    if (!anchor.data || anchor.len == 0) return YAM_OK;
    yam_status st = buf_put(e, '&');
    if (st != YAM_OK) return st;
    st = buf_puts(e, anchor.data, anchor.len);
    if (st != YAM_OK) return st;
    return buf_put(e, ' ');
}

static yam_status emit_tag(yam_emitter *e, yam_str tag) {
    if (!tag.data || tag.len == 0) return YAM_OK;
    static const char prefix[] = "tag:yaml.org,2002:";
    static const size_t plen = sizeof(prefix) - 1;

    yam_status st;
    if (tag.len > plen && memcmp(tag.data, prefix, plen) == 0) {
        st = PUTS(e, "!!");
        if (st != YAM_OK) return st;
        st = buf_puts(e, tag.data + plen, tag.len - plen);
    } else if (tag.data[0] == '!') {
        st = buf_puts(e, tag.data, tag.len);
    } else {
        st = PUTS(e, "!<");
        if (st != YAM_OK) return st;
        st = buf_puts(e, tag.data, tag.len);
        if (st != YAM_OK) return st;
        st = buf_put(e, '>');
    }
    if (st != YAM_OK) return st;
    return buf_put(e, ' ');
}

static yam_status emit_props(yam_emitter *e, const yam_event *evt) {
    yam_status st = emit_anchor(e, evt->anchor);
    if (st != YAM_OK) return st;
    return emit_tag(e, evt->tag);
}

/* ── Pre-node prefix ─────────────────────────────────────── */

static yam_status emit_pre_node(yam_emitter *e, bool is_collection) {
    emit_ctx *ctx = top_ctx(e);
    yam_status st;

    if (!ctx) {
        /* root level — newline between documents */
        if (e->len > 0 && e->buf[e->len - 1] != '\n') {
            st = buf_put(e, '\n');
            if (st != YAM_OK) return st;
        }
        return YAM_OK;
    }

    switch (ctx->type) {
    case EMIT_CTX_BLOCK_MAP:
        if (ctx->expect_key) {
            /* newline + indent before key (skip for first key after "- " or at doc start) */
            if (e->after_seq_dash) {
                /* compact mapping in sequence: first key shares "- " line */
            } else if (ctx->count > 0 || (e->len > 0 && e->buf[e->len - 1] != '\n')) {
                st = buf_put(e, '\n');
                if (st != YAM_OK) return st;
                st = buf_indent(e, ctx->indent);
                if (st != YAM_OK) return st;
            } else if (e->len > 0) {
                /* after a newline, just indent */
                st = buf_indent(e, ctx->indent);
                if (st != YAM_OK) return st;
            }
        } else {
            /* ": " or ":\n" between key and value */
            if (is_collection) {
                st = PUTS(e, ":\n");
                if (st != YAM_OK) return st;
            } else {
                st = PUTS(e, ": ");
                if (st != YAM_OK) return st;
            }
            e->wrote_block_key = false;
        }
        break;

    case EMIT_CTX_BLOCK_SEQ:
        /* newline + indent + "- " */
        if (e->len > 0) {
            st = buf_put(e, '\n');
            if (st != YAM_OK) return st;
        }
        st = buf_indent(e, ctx->indent);
        if (st != YAM_OK) return st;
        st = PUTS(e, "- ");
        if (st != YAM_OK) return st;
        e->after_seq_dash = true;
        break;

    case EMIT_CTX_FLOW_MAP:
        if (ctx->expect_key) {
            if (ctx->count > 0) {
                st = buf_puts(e, entry_sep(e), (size_t)entry_sep_len(e));
                if (st != YAM_OK) return st;
            }
        } else {
            st = buf_puts(e, kv_sep(e), (size_t)kv_sep_len(e));
            if (st != YAM_OK) return st;
        }
        break;

    case EMIT_CTX_FLOW_SEQ:
        if (ctx->count > 0) {
            st = buf_puts(e, entry_sep(e), (size_t)entry_sep_len(e));
            if (st != YAM_OK) return st;
        }
        break;
    }

    return YAM_OK;
}

/* Advance the parent context after emitting a node */
static void advance_ctx(yam_emitter *e) {
    emit_ctx *ctx = top_ctx(e);
    if (!ctx) return;

    if (ctx->type == EMIT_CTX_BLOCK_MAP || ctx->type == EMIT_CTX_FLOW_MAP) {
        if (ctx->expect_key) {
            ctx->expect_key = false;
            e->wrote_block_key = (ctx->type == EMIT_CTX_BLOCK_MAP);
        } else {
            ctx->expect_key = true;
            ctx->count++;
            e->wrote_block_key = false;
        }
    } else {
        /* sequence */
        ctx->count++;
    }
    e->after_seq_dash = false;
}

/* ── Main emit function ──────────────────────────────────── */

yam_status yam_emit(yam_emitter *e, const yam_event *evt) {
    yam_status st;

    switch (evt->type) {
    case YAM_EVT_STREAM_START:
        e->first_doc = true;
        return YAM_OK;

    case YAM_EVT_STREAM_END:
        /* ensure trailing newline */
        if (e->len > 0 && e->buf[e->len - 1] != '\n')
            return buf_put(e, '\n');
        return YAM_OK;

    case YAM_EVT_DOC_START:
        if (!evt->implicit) {
            if (!e->first_doc && e->len > 0 && e->buf[e->len - 1] != '\n') {
                st = buf_put(e, '\n');
                if (st != YAM_OK) return st;
            }
            st = PUTS(e, "---");
            if (st != YAM_OK) return st;
        }
        e->doc_open = true;
        e->first_doc = false;
        return YAM_OK;

    case YAM_EVT_DOC_END:
        if (!evt->implicit) {
            if (e->len > 0 && e->buf[e->len - 1] != '\n') {
                st = buf_put(e, '\n');
                if (st != YAM_OK) return st;
            }
            st = PUTS(e, "...\n");
            if (st != YAM_OK) return st;
        }
        e->doc_open = false;
        return YAM_OK;

    case YAM_EVT_MAPPING_START: {
        bool use_flow = (e->opts.style != YAM_EMIT_BLOCK) || evt->flow;
        bool is_coll = true;

        st = emit_pre_node(e, is_coll);
        if (st != YAM_OK) return st;

        st = emit_props(e, evt);
        if (st != YAM_OK) return st;

        if (use_flow) {
            st = buf_put(e, '{');
            if (st != YAM_OK) return st;
            st = push_ctx(e, EMIT_CTX_FLOW_MAP, 0);
        } else {
            emit_ctx *parent = top_ctx(e);
            int indent;
            if (parent && parent->type == EMIT_CTX_BLOCK_SEQ) {
                /* compact mapping in sequence: indent aligns with content after "- " */
                indent = parent->indent + 2;
            } else if (parent && (parent->type == EMIT_CTX_BLOCK_MAP)) {
                indent = parent->indent + e->opts.indent;
            } else {
                indent = 0;
            }
            st = push_ctx(e, EMIT_CTX_BLOCK_MAP, indent);
        }
        if (st != YAM_OK) return st;
        /* advance parent context, preserving after_seq_dash for compact mapping */
        bool saved_dash = e->after_seq_dash;
        e->stack_len--;
        advance_ctx(e);
        e->stack_len++;
        e->after_seq_dash = saved_dash;
        return YAM_OK;
    }

    case YAM_EVT_MAPPING_END: {
        emit_ctx *ctx = top_ctx(e);
        if (!ctx) return YAM_ERR_EMIT;

        if (ctx->type == EMIT_CTX_FLOW_MAP) {
            st = buf_put(e, '}');
            if (st != YAM_OK) return st;
        } else if (ctx->count == 0) {
            /* empty block mapping → {} */
            st = PUTS(e, "{}");
            if (st != YAM_OK) return st;
        }
        pop_ctx(e);
        return YAM_OK;
    }

    case YAM_EVT_SEQUENCE_START: {
        bool use_flow = (e->opts.style != YAM_EMIT_BLOCK) || evt->flow;
        bool is_coll = true;

        st = emit_pre_node(e, is_coll);
        if (st != YAM_OK) return st;

        st = emit_props(e, evt);
        if (st != YAM_OK) return st;

        if (use_flow) {
            st = buf_put(e, '[');
            if (st != YAM_OK) return st;
            st = push_ctx(e, EMIT_CTX_FLOW_SEQ, 0);
        } else {
            emit_ctx *parent = top_ctx(e);
            int indent;
            if (parent && parent->type == EMIT_CTX_BLOCK_MAP) {
                indent = parent->indent + e->opts.indent;
            } else if (parent && parent->type == EMIT_CTX_BLOCK_SEQ) {
                indent = parent->indent + e->opts.indent;
            } else {
                indent = 0;
            }
            st = push_ctx(e, EMIT_CTX_BLOCK_SEQ, indent);
        }
        if (st != YAM_OK) return st;
        /* advance parent context, preserving after_seq_dash */
        bool saved_dash2 = e->after_seq_dash;
        e->stack_len--;
        advance_ctx(e);
        e->stack_len++;
        e->after_seq_dash = saved_dash2;
        return YAM_OK;
    }

    case YAM_EVT_SEQUENCE_END: {
        emit_ctx *ctx = top_ctx(e);
        if (!ctx) return YAM_ERR_EMIT;

        if (ctx->type == EMIT_CTX_FLOW_SEQ) {
            st = buf_put(e, ']');
            if (st != YAM_OK) return st;
        } else if (ctx->count == 0) {
            /* empty block sequence → [] */
            st = PUTS(e, "[]");
            if (st != YAM_OK) return st;
        }
        pop_ctx(e);
        return YAM_OK;
    }

    case YAM_EVT_SCALAR: {
        st = emit_pre_node(e, false);
        if (st != YAM_OK) return st;

        st = emit_props(e, evt);
        if (st != YAM_OK) return st;

        st = emit_scalar(e, evt);
        if (st != YAM_OK) return st;

        advance_ctx(e);
        return YAM_OK;
    }

    case YAM_EVT_ALIAS: {
        st = emit_pre_node(e, false);
        if (st != YAM_OK) return st;

        st = buf_put(e, '*');
        if (st != YAM_OK) return st;
        st = buf_puts(e, evt->value.data, evt->value.len);
        if (st != YAM_OK) return st;

        advance_ctx(e);
        return YAM_OK;
    }

    default:
        return YAM_ERR_EMIT;
    }
}

/* ── Constructor / destructor ────────────────────────────── */

yam_emitter *yam_emitter_new(yam_emit_opts opts, yam_arena *a) {
    yam_emitter *e = malloc(sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));

    e->opts = opts;
    if (e->opts.indent < 1) e->opts.indent = 2;
    if (e->opts.indent > 10) e->opts.indent = 10;
    e->arena = a;

    e->cap = EMIT_INIT_CAP;
    e->buf = malloc(e->cap);
    if (!e->buf) { free(e); return NULL; }

    e->stack_cap = 8;
    e->stack = malloc((size_t)e->stack_cap * sizeof(emit_ctx));
    if (!e->stack) { free(e->buf); free(e); return NULL; }

    e->first_doc = true;
    return e;
}

yam_str yam_emitter_output(yam_emitter *e) {
    if (!e || !e->buf || e->len == 0) return YAM_STR_NULL;
    char *copy = yam_arena_dup(e->arena, e->buf, e->len);
    if (!copy) return YAM_STR_NULL;
    return (yam_str){copy, e->len};
}

void yam_emitter_free(yam_emitter *e) {
    if (!e) return;
    free(e->buf);
    free(e->stack);
    free(e);
}
