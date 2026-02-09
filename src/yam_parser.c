/*
 * yam_parser.c — YAML 1.2 event parser
 *
 * Thin layer over the scanner: handles block structure via indent tracking,
 * simple key detection via deferred scalar approach, and flow context.
 *
 * Architecture:
 *   The parser consumes tokens from the scanner and produces events.
 *   Block structure (mappings, sequences) is tracked via an indent-based
 *   context stack. Simple keys are detected by peeking ahead for ':'
 *   after a scalar. Flow collections are handled with dedicated states.
 */

#include "yam/yam.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Context types ───────────────────────────────────────── */

typedef enum {
    CTX_BLOCK_MAP,
    CTX_BLOCK_SEQ,
    CTX_FLOW_MAP,
    CTX_FLOW_SEQ,
} ctx_type;

typedef struct {
    ctx_type type;
    int      indent;
} ctx_entry;

/* ── Incremental parser states ──────────────────────────── */

typedef enum {
    ST_STREAM_START, ST_STREAM_DOC_LOOP, ST_STREAM_END,
    ST_DOC_DIRECTIVES, ST_DOC_START_EXPLICIT, ST_DOC_START_IMPLICIT,
    ST_DOC_END_EXPLICIT, ST_DOC_CONTENT,
    ST_BLOCK_NODE, ST_BLOCK_NODE_SCALAR, ST_BLOCK_NODE_SCALAR_KEY,
    ST_BLOCK_NODE_ALIAS, ST_BLOCK_NODE_ALIAS_KEY,
    ST_BLOCK_NODE_SEQ, ST_BLOCK_NODE_EXPLICIT_KEY, ST_BLOCK_NODE_BARE_VALUE,
    ST_BLOCK_NODE_EMPTY,
    ST_BLOCK_MAP_LOOP, ST_BLOCK_MAP_EXPLICIT_KEY, ST_BLOCK_MAP_SIMPLE_KEY,
    ST_BLOCK_MAP_POST_KEY, ST_BLOCK_MAP_VALUE, ST_BLOCK_MAP_VALUE_NODE,
    ST_BLOCK_MAP_END,
    ST_BLOCK_SEQ_LOOP, ST_BLOCK_SEQ_ENTRY, ST_BLOCK_SEQ_ENTRY_NODE,
    ST_BLOCK_SEQ_END,
    ST_UNROLL,
    /* Flow sequence (incremental) */
    ST_FLOW_SEQ_LOOP, ST_FLOW_SEQ_ENTRY, ST_FLOW_SEQ_ENTRY_KEY,
    ST_FLOW_SEQ_IMPLICIT_END,
    ST_FLOW_SEQ_EXPLICIT_KEY, ST_FLOW_SEQ_EXPLICIT_VALUE,
    ST_FLOW_SEQ_EXPLICIT_END,
    ST_FLOW_SEQ_CHECK_COLON, ST_FLOW_SEQ_SEP, ST_FLOW_SEQ_END,
    /* Flow mapping (incremental) */
    ST_FLOW_MAP_LOOP, ST_FLOW_MAP_VALUE, ST_FLOW_MAP_SEP, ST_FLOW_MAP_END,
    /* Flow node dispatch (incremental) */
    ST_FLOW_NODE,
    ST_EAGER_DRAIN,
    ST_DONE,
} parser_state;

typedef struct {
    ctx_type     type;
    int          indent;
    parser_state return_state;
} state_frame;

/* ── Parser state ────────────────────────────────────────── */

struct yam_parser {
    yam_scanner *scanner;
    yam_arena   *arena;
    const char  *input;
    size_t       input_len;

    /* token lookahead */
    yam_token current;
    bool      have_token;

    /* event list (dynamic array, drained via cursor) */
    yam_event *events;
    int        evt_len;
    int        evt_cap;
    int        evt_cursor;

    /* context stack */
    ctx_entry *contexts;
    int        ctx_len;
    int        ctx_cap;

    /* pending anchor/tag for next node */
    yam_str pending_anchor;
    yam_str pending_tag;
    bool    has_anchor;
    bool    has_tag;
    size_t  props_line;  /* line where first prop was consumed */
    int     props_col;   /* column (0-based) where first prop was consumed */
    yam_mark props_start; /* full mark of first property token */

    /* tag directives: handle → prefix mapping */
    struct {
        char handle[16];  /* e.g., "!", "!!", "!e!" */
        char prefix[256]; /* e.g., "tag:example.com,2000:app/" */
    } tag_directives[8];
    int tag_dir_count;

    /* lifecycle */
    bool stream_started;
    bool stream_ended;
    bool doc_open;

    /* schema (optional, for tag resolution) */
    const yam_schema *schema;

    /* merge key resolution (opt-in) */
    bool merge_enabled;

    /* alias resolution (opt-in) */
    bool resolve_enabled;

    /* safety limit */
    int max_events;

    /* error context */
    char     error_msg[256];
    yam_mark error_mark;
    bool     oom;

    /* incremental state machine */
    bool         incremental;   /* true = state machine, false = eager */
    parser_state state;
    state_frame *frames;
    int          frame_len;
    int          frame_cap;
    yam_event    out_buf[8];    /* small output buffer for incremental */
    int          out_len;
    int          out_cursor;
    yam_event    saved_node;    /* saved scalar/alias for : lookahead */
    int          saved_node_col;
    bool         have_saved_node;
    parser_state unroll_return;
    parser_state node_return;  /* where to go after a block node completes */
    int          events_delivered; /* events returned to caller (for eager fallback skip) */
    yam_status   scan_error;      /* last scanner error (for incremental path) */
};

/* ── Error reporting ─────────────────────────────────────── */

#define PARSE_ERROR(p, msg) do { \
    snprintf((p)->error_msg, sizeof((p)->error_msg), "%s", (msg)); \
    (p)->error_mark = (p)->current.start; \
    return YAM_ERR_PARSE; \
} while(0)

/* ── Helpers ─────────────────────────────────────────────── */

static inline yam_event evt_simple(yam_event_type type) {
    yam_event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    return e;
}

static inline bool over_limit(yam_parser *p) {
    return p->max_events > 0 && p->evt_len >= p->max_events;
}

static inline bool enqueue(yam_parser *p, const yam_event *evt) {
    if (over_limit(p)) { p->oom = true; return false; }
    if (p->evt_len >= p->evt_cap) {
        int new_cap = p->evt_cap * 2;
        if (new_cap < 64) new_cap = 64;
        yam_event *new_evts = realloc(p->events, new_cap * sizeof(yam_event));
        if (!new_evts) { p->oom = true; return false; }
        p->events = new_evts;
        p->evt_cap = new_cap;
    }
    yam_event *dst = &p->events[p->evt_len++];
    *dst = *evt;
    /* resolve tag via schema if set and no explicit tag */
    if (p->schema && dst->tag.data == NULL) {
        if (dst->type == YAM_EVT_SCALAR)
            dst->tag = yam_schema_resolve(p->schema, dst);
        else if (dst->type == YAM_EVT_MAPPING_START)
            dst->tag = p->schema->default_map_tag;
        else if (dst->type == YAM_EVT_SEQUENCE_START)
            dst->tag = p->schema->default_seq_tag;
    }
    return true;
}

static inline bool dequeue(yam_parser *p, yam_event *evt) {
    if (p->evt_cursor >= p->evt_len) return false;
    *evt = p->events[p->evt_cursor++];
    return true;
}

static inline bool push_ctx(yam_parser *p, ctx_type type, int indent) {
    if (p->ctx_len >= p->ctx_cap) {
        int new_cap = p->ctx_cap * 2;
        ctx_entry *new_data = realloc(p->contexts, new_cap * sizeof(ctx_entry));
        if (!new_data) { p->oom = true; return false; }
        p->contexts = new_data;
        p->ctx_cap = new_cap;
    }
    p->contexts[p->ctx_len++] = (ctx_entry){type, indent};
    return true;
}

static inline void pop_ctx(yam_parser *p) {
    if (p->ctx_len > 0) p->ctx_len--;
}

static inline ctx_entry *top_ctx(yam_parser *p) {
    return p->ctx_len > 0 ? &p->contexts[p->ctx_len - 1] : NULL;
}

/* ── Token access ────────────────────────────────────────── */

static inline yam_status peek_token(yam_parser *p) {
    if (p->have_token) return YAM_OK;
    yam_status st = yam_scan_next(p->scanner, &p->current);
    if (st != YAM_OK) {
        const char *smsg = yam_scanner_error(p->scanner);
        if (smsg) {
            snprintf(p->error_msg, sizeof(p->error_msg), "%s", smsg);
            p->error_mark = yam_scanner_error_mark(p->scanner);
        }
        p->scan_error = st; /* save for incremental path error detection */
        return st;
    }
    p->have_token = true;
    return YAM_OK;
}

static inline void consume_token(yam_parser *p) {
    p->have_token = false;
}

static inline yam_token_type tok_type(yam_parser *p) {
    return p->current.type;
}

/* Token column (0-based) */
static inline int tok_col(yam_parser *p) {
    return (int)p->current.start.col - 1;
}

/* ── Context helpers ─────────────────────────────────────── */

static inline bool in_flow(yam_parser *p) {
    ctx_entry *top = top_ctx(p);
    return top && (top->type == CTX_FLOW_MAP || top->type == CTX_FLOW_SEQ);
}

/* Close all block contexts */
static void unroll_all(yam_parser *p, yam_mark mark) {
    while (p->ctx_len > 0) {
        ctx_entry *top = top_ctx(p);
        if (top->type == CTX_FLOW_MAP || top->type == CTX_FLOW_SEQ) break;
        yam_event_type et = (top->type == CTX_BLOCK_MAP)
            ? YAM_EVT_MAPPING_END : YAM_EVT_SEQUENCE_END;
        yam_event evt = evt_simple(et);
        evt.start = mark;
        evt.end = mark;
        enqueue(p, &evt);
        pop_ctx(p);
    }
}

/* ── Attach pending anchor/tag ───────────────────────────── */

static inline void attach_props(yam_parser *p, yam_event *evt) {
    bool had_props = p->has_anchor || p->has_tag;
    if (p->has_anchor) {
        evt->anchor = p->pending_anchor;
        p->has_anchor = false;
        p->pending_anchor = YAM_STR_NULL;
    }
    if (p->has_tag) {
        evt->tag = p->pending_tag;
        p->has_tag = false;
        p->pending_tag = YAM_STR_NULL;
    }
    if (had_props) {
        evt->start = p->props_start;
    }
}

/* ── Emit empty scalar ───────────────────────────────────── */

static void emit_empty(yam_parser *p) {
    yam_event evt = evt_simple(YAM_EVT_SCALAR);
    evt.scalar_style = YAM_SCALAR_PLAIN;
    evt.start = p->current.start;
    evt.end = p->current.start;
    attach_props(p, &evt);
    enqueue(p, &evt);
}

/* ── Ensure doc is open ──────────────────────────────────── */

static void ensure_doc(yam_parser *p, bool explicit, yam_mark mark) {
    if (!p->doc_open) {
        yam_event evt = evt_simple(YAM_EVT_DOC_START);
        evt.implicit = !explicit;
        evt.start = mark;
        evt.end = mark;
        enqueue(p, &evt);
        p->doc_open = true;
    }
}

/* ── Consume anchor/tag properties ───────────────────────── */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* copy src to dst, decoding %XX percent-encoded bytes; returns decoded length */
static size_t pct_decode(char *dst, const char *src, size_t len) {
    size_t di = 0;
    for (size_t si = 0; si < len; ) {
        if (src[si] == '%' && si + 2 < len) {
            int hi = hex_val(src[si + 1]);
            int lo = hex_val(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 3;
                continue;
            }
        }
        dst[di++] = src[si++];
    }
    return di;
}

static yam_str expand_tag(yam_parser *p, yam_str raw) {
    if (raw.len < 1 || raw.data[0] != '!') return raw;

    /* verbatim tag: !<...> → strip !< and > */
    if (raw.len >= 3 && raw.data[1] == '<' && raw.data[raw.len - 1] == '>') {
        size_t inner_len = raw.len - 3; /* strip !< and > */
        char *buf = yam_arena_alloc(p->arena, inner_len + 1, 1);
        if (!buf) return raw;
        size_t dlen = pct_decode(buf, raw.data + 2, inner_len);
        buf[dlen] = '\0';
        return (yam_str){buf, dlen};
    }

    /* find the tag handle: !!, !x!, or ! */
    size_t handle_len = 1; /* at least "!" */
    if (raw.len >= 2 && raw.data[1] == '!') {
        /* !! secondary handle */
        handle_len = 2;
    } else if (raw.len >= 3) {
        /* !x...! named handle: find second '!' */
        for (size_t i = 1; i < raw.len; i++) {
            if (raw.data[i] == '!') {
                handle_len = i + 1;
                break;
            }
        }
    }

    /* look up the handle in tag directives */
    for (int i = 0; i < p->tag_dir_count; i++) {
        size_t hlen = strlen(p->tag_directives[i].handle);
        if (hlen == handle_len && memcmp(raw.data, p->tag_directives[i].handle, hlen) == 0) {
            const char *prefix = p->tag_directives[i].prefix;
            size_t plen = strlen(prefix);
            size_t suffix_len = raw.len - handle_len;
            size_t total = plen + suffix_len; /* max size before decoding */
            char *buf = yam_arena_alloc(p->arena, total + 1, 1);
            if (!buf) return raw;
            memcpy(buf, prefix, plen);
            size_t dlen = pct_decode(buf + plen, raw.data + handle_len, suffix_len);
            buf[plen + dlen] = '\0';
            return (yam_str){buf, plen + dlen};
        }
    }

    /* default: !! → tag:yaml.org,2002: */
    if (raw.len >= 2 && raw.data[1] == '!') {
        const char *prefix = "tag:yaml.org,2002:";
        size_t plen = 18;
        size_t suffix_len = raw.len - 2;
        size_t total = plen + suffix_len;
        char *buf = yam_arena_alloc(p->arena, total + 1, 1);
        if (!buf) return raw;
        memcpy(buf, prefix, plen);
        size_t dlen = pct_decode(buf + plen, raw.data + 2, suffix_len);
        buf[plen + dlen] = '\0';
        return (yam_str){buf, plen + dlen};
    }

    return raw;
}

static yam_status consume_props(yam_parser *p) {
    /* Consume at most one anchor and one tag per node */
    for (;;) {
        yam_status st = peek_token(p);
        if (st != YAM_OK) return st;
        if (tok_type(p) == YAM_TOK_ANCHOR && !p->has_anchor) {
            /* If we already have a tag on a previous line and see an anchor
             * on a new line, stop — the tag is for the collection and the
             * anchor starts props for the first element. */
            if (p->has_tag && p->current.start.line != p->props_line) break;
            if (!p->has_tag) {
                p->props_line = p->current.start.line;
                p->props_col = tok_col(p);
                p->props_start = p->current.start;
            }
            p->pending_anchor = p->current.value;
            p->has_anchor = true;
            consume_token(p);
        } else if (tok_type(p) == YAM_TOK_TAG && !p->has_tag) {
            if (!p->has_anchor) {
                p->props_line = p->current.start.line;
                p->props_col = tok_col(p);
                p->props_start = p->current.start;
            }
            p->pending_tag = expand_tag(p, p->current.value);
            p->has_tag = true;
            consume_token(p);
        } else {
            break;
        }
    }
    return YAM_OK;
}

/* ── Forward declarations ────────────────────────────────── */

static yam_status parse_block_node(yam_parser *p);
static yam_status parse_flow_node(yam_parser *p);
static yam_status parse_flow_sequence(yam_parser *p);
static yam_status parse_flow_mapping(yam_parser *p);
static yam_status parse_block_sequence(yam_parser *p, int seq_indent);
static yam_status parse_block_mapping(yam_parser *p, int map_indent);
static yam_status parse_block_map_value(yam_parser *p, int map_indent);

/* ── Parse flow sequence contents ────────────────────────── */

static yam_status parse_flow_sequence(yam_parser *p) {
    yam_status st;

    for (;;) {
        if (p->oom) return YAM_ERR_MEMORY;
        if (over_limit(p)) PARSE_ERROR(p, "event limit exceeded");
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_FLOW_SEQ_END) {
            yam_mark close = p->current.start;
            yam_mark close_end = p->current.end;
            consume_token(p);
            yam_event end_evt = evt_simple(YAM_EVT_SEQUENCE_END);
            end_evt.start = close;
            end_evt.end = close_end;
            enqueue(p, &end_evt);
            pop_ctx(p);
            return YAM_OK;
        }

        /* check for explicit key ? — starts a flow pair */
        if (tok_type(p) == YAM_TOK_BLOCK_MAP_KEY) {
            /* explicit key in flow sequence → flow pair (implicit mapping) */
            yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
            map_evt.flow = true;
            map_evt.start = p->current.start;
            map_evt.end = p->current.end;
            enqueue(p, &map_evt);

            consume_token(p);  /* consume ? */
            st = peek_token(p);
            if (st != YAM_OK) return st;

            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE ||
                tok_type(p) == YAM_TOK_FLOW_ENTRY ||
                tok_type(p) == YAM_TOK_FLOW_SEQ_END) {
                emit_empty(p);  /* empty key */
            } else {
                st = parse_flow_node(p);
                if (st != YAM_OK) return st;
            }

            /* parse value */
            st = peek_token(p);
            if (st != YAM_OK) return st;
            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
                consume_token(p);
                st = peek_token(p);
                if (st != YAM_OK) return st;
                if (tok_type(p) == YAM_TOK_FLOW_ENTRY ||
                    tok_type(p) == YAM_TOK_FLOW_SEQ_END) {
                    emit_empty(p);
                } else {
                    st = parse_flow_node(p);
                    if (st != YAM_OK) return st;
                }
            } else {
                emit_empty(p);
            }

            {
                yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            goto check_flow_sep;
        }

        /* Save position before parsing entry (for implicit key wrapping) */
        int pre_key_len = p->evt_len;

        /* parse entry */
        st = parse_flow_node(p);
        if (st != YAM_OK) return st;

        /* check for implicit flow pair: node followed by : */
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
            /* Insert MAPPING_START before all key events */
            int key_idx = pre_key_len;
            int num_key_events = p->evt_len - key_idx;
            if (num_key_events > 0) {
                /* grow array by 1 */
                yam_event placeholder = evt_simple(YAM_EVT_MAPPING_START);
                placeholder.flow = true;
                placeholder.start = p->events[key_idx].start;
                placeholder.end = p->events[key_idx].start;
                if (!enqueue(p, &placeholder)) return YAM_ERR_MEMORY;
                /* shift key events right to make room for mapping start */
                memmove(&p->events[key_idx + 1],
                        &p->events[key_idx],
                        num_key_events * sizeof(yam_event));
                p->events[key_idx] = placeholder;
            }

            consume_token(p); /* consume : */
            st = peek_token(p);
            if (st != YAM_OK) return st;

            if (tok_type(p) == YAM_TOK_FLOW_ENTRY ||
                tok_type(p) == YAM_TOK_FLOW_SEQ_END) {
                emit_empty(p);
            } else {
                st = parse_flow_node(p);
                if (st != YAM_OK) return st;
            }

            {
                yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
        }

check_flow_sep:
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_FLOW_ENTRY) {
            consume_token(p);
            continue;
        }

        if (tok_type(p) == YAM_TOK_FLOW_SEQ_END) {
            continue; /* will be handled at top of loop */
        }

        PARSE_ERROR(p, "expected ',' or ']' in flow sequence");
    }
}

/* ── Parse flow mapping contents ─────────────────────────── */

static yam_status parse_flow_mapping(yam_parser *p) {
    yam_status st;

    for (;;) {
        if (p->oom) return YAM_ERR_MEMORY;
        if (over_limit(p)) PARSE_ERROR(p, "event limit exceeded");
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_FLOW_MAP_END) {
            yam_mark close = p->current.start;
            yam_mark close_end = p->current.end;
            consume_token(p);
            yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
            end_evt.start = close;
            end_evt.end = close_end;
            enqueue(p, &end_evt);
            pop_ctx(p);
            return YAM_OK;
        }

        /* parse key */
        if (tok_type(p) == YAM_TOK_BLOCK_MAP_KEY) {
            consume_token(p);
            st = peek_token(p);
            if (st != YAM_OK) return st;
            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE ||
                tok_type(p) == YAM_TOK_FLOW_ENTRY ||
                tok_type(p) == YAM_TOK_FLOW_MAP_END) {
                emit_empty(p);
            } else {
                st = parse_flow_node(p);
                if (st != YAM_OK) return st;
            }
        } else {
            st = parse_flow_node(p);
            if (st != YAM_OK) return st;
        }

        /* parse value */
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
            consume_token(p);
            st = peek_token(p);
            if (st != YAM_OK) return st;
            if (tok_type(p) == YAM_TOK_FLOW_ENTRY ||
                tok_type(p) == YAM_TOK_FLOW_MAP_END) {
                emit_empty(p);
            } else {
                st = parse_flow_node(p);
                if (st != YAM_OK) return st;
            }
        } else {
            emit_empty(p);
        }

        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_FLOW_ENTRY) {
            consume_token(p);
            continue;
        }

        if (tok_type(p) == YAM_TOK_FLOW_MAP_END) {
            continue;
        }

        PARSE_ERROR(p, "expected ',' or '}' in flow mapping");
    }
}

/* ── Parse a node in flow context ────────────────────────── */

static yam_status parse_flow_node(yam_parser *p) {
    if (p->oom) return YAM_ERR_MEMORY;
    yam_status st = consume_props(p);
    if (st != YAM_OK) return st;

    st = peek_token(p);
    if (st != YAM_OK) return st;

    yam_token_type tt = tok_type(p);

    if (tt == YAM_TOK_ALIAS) {
        yam_event evt = evt_simple(YAM_EVT_ALIAS);
        evt.value = p->current.value;
        evt.start = p->current.start;
        evt.end = p->current.end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        consume_token(p);
        return YAM_OK;
    }

    if (tt == YAM_TOK_FLOW_SEQ_START) {
        int col = tok_col(p);
        yam_mark open_start = p->current.start;
        yam_mark open_end = p->current.end;
        consume_token(p);
        yam_event evt = evt_simple(YAM_EVT_SEQUENCE_START);
        evt.flow = true;
        evt.start = open_start;
        evt.end = open_end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_FLOW_SEQ, col);
        return parse_flow_sequence(p);
    }

    if (tt == YAM_TOK_FLOW_MAP_START) {
        int col = tok_col(p);
        yam_mark open_start = p->current.start;
        yam_mark open_end = p->current.end;
        consume_token(p);
        yam_event evt = evt_simple(YAM_EVT_MAPPING_START);
        evt.flow = true;
        evt.start = open_start;
        evt.end = open_end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_FLOW_MAP, col);
        return parse_flow_mapping(p);
    }

    if (tt == YAM_TOK_SCALAR) {
        yam_event evt = evt_simple(YAM_EVT_SCALAR);
        evt.value = p->current.value;
        evt.scalar_style = p->current.scalar_style;
        evt.start = p->current.start;
        evt.end = p->current.end;
        attach_props(p, &evt);

        consume_token(p);

        /* check for implicit key in flow context */
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
            /* this scalar is a key in an implicit flow mapping */
            /* For now, just emit the scalar; the caller handles key-value */
        }

        enqueue(p, &evt);
        return YAM_OK;
    }

    /* empty scalar */
    emit_empty(p);
    return YAM_OK;
}

/* ── Parse block map value ───────────────────────────────── */

static yam_status parse_block_map_value(yam_parser *p, int map_indent) {
    if (p->oom) return YAM_ERR_MEMORY;
    yam_status st = peek_token(p);
    if (st != YAM_OK) return st;

    if (tok_type(p) != YAM_TOK_BLOCK_MAP_VALUE) {
        emit_empty(p);
        return YAM_OK;
    }

    consume_token(p); /* consume : */

    st = peek_token(p);
    if (st != YAM_OK) return st;

    yam_token_type tt = tok_type(p);
    int col = tok_col(p);

    /* empty value cases */
    if (tt == YAM_TOK_STREAM_END || tt == YAM_TOK_DOC_START ||
        tt == YAM_TOK_DOC_END) {
        emit_empty(p);
        return YAM_OK;
    }

    /* next key at same indent → empty value */
    if (col == map_indent && (tt == YAM_TOK_SCALAR || tt == YAM_TOK_ANCHOR ||
        tt == YAM_TOK_TAG || tt == YAM_TOK_BLOCK_MAP_KEY ||
        tt == YAM_TOK_BLOCK_MAP_VALUE)) {
        emit_empty(p);
        return YAM_OK;
    }

    /* seq entry at lesser indent than map → empty value.
     * At same indent → valid value (YAML allows sequences at key indent). */
    if (tt == YAM_TOK_BLOCK_SEQ_ENTRY && col < map_indent) {
        emit_empty(p);
        return YAM_OK;
    }

    /* parse value as block node */
    return parse_block_node(p);
}

/* ── Parse block mapping ─────────────────────────────────── */

static yam_status parse_block_mapping(yam_parser *p, int map_indent) {
    yam_status st;

    for (;;) {
        if (p->oom) return YAM_ERR_MEMORY;
        if (over_limit(p)) PARSE_ERROR(p, "event limit exceeded");
        st = peek_token(p);
        if (st != YAM_OK) return st;

        int col = tok_col(p);
        yam_token_type tt = tok_type(p);

        /* explicit key ? */
        if (tt == YAM_TOK_BLOCK_MAP_KEY && col == map_indent) {
            consume_token(p);

            st = peek_token(p);
            if (st != YAM_OK) return st;

            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE &&
                tok_col(p) == map_indent) {
                emit_empty(p);
            } else if (tok_type(p) == YAM_TOK_ANCHOR ||
                       tok_type(p) == YAM_TOK_TAG) {
                /* Consume props, then check if ':' follows at map indent.
                 * If so, the props belong to an empty key scalar, not a nested node. */
                st = consume_props(p);
                if (st != YAM_OK) return st;
                st = peek_token(p);
                if (st != YAM_OK) return st;
                if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE &&
                    tok_col(p) == map_indent) {
                    emit_empty(p);
                } else {
                    st = parse_block_node(p);
                    if (st != YAM_OK) return st;
                }
            } else {
                st = parse_block_node(p);
                if (st != YAM_OK) return st;
            }

            /* parse value */
            st = parse_block_map_value(p, map_indent);
            if (st != YAM_OK) return st;
            continue;
        }

        /* empty key + value (: at map indent) */
        if (tt == YAM_TOK_BLOCK_MAP_VALUE && col == map_indent) {
            emit_empty(p); /* empty key */
            st = parse_block_map_value(p, map_indent);
            if (st != YAM_OK) return st;
            continue;
        }

        /* simple key (scalar at map indent) */
        if (col != map_indent) break;

        /* tokens that can't be keys */
        if (tt == YAM_TOK_STREAM_END || tt == YAM_TOK_DOC_START ||
            tt == YAM_TOK_DOC_END || tt == YAM_TOK_BLOCK_SEQ_ENTRY) {
            break;
        }

        /* consume properties that might be on the key */
        int key_start_col = col; /* remember where the key (or its props) started */
        st = consume_props(p);
        if (st != YAM_OK) return st;

        st = peek_token(p);
        if (st != YAM_OK) return st;
        tt = tok_type(p);
        col = tok_col(p);

        /* key matches map indent if either the props or scalar is at map_indent */
        bool at_map_indent = (col == map_indent) || (key_start_col == map_indent);
        if (tt == YAM_TOK_SCALAR && at_map_indent) {
            yam_event evt = evt_simple(YAM_EVT_SCALAR);
            evt.value = p->current.value;
            evt.scalar_style = p->current.scalar_style;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);

            /* peek for : */
            st = peek_token(p);
            if (st != YAM_OK) return st;

            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
                /* confirmed key */
                enqueue(p, &evt);
                st = parse_block_map_value(p, map_indent);
                if (st != YAM_OK) return st;
                continue;
            }

            /* not a key, just a value — this shouldn't really happen
               in a well-formed mapping, but emit it and break */
            enqueue(p, &evt);
            break;
        }

        /* alias as key */
        if (tt == YAM_TOK_ALIAS && col == map_indent) {
            yam_event evt = evt_simple(YAM_EVT_ALIAS);
            evt.value = p->current.value;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);

            st = peek_token(p);
            if (st != YAM_OK) return st;

            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
                enqueue(p, &evt);
                st = parse_block_map_value(p, map_indent);
                if (st != YAM_OK) return st;
                continue;
            }
            enqueue(p, &evt);
            break;
        }

        /* flow collection as key */
        if ((tt == YAM_TOK_FLOW_SEQ_START || tt == YAM_TOK_FLOW_MAP_START) &&
            col == map_indent) {
            st = parse_block_node(p);
            if (st != YAM_OK) return st;

            st = peek_token(p);
            if (st != YAM_OK) return st;

            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
                st = parse_block_map_value(p, map_indent);
                if (st != YAM_OK) return st;
                continue;
            }
            break;
        }

        break;
    }

    return YAM_OK;
}

/* ── Parse block sequence ────────────────────────────────── */

static yam_status parse_block_sequence(yam_parser *p, int seq_indent) {
    yam_status st;

    for (;;) {
        if (p->oom) return YAM_ERR_MEMORY;
        if (over_limit(p)) PARSE_ERROR(p, "event limit exceeded");
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) != YAM_TOK_BLOCK_SEQ_ENTRY || tok_col(p) != seq_indent)
            break;

        consume_token(p); /* consume - */

        st = peek_token(p);
        if (st != YAM_OK) return st;

        yam_token_type tt = tok_type(p);

        /* empty entry */
        if (tt == YAM_TOK_BLOCK_SEQ_ENTRY && tok_col(p) == seq_indent) {
            emit_empty(p);
            continue;
        }
        if (tt == YAM_TOK_STREAM_END || tt == YAM_TOK_DOC_START ||
            tt == YAM_TOK_DOC_END) {
            emit_empty(p);
            break;
        }

        /* parse entry content */
        st = parse_block_node(p);
        if (st != YAM_OK) return st;
    }

    return YAM_OK;
}

/* ── Parse block node (may be scalar, collection, alias, flow) ── */

static yam_status parse_block_node(yam_parser *p) {
    if (p->oom) return YAM_ERR_MEMORY;
    yam_status st = consume_props(p);
    if (st != YAM_OK) return st;

    st = peek_token(p);
    if (st != YAM_OK) return st;

    yam_token_type tt = tok_type(p);
    int col = tok_col(p);

    /* If next token is ANCHOR/TAG, these are props for an inner node.
     * Save our props (for the outer collection) and let the inner
     * node's props be consumed when we recurse into parsing. */
    if ((tt == YAM_TOK_ANCHOR || tt == YAM_TOK_TAG) &&
        (p->has_anchor || p->has_tag)) {
        /* Save outer node's props */
        yam_str saved_anchor = p->pending_anchor;
        yam_str saved_tag = p->pending_tag;
        bool had_anchor = p->has_anchor;
        bool had_tag = p->has_tag;
        yam_mark saved_props_start = p->props_start;
        p->has_anchor = false;
        p->has_tag = false;
        p->pending_anchor = YAM_STR_NULL;
        p->pending_tag = YAM_STR_NULL;

        /* Consume inner node's props to peek at the real structure */
        st = consume_props(p);
        if (st != YAM_OK) return st;

        /* Save inner props */
        yam_str inner_anchor = p->pending_anchor;
        yam_str inner_tag = p->pending_tag;
        bool inner_has_anchor = p->has_anchor;
        bool inner_has_tag = p->has_tag;

        /* Restore outer props for attach_props calls below */
        p->pending_anchor = saved_anchor;
        p->pending_tag = saved_tag;
        p->has_anchor = had_anchor;
        p->has_tag = had_tag;
        p->props_start = saved_props_start;

        st = peek_token(p);
        if (st != YAM_OK) return st;
        tt = tok_type(p);
        col = tok_col(p);

        /* Now handle the node type with inner props deferred */
        if (tt == YAM_TOK_SCALAR) {
            yam_event evt = evt_simple(YAM_EVT_SCALAR);
            evt.value = p->current.value;
            evt.scalar_style = p->current.scalar_style;
            evt.start = p->current.start;
            evt.end = p->current.end;
            /* Attach inner props to the scalar */
            if (inner_has_anchor) evt.anchor = inner_anchor;
            if (inner_has_tag) evt.tag = inner_tag;

            int scalar_col = col;
            consume_token(p);

            st = peek_token(p);
            if (st != YAM_OK) return st;

            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE && !in_flow(p)) {
                /* mapping — outer props go on +MAP */
                yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
                map_evt.start = evt.start;
                map_evt.end = evt.start;
                attach_props(p, &map_evt); /* outer anchor/tag */
                enqueue(p, &map_evt);
                push_ctx(p, CTX_BLOCK_MAP, scalar_col);
                enqueue(p, &evt); /* key scalar with inner anchor/tag */

                st = parse_block_map_value(p, scalar_col);
                if (st != YAM_OK) return st;
                st = parse_block_mapping(p, scalar_col);
                if (st != YAM_OK) return st;
                {
                    yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
                    end_evt.start = p->current.start;
                    end_evt.end = p->current.start;
                    enqueue(p, &end_evt);
                }
                pop_ctx(p);
                return YAM_OK;
            }

            /* not a mapping — outer props go on scalar, inner should too
             * (but this is unusual; fall through with scalar) */
            attach_props(p, &evt);
            enqueue(p, &evt);
            return YAM_OK;
        }

        if (tt == YAM_TOK_BLOCK_SEQ_ENTRY) {
            yam_event evt = evt_simple(YAM_EVT_SEQUENCE_START);
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            enqueue(p, &evt);
            push_ctx(p, CTX_BLOCK_SEQ, col);
            /* Restore inner props for the first entry's node */
            p->pending_anchor = inner_anchor;
            p->pending_tag = inner_tag;
            p->has_anchor = inner_has_anchor;
            p->has_tag = inner_has_tag;
            st = parse_block_sequence(p, col);
            if (st != YAM_OK) return st;
            {
                yam_event end_evt = evt_simple(YAM_EVT_SEQUENCE_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
            return YAM_OK;
        }

        /* Flow collection with double props — outer props go on implicit
         * block mapping, inner props go on the flow collection, which
         * becomes a complex key if followed by ':'. */
        if (tt == YAM_TOK_FLOW_SEQ_START || tt == YAM_TOK_FLOW_MAP_START) {
            int flow_col = col;
            yam_mark flow_open = p->current.start;
            yam_mark flow_open_end = p->current.end;
            /* Emit mapping start with outer props first */
            yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
            map_evt.anchor = saved_anchor;
            map_evt.tag = saved_tag;
            map_evt.start = saved_props_start;
            map_evt.end = saved_props_start;
            enqueue(p, &map_evt);
            push_ctx(p, CTX_BLOCK_MAP, flow_col);

            /* Parse the flow collection directly (not via parse_block_node,
             * to avoid double complex-key detection).
             * Clear pending props first so flow content can use consume_props. */
            p->has_anchor = false;
            p->has_tag = false;
            p->pending_anchor = YAM_STR_NULL;
            p->pending_tag = YAM_STR_NULL;

            consume_token(p);
            if (tt == YAM_TOK_FLOW_SEQ_START) {
                yam_event sevt = evt_simple(YAM_EVT_SEQUENCE_START);
                sevt.flow = true;
                sevt.start = flow_open;
                sevt.end = flow_open_end;
                if (inner_has_anchor) sevt.anchor = inner_anchor;
                if (inner_has_tag) sevt.tag = inner_tag;
                enqueue(p, &sevt);
                push_ctx(p, CTX_FLOW_SEQ, col);
                st = parse_flow_sequence(p);
            } else {
                yam_event mevt = evt_simple(YAM_EVT_MAPPING_START);
                mevt.flow = true;
                mevt.start = flow_open;
                mevt.end = flow_open_end;
                if (inner_has_anchor) mevt.anchor = inner_anchor;
                if (inner_has_tag) mevt.tag = inner_tag;
                enqueue(p, &mevt);
                push_ctx(p, CTX_FLOW_MAP, col);
                st = parse_flow_mapping(p);
            }
            if (st != YAM_OK) return st;

            st = peek_token(p);
            if (st != YAM_OK) return st;

            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
                st = parse_block_map_value(p, flow_col);
                if (st != YAM_OK) return st;
                st = parse_block_mapping(p, flow_col);
                if (st != YAM_OK) return st;
            }
            {
                yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
            return YAM_OK;
        }

        /* Restore inner props for other cases */
        p->pending_anchor = inner_anchor;
        p->pending_tag = inner_tag;
        p->has_anchor = inner_has_anchor;
        p->has_tag = inner_has_tag;
        /* fall through to normal handling (outer props already in pending) */
        /* Actually attach outer props first, then inner will be re-consumed */
        /* This is a complex edge case; emit empty with outer props */
        yam_event empty = evt_simple(YAM_EVT_SCALAR);
        empty.value = YAM_STR_NULL;
        empty.start = p->current.start;
        empty.end = p->current.start;
        attach_props(p, &empty);
        enqueue(p, &empty);
        return YAM_OK;
    }

    /* alias */
    if (tt == YAM_TOK_ALIAS) {
        yam_event evt = evt_simple(YAM_EVT_ALIAS);
        evt.value = p->current.value;
        evt.start = p->current.start;
        evt.end = p->current.end;
        int alias_col = col;
        consume_token(p);

        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE && !in_flow(p)) {
            /* alias is a key in a new block mapping — props go on mapping */
            yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
            map_evt.start = evt.start;
            map_evt.end = evt.start;
            attach_props(p, &map_evt);
            enqueue(p, &map_evt);
            push_ctx(p, CTX_BLOCK_MAP, alias_col);
            enqueue(p, &evt); /* alias as key (no props) */
            st = parse_block_map_value(p, alias_col);
            if (st != YAM_OK) return st;
            st = parse_block_mapping(p, alias_col);
            if (st != YAM_OK) return st;
            {
                yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
            return YAM_OK;
        }

        attach_props(p, &evt);
        enqueue(p, &evt);
        return YAM_OK;
    }

    /* flow sequence */
    if (tt == YAM_TOK_FLOW_SEQ_START) {
        int flow_col = col;
        /* Save queue position in case this is a complex key */
        int saved_evt_len = p->evt_len;

        yam_mark open_start = p->current.start;
        yam_mark open_end = p->current.end;
        consume_token(p);
        yam_event evt = evt_simple(YAM_EVT_SEQUENCE_START);
        evt.flow = true;
        evt.start = open_start;
        evt.end = open_end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_FLOW_SEQ, col);
        st = parse_flow_sequence(p);
        if (st != YAM_OK) return st;

        /* Check if this flow seq was a complex key (followed by ':').
         * Don't wrap if the ':' belongs to an existing parent block mapping. */
        st = peek_token(p);
        if (st != YAM_OK) return st;
        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE && !in_flow(p)) {
            ctx_entry *fctx = top_ctx(p);
            bool colon_in_parent = fctx && fctx->type == CTX_BLOCK_MAP &&
                                   tok_col(p) == fctx->indent;
            if (colon_in_parent) goto flow_seq_done;
            /* Insert mapping start before the flow events */
            yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
            map_evt.start = p->events[saved_evt_len].start;
            map_evt.end = p->events[saved_evt_len].start;
            if (!enqueue(p, &map_evt)) return YAM_ERR_MEMORY;
            memmove(&p->events[saved_evt_len + 1],
                    &p->events[saved_evt_len],
                    (p->evt_len - saved_evt_len - 1) * sizeof(yam_event));
            p->events[saved_evt_len] = map_evt;
            push_ctx(p, CTX_BLOCK_MAP, flow_col);
            st = parse_block_map_value(p, flow_col);
            if (st != YAM_OK) return st;
            st = parse_block_mapping(p, flow_col);
            if (st != YAM_OK) return st;
            {
                yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
        }
        flow_seq_done:
        return YAM_OK;
    }

    /* flow mapping */
    if (tt == YAM_TOK_FLOW_MAP_START) {
        int flow_col = col;
        int saved_evt_len = p->evt_len;

        yam_mark open_start2 = p->current.start;
        yam_mark open_end2 = p->current.end;
        consume_token(p);
        yam_event evt = evt_simple(YAM_EVT_MAPPING_START);
        evt.flow = true;
        evt.start = open_start2;
        evt.end = open_end2;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_FLOW_MAP, col);
        st = parse_flow_mapping(p);
        if (st != YAM_OK) return st;

        /* Check if this flow map was a complex key (followed by ':') */
        st = peek_token(p);
        if (st != YAM_OK) return st;
        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE && !in_flow(p)) {
            ctx_entry *fmctx = top_ctx(p);
            bool colon_in_parent2 = fmctx && fmctx->type == CTX_BLOCK_MAP &&
                                    tok_col(p) == fmctx->indent;
            if (colon_in_parent2) goto flow_map_done;
            yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
            map_evt.start = p->events[saved_evt_len].start;
            map_evt.end = p->events[saved_evt_len].start;
            if (!enqueue(p, &map_evt)) return YAM_ERR_MEMORY;
            memmove(&p->events[saved_evt_len + 1],
                    &p->events[saved_evt_len],
                    (p->evt_len - saved_evt_len - 1) * sizeof(yam_event));
            p->events[saved_evt_len] = map_evt;
            push_ctx(p, CTX_BLOCK_MAP, flow_col);
            st = parse_block_map_value(p, flow_col);
            if (st != YAM_OK) return st;
            st = parse_block_mapping(p, flow_col);
            if (st != YAM_OK) return st;
            {
                yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
        }
        flow_map_done:
        return YAM_OK;
    }

    /* If we have pending props (anchor/tag) and the next token indicates
     * there's no content for them (doc/stream end, or sibling in parent
     * collection), emit them on an empty scalar. */
    if ((p->has_anchor || p->has_tag) &&
        (tt == YAM_TOK_DOC_START || tt == YAM_TOK_DOC_END ||
         tt == YAM_TOK_STREAM_END)) {
        yam_event empty_evt = evt_simple(YAM_EVT_SCALAR);
        empty_evt.value = YAM_STR_NULL;
        empty_evt.start = p->current.start;
        empty_evt.end = p->current.start;
        attach_props(p, &empty_evt);
        enqueue(p, &empty_evt);
        return YAM_OK;
    }
    if ((p->has_anchor || p->has_tag) && tt == YAM_TOK_BLOCK_SEQ_ENTRY) {
        ctx_entry *ctx = top_ctx(p);
        /* If '-' is below (strictly less than) the current context's indent,
         * the tag/anchor belongs to an empty scalar (the '-' is a parent).
         * At equal indent in a sequence, it's a sibling → empty scalar too. */
        bool is_empty = false;
        if (ctx && col < ctx->indent)
            is_empty = true;
        else if (ctx && ctx->type == CTX_BLOCK_SEQ && col == ctx->indent)
            is_empty = true;
        if (is_empty) {
            yam_event empty_evt = evt_simple(YAM_EVT_SCALAR);
            empty_evt.value = YAM_STR_NULL;
            empty_evt.start = p->current.start;
            empty_evt.end = p->current.start;
            attach_props(p, &empty_evt);
            enqueue(p, &empty_evt);
            return YAM_OK;
        }
    }

    /* block sequence */
    if (tt == YAM_TOK_BLOCK_SEQ_ENTRY) {
        yam_event evt = evt_simple(YAM_EVT_SEQUENCE_START);
        evt.start = p->current.start;
        evt.end = p->current.end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_BLOCK_SEQ, col);
        st = parse_block_sequence(p, col);
        if (st != YAM_OK) return st;
        {
            yam_event end_evt = evt_simple(YAM_EVT_SEQUENCE_END);
            end_evt.start = p->current.start;
            end_evt.end = p->current.start;
            enqueue(p, &end_evt);
        }
        pop_ctx(p);
        return YAM_OK;
    }

    /* scalar — check if it's at the same indent as parent mapping (sibling key) */
    if (tt == YAM_TOK_SCALAR) {
        ctx_entry *ctx = top_ctx(p);
        if (ctx && ctx->type == CTX_BLOCK_MAP && col <= ctx->indent) {
            /* This scalar is at the same or lower indent as the current mapping.
             * It belongs to the parent — emit empty value with any pending props. */
            yam_event empty_evt = evt_simple(YAM_EVT_SCALAR);
            empty_evt.value = YAM_STR_NULL;
            empty_evt.start = p->current.start;
            empty_evt.end = p->current.start;
            attach_props(p, &empty_evt);
            enqueue(p, &empty_evt);
            return YAM_OK;
        }

        yam_event evt = evt_simple(YAM_EVT_SCALAR);
        evt.value = p->current.value;
        evt.scalar_style = p->current.scalar_style;
        evt.start = p->current.start;
        evt.end = p->current.end;
        size_t scalar_line = p->current.start.line;

        int scalar_col = col;
        consume_token(p);

        /* peek for : → simple key (starts a nested mapping) */
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE && !in_flow(p)) {
            int colon_col = tok_col(p);
            /* The effective key column considers props on the same line:
             * e.g., "!!str a:" has effective col = 0 (the tag col) */
            int eff_key_col = scalar_col;
            if ((p->has_anchor || p->has_tag) &&
                p->props_line == scalar_line &&
                p->props_col < scalar_col) {
                eff_key_col = p->props_col;
            }
            /* Check: the key must be at a deeper indent than the enclosing
             * block, AND the : must be deeper too. */
            ctx_entry *ctx = top_ctx(p);
            bool nested = !ctx ||
                          (eff_key_col > ctx->indent && colon_col > ctx->indent);

            if (nested) {
                /* this scalar is a key for a NEW mapping.
                 * If pending props are from a different line than the scalar,
                 * they belong on the mapping (e.g., !!map\n  key: val).
                 * If from the same line, they belong on the key scalar
                 * (e.g., &anchor key: val or !!str key: val). */
                yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
                bool props_on_map = (p->has_anchor || p->has_tag) &&
                                    p->props_line != scalar_line;

                /* mapping indent uses effective key col (considers props) */
                int map_col = eff_key_col;

                if (props_on_map) {
                    map_evt.start = evt.start;
                    map_evt.end = evt.start;
                    attach_props(p, &map_evt);
                    enqueue(p, &map_evt);
                    push_ctx(p, CTX_BLOCK_MAP, map_col);
                    enqueue(p, &evt); /* key scalar without props */
                } else {
                    map_evt.start = evt.start;
                    map_evt.end = evt.start;
                    enqueue(p, &map_evt);
                    push_ctx(p, CTX_BLOCK_MAP, map_col);
                    attach_props(p, &evt); /* props go on the key scalar */
                    enqueue(p, &evt);
                }

                st = parse_block_map_value(p, map_col);
                if (st != YAM_OK) return st;

                /* continue mapping for more keys at same indent */
                st = parse_block_mapping(p, map_col);
                if (st != YAM_OK) return st;

                {
                    yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
                    end_evt.start = p->current.start;
                    end_evt.end = p->current.start;
                    enqueue(p, &end_evt);
                }
                pop_ctx(p);
                return YAM_OK;
            }
        }

        /* just a scalar value — attach props now */
        attach_props(p, &evt);
        enqueue(p, &evt);
        return YAM_OK;
    }

    /* explicit key ? */
    if (tt == YAM_TOK_BLOCK_MAP_KEY) {
        yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
        map_evt.start = p->current.start;
        map_evt.end = p->current.end;
        attach_props(p, &map_evt);
        enqueue(p, &map_evt);
        push_ctx(p, CTX_BLOCK_MAP, col);

        st = parse_block_mapping(p, col);
        if (st != YAM_OK) return st;

        {
            yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
            end_evt.start = p->current.start;
            end_evt.end = p->current.start;
            enqueue(p, &end_evt);
        }
        pop_ctx(p);
        return YAM_OK;
    }

    /* bare : (empty key mapping) — tag/anchor belongs on the empty key */
    if (tt == YAM_TOK_BLOCK_MAP_VALUE && !in_flow(p)) {
        yam_event map_evt = evt_simple(YAM_EVT_MAPPING_START);
        map_evt.start = p->current.start;
        map_evt.end = p->current.end;
        /* if we have props, they belong on the empty key, not the mapping.
         * Use props column as mapping indent since props precede the colon. */
        if (p->has_anchor || p->has_tag) {
            int map_col = p->props_col;
            map_evt.start = p->props_start;
            map_evt.end = p->props_start;
            enqueue(p, &map_evt);
            push_ctx(p, CTX_BLOCK_MAP, map_col);
            /* emit empty key with the pending props */
            yam_event key_evt = evt_simple(YAM_EVT_SCALAR);
            key_evt.value = YAM_STR_NULL;
            key_evt.start = p->current.start;
            key_evt.end = p->current.start;
            attach_props(p, &key_evt);
            enqueue(p, &key_evt);
            st = parse_block_map_value(p, map_col);
            if (st != YAM_OK) return st;
            st = parse_block_mapping(p, map_col);
        } else {
            enqueue(p, &map_evt);
            push_ctx(p, CTX_BLOCK_MAP, col);
            st = parse_block_mapping(p, col);
        }
        if (st != YAM_OK) return st;

        {
            yam_event end_evt = evt_simple(YAM_EVT_MAPPING_END);
            end_evt.start = p->current.start;
            end_evt.end = p->current.start;
            enqueue(p, &end_evt);
        }
        pop_ctx(p);
        return YAM_OK;
    }

    /* empty node / doc boundary */
    if (tt == YAM_TOK_STREAM_END || tt == YAM_TOK_DOC_START ||
        tt == YAM_TOK_DOC_END || tt == YAM_TOK_NONE) {
        emit_empty(p);
        return YAM_OK;
    }

    /* fallback: empty scalar */
    emit_empty(p);
    return YAM_OK;
}

/* ── Parse document ──────────────────────────────────────── */

static yam_status parse_document(yam_parser *p) {
    if (p->oom) return YAM_ERR_MEMORY;
    yam_status st = peek_token(p);
    if (st != YAM_OK) return st;

    /* parse directives (%YAML, %TAG) — only plain scalars at col 1 */
    while (tok_type(p) == YAM_TOK_SCALAR &&
           p->current.scalar_style == YAM_SCALAR_PLAIN &&
           p->current.start.col == 1 &&
           p->current.value.len > 0 && p->current.value.data[0] == '%') {
        yam_str val = p->current.value;
        /* parse %TAG handle prefix */
        if (val.len > 5 && memcmp(val.data, "%TAG ", 5) == 0) {
            const char *s = val.data + 5;
            const char *end = val.data + val.len;
            /* skip spaces */
            while (s < end && *s == ' ') s++;
            /* read handle (e.g., "!", "!!", "!e!") */
            const char *h_start = s;
            while (s < end && *s != ' ') s++;
            size_t hlen = s - h_start;
            /* skip spaces */
            while (s < end && *s == ' ') s++;
            /* read prefix */
            const char *p_start = s;
            while (s < end && *s != ' ' && *s != '\n') s++;
            size_t plen = s - p_start;

            if (hlen > 0 && hlen < 16 && plen > 0 && plen < 256 &&
                p->tag_dir_count < 8) {
                int idx = -1;
                /* check if handle already exists, replace if so */
                for (int i = 0; i < p->tag_dir_count; i++) {
                    if (strlen(p->tag_directives[i].handle) == hlen &&
                        memcmp(p->tag_directives[i].handle, h_start, hlen) == 0) {
                        idx = i;
                        break;
                    }
                }
                if (idx < 0) idx = p->tag_dir_count++;
                memcpy(p->tag_directives[idx].handle, h_start, hlen);
                p->tag_directives[idx].handle[hlen] = '\0';
                memcpy(p->tag_directives[idx].prefix, p_start, plen);
                p->tag_directives[idx].prefix[plen] = '\0';
            }
        }
        consume_token(p);
        st = peek_token(p);
        if (st != YAM_OK) return st;
    }

    if (tok_type(p) == YAM_TOK_DOC_START) {
        /* explicit doc start */
        if (p->doc_open) {
            unroll_all(p, p->current.start);
            yam_event evt = evt_simple(YAM_EVT_DOC_END);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            enqueue(p, &evt);
            p->doc_open = false;
        }
        yam_mark doc_start = p->current.start;
        yam_mark doc_end = p->current.end;
        consume_token(p);
        yam_event evt = evt_simple(YAM_EVT_DOC_START);
        evt.implicit = false;
        evt.start = doc_start;
        evt.end = doc_end;
        enqueue(p, &evt);
        p->doc_open = true;

        /* peek for content */
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_STREAM_END ||
            tok_type(p) == YAM_TOK_DOC_END ||
            tok_type(p) == YAM_TOK_DOC_START) {
            /* empty document */
            emit_empty(p);
        } else {
            st = parse_block_node(p);
            if (st != YAM_OK) return st;
        }

        return YAM_OK;
    }

    if (tok_type(p) == YAM_TOK_DOC_END) {
        if (p->doc_open) {
            unroll_all(p, p->current.start);
            yam_event evt = evt_simple(YAM_EVT_DOC_END);
            evt.implicit = false;
            evt.start = p->current.start;
            evt.end = p->current.end;
            enqueue(p, &evt);
            p->doc_open = false;
        }
        consume_token(p);
        return YAM_OK;
    }

    if (tok_type(p) == YAM_TOK_STREAM_END) {
        return YAM_OK; /* handled by caller */
    }

    /* implicit document */
    if (!p->doc_open) {
        ensure_doc(p, false, p->current.start);
    }

    st = parse_block_node(p);
    if (st != YAM_OK) return st;
    return YAM_OK;
}

/* ── Parse stream ────────────────────────────────────────── */

static yam_status parse_stream(yam_parser *p) {
    yam_status st;

    /* consume STREAM_START */
    st = peek_token(p);
    if (st != YAM_OK) return st;
    yam_mark stream_start = p->current.start;
    yam_mark stream_start_end = p->current.end;
    consume_token(p);

    {
        yam_event sevt = evt_simple(YAM_EVT_STREAM_START);
        sevt.start = stream_start;
        sevt.end = stream_start_end;
        enqueue(p, &sevt);
    }
    p->stream_started = true;

    /* parse documents */
    int guard = 0;
    for (;;) {
        st = peek_token(p);
        if (st != YAM_OK) return st;

        if (tok_type(p) == YAM_TOK_STREAM_END || tok_type(p) == YAM_TOK_NONE) {
            if (p->doc_open) {
                unroll_all(p, p->current.start);
                yam_event evt = evt_simple(YAM_EVT_DOC_END);
                evt.implicit = true;
                evt.start = p->current.start;
                evt.end = p->current.start;
                enqueue(p, &evt);
                p->doc_open = false;
            }
            yam_mark end_mark = p->current.start;
            yam_mark end_mark_end = p->current.end;
            consume_token(p);
            {
                yam_event se = evt_simple(YAM_EVT_STREAM_END);
                se.start = end_mark;
                se.end = end_mark_end;
                enqueue(p, &se);
            }
            p->stream_ended = true;
            return YAM_OK;
        }

        /* safety: save position to detect non-progress */
        int prev_evts = p->evt_len;

        st = parse_document(p);
        if (st != YAM_OK) return st;

        /* if no progress was made, consume the problematic token */
        if (p->evt_len == prev_evts) {
            consume_token(p);
            if (++guard > 10000) PARSE_ERROR(p, "parser stuck, possible malformed input");
        } else {
            guard = 0;
        }
    }
}

/* ── Merge key resolution ────────────────────────────────── */

/* Compute the exclusive end index for the node starting at idx */
static int node_end(const yam_event *events, int len, int idx) {
    yam_event_type t = events[idx].type;
    if (t == YAM_EVT_SCALAR || t == YAM_EVT_ALIAS)
        return idx + 1;
    if (t == YAM_EVT_MAPPING_START || t == YAM_EVT_SEQUENCE_START) {
        yam_event_type end_t = (t == YAM_EVT_MAPPING_START)
            ? YAM_EVT_MAPPING_END : YAM_EVT_SEQUENCE_END;
        int depth = 1;
        for (int j = idx + 1; j < len; j++) {
            if (events[j].type == t) depth++;
            else if (events[j].type == end_t && --depth == 0) return j + 1;
        }
    }
    return idx + 1;
}

/* Anchor table */
typedef struct {
    yam_str name;
    int     start;  /* index of anchored node */
    int     end;    /* exclusive end */
} merge_anchor;

typedef struct {
    merge_anchor *entries;
    int           len;
    int           cap;
} merge_anchor_table;

static void atbl_init(merge_anchor_table *t) {
    t->entries = NULL; t->len = 0; t->cap = 0;
}

static void atbl_free(merge_anchor_table *t) {
    free(t->entries);
    t->entries = NULL; t->len = 0; t->cap = 0;
}

static void atbl_add(merge_anchor_table *t, yam_str name, int start, int end) {
    if (t->len >= t->cap) {
        int nc = t->cap ? t->cap * 2 : 16;
        merge_anchor *ne = realloc(t->entries, nc * sizeof(merge_anchor));
        if (!ne) return;
        t->entries = ne; t->cap = nc;
    }
    t->entries[t->len++] = (merge_anchor){name, start, end};
}

/* TODO: linear scan — replace with a hash table if anchor-heavy inputs
 * become a bottleneck (currently fine for typical anchor counts). */
static merge_anchor *atbl_lookup(merge_anchor_table *t, yam_str name) {
    for (int i = t->len - 1; i >= 0; i--) {
        if (t->entries[i].name.len == name.len &&
            memcmp(t->entries[i].name.data, name.data, name.len) == 0)
            return &t->entries[i];
    }
    return NULL;
}

static void atbl_build(merge_anchor_table *t, const yam_event *events, int len) {
    atbl_init(t);
    for (int i = 0; i < len; i++) {
        if (events[i].anchor.data && events[i].anchor.len > 0) {
            int end = node_end(events, len, i);
            atbl_add(t, events[i].anchor, i, end);
        }
    }
}

/* Dynamic event array for building output */
typedef struct {
    yam_event *data;
    int        len;
    int        cap;
    bool       oom;
} evt_buf;

static void ebuf_init(evt_buf *b, int cap) {
    b->data = malloc(cap * sizeof(yam_event));
    b->len = 0;
    b->cap = cap;
    b->oom = !b->data;
}

static void ebuf_free(evt_buf *b) {
    free(b->data);
    b->data = NULL; b->len = 0; b->cap = 0;
}

static void ebuf_push(evt_buf *b, yam_event e) {
    if (b->oom) return;
    if (b->len >= b->cap) {
        int nc = b->cap * 2;
        yam_event *nd = realloc(b->data, nc * sizeof(yam_event));
        if (!nd) { b->oom = true; return; }
        b->data = nd; b->cap = nc;
    }
    b->data[b->len++] = e;
}

static void ebuf_push_range(evt_buf *b, const yam_event *events, int start, int end) {
    for (int i = start; i < end; i++)
        ebuf_push(b, events[i]);
}

/* Key set for tracking seen keys (override semantics) */
typedef struct {
    yam_str *keys;
    int      len;
    int      cap;
} key_set;

static void kset_init(key_set *ks) {
    ks->keys = NULL; ks->len = 0; ks->cap = 0;
}

static void kset_free(key_set *ks) {
    free(ks->keys);
    ks->keys = NULL; ks->len = 0; ks->cap = 0;
}

static bool kset_contains(const key_set *ks, yam_str key) {
    for (int i = 0; i < ks->len; i++) {
        if (ks->keys[i].len == key.len &&
            memcmp(ks->keys[i].data, key.data, key.len) == 0)
            return true;
    }
    return false;
}

static void kset_add(key_set *ks, yam_str key) {
    if (kset_contains(ks, key)) return;
    if (ks->len >= ks->cap) {
        int nc = ks->cap ? ks->cap * 2 : 16;
        yam_str *nk = realloc(ks->keys, nc * sizeof(yam_str));
        if (!nk) return;
        ks->keys = nk; ks->cap = nc;
    }
    ks->keys[ks->len++] = key;
}

static bool is_merge_key(const yam_event *evt) {
    return evt->type == YAM_EVT_SCALAR
        && evt->scalar_style == YAM_SCALAR_PLAIN
        && evt->value.len == 2
        && evt->value.data[0] == '<' && evt->value.data[1] == '<';
}

/* Merge key-value pairs from a source mapping into buf, skipping already-seen keys */
static void merge_from_mapping(const yam_event *events, int evt_len,
                               int map_start, int map_end,
                               evt_buf *out, key_set *seen) {
    /* iterate key-value pairs inside the mapping (skip MAPPING_START/END) */
    int pos = map_start + 1;
    while (pos < map_end - 1) {
        int key_start = pos;
        int key_end = node_end(events, evt_len, pos);
        int val_start = key_end;
        int val_end = node_end(events, evt_len, val_start);

        /* only scalar keys participate in override checking */
        bool is_scalar_key = (key_end - key_start == 1 &&
                              events[key_start].type == YAM_EVT_SCALAR);
        bool skip = false;
        if (is_scalar_key) {
            yam_str kv = events[key_start].value;
            if (kset_contains(seen, kv)) {
                skip = true;
            } else {
                kset_add(seen, kv);
            }
        }

        if (!skip) {
            /* copy key events, stripping anchors from top-level */
            for (int j = key_start; j < key_end; j++) {
                yam_event copy = events[j];
                if (j == key_start) copy.anchor = YAM_STR_NULL;
                ebuf_push(out, copy);
            }
            /* copy value events */
            for (int j = val_start; j < val_end; j++) {
                yam_event copy = events[j];
                if (j == val_start) copy.anchor = YAM_STR_NULL;
                ebuf_push(out, copy);
            }
        }

        pos = val_end;
    }
}

/* Process a single merge value (ALIAS or SEQUENCE of aliases) */
static void process_merge_value(const yam_event *events, int evt_len,
                                int val_start, int val_end,
                                merge_anchor_table *anchors,
                                evt_buf *out, key_set *seen) {
    if (events[val_start].type == YAM_EVT_ALIAS) {
        /* <<: *alias */
        merge_anchor *a = atbl_lookup(anchors, events[val_start].value);
        if (a && events[a->start].type == YAM_EVT_MAPPING_START) {
            merge_from_mapping(events, evt_len, a->start, a->end, out, seen);
        }
    } else if (events[val_start].type == YAM_EVT_SEQUENCE_START) {
        /* <<: [*a, *b, ...] */
        int pos = val_start + 1;
        int seq_end = val_end - 1; /* SEQUENCE_END */
        while (pos < seq_end) {
            if (events[pos].type == YAM_EVT_ALIAS) {
                merge_anchor *a = atbl_lookup(anchors, events[pos].value);
                if (a && events[a->start].type == YAM_EVT_MAPPING_START) {
                    merge_from_mapping(events, evt_len, a->start, a->end,
                                       out, seen);
                }
                pos++;
            } else {
                pos = node_end(events, evt_len, pos);
            }
        }
    }
    /* other value types (scalar, mapping) — silently ignored */
}

static yam_status resolve_merges(yam_parser *p) {
    #define MAX_MERGE_PASSES 32

    for (int pass = 0; pass < MAX_MERGE_PASSES; pass++) {
        merge_anchor_table anchors;
        atbl_build(&anchors, p->events, p->evt_len);

        evt_buf out;
        ebuf_init(&out, p->evt_len * 2);
        bool changed = false;

        int i = 0;
        while (i < p->evt_len) {
            if (p->events[i].type != YAM_EVT_MAPPING_START) {
                ebuf_push(&out, p->events[i]);
                i++;
                continue;
            }

            /* found a mapping — find its end */
            int map_start = i;
            int map_end = node_end(p->events, p->evt_len, i);

            /* check if this mapping has any merge keys */
            bool has_merge = false;
            {
                int pos = map_start + 1;
                while (pos < map_end - 1) {
                    int ke = node_end(p->events, p->evt_len, pos);
                    if (is_merge_key(&p->events[pos])) { has_merge = true; break; }
                    int ve = node_end(p->events, p->evt_len, ke);
                    pos = ve;
                }
            }

            if (!has_merge) {
                /* no merge keys at this level — just push MAPPING_START
                 * and let the loop continue to process inner events
                 * (which may contain nested mappings with merge keys) */
                ebuf_push(&out, p->events[i]);
                i++;
                continue;
            }

            /* this mapping has merges — rebuild it */
            changed = true;

            /* first collect explicit (non-merge) keys */
            key_set seen;
            kset_init(&seen);

            /* pass 1: record all explicit keys */
            {
                int pos = map_start + 1;
                while (pos < map_end - 1) {
                    int key_start = pos;
                    int key_end = node_end(p->events, p->evt_len, pos);
                    int val_end = node_end(p->events, p->evt_len, key_end);

                    if (!is_merge_key(&p->events[key_start])) {
                        if (key_end - key_start == 1 &&
                            p->events[key_start].type == YAM_EVT_SCALAR) {
                            kset_add(&seen, p->events[key_start].value);
                        }
                    }
                    pos = val_end;
                }
            }

            /* emit MAPPING_START */
            ebuf_push(&out, p->events[map_start]);

            /* pass 2: emit explicit pairs first */
            {
                int pos = map_start + 1;
                while (pos < map_end - 1) {
                    int key_start = pos;
                    int key_end = node_end(p->events, p->evt_len, pos);
                    int val_end = node_end(p->events, p->evt_len, key_end);

                    if (!is_merge_key(&p->events[key_start])) {
                        ebuf_push_range(&out, p->events, key_start, val_end);
                    }
                    pos = val_end;
                }
            }

            /* pass 3: process merge keys in order, injecting non-duplicate pairs */
            {
                int pos = map_start + 1;
                while (pos < map_end - 1) {
                    int key_start = pos;
                    int key_end = node_end(p->events, p->evt_len, pos);
                    int val_start = key_end;
                    int val_end = node_end(p->events, p->evt_len, key_end);

                    if (is_merge_key(&p->events[key_start])) {
                        process_merge_value(p->events, p->evt_len,
                                            val_start, val_end,
                                            &anchors, &out, &seen);
                    }
                    pos = val_end;
                }
            }

            /* emit MAPPING_END */
            ebuf_push(&out, p->events[map_end - 1]);

            kset_free(&seen);
            i = map_end;
        }

        atbl_free(&anchors);

        if (out.oom) {
            ebuf_free(&out);
            return YAM_ERR_MEMORY;
        }

        if (!changed) {
            ebuf_free(&out);
            break;
        }

        /* replace event array */
        free(p->events);
        p->events = out.data;
        p->evt_len = out.len;
        p->evt_cap = out.cap;
        p->evt_cursor = 0;
        /* don't free out — ownership transferred */
    }

    return YAM_OK;
    #undef MAX_MERGE_PASSES
}

/* ── Alias resolution ────────────────────────────────────── */

enum { ACYCLE_WHITE = 0, ACYCLE_GRAY = 1, ACYCLE_BLACK = 2 };

static bool alias_has_cycle(int idx, merge_anchor_table *t, int *color,
                            const yam_event *events) {
    color[idx] = ACYCLE_GRAY;
    for (int j = t->entries[idx].start; j < t->entries[idx].end; j++) {
        if (events[j].type != YAM_EVT_ALIAS) continue;
        for (int k = 0; k < t->len; k++) {
            if (t->entries[k].name.len == events[j].value.len &&
                memcmp(t->entries[k].name.data, events[j].value.data,
                       events[j].value.len) == 0) {
                if (color[k] == ACYCLE_GRAY) return true;
                if (color[k] == ACYCLE_WHITE &&
                    alias_has_cycle(k, t, color, events))
                    return true;
            }
        }
    }
    color[idx] = ACYCLE_BLACK;
    return false;
}

static bool is_cyclic_name(merge_anchor_table *t, bool *cyclic, yam_str name) {
    for (int i = 0; i < t->len; i++) {
        if (t->entries[i].name.len == name.len &&
            memcmp(t->entries[i].name.data, name.data, name.len) == 0)
            return cyclic[i];
    }
    return false;
}

static yam_status resolve_aliases(yam_parser *p) {
    /* build initial anchor table for cycle detection */
    merge_anchor_table anchors;
    atbl_build(&anchors, p->events, p->evt_len);

    if (anchors.len == 0) {
        atbl_free(&anchors);
        return YAM_OK;
    }

    /* detect cycles via DFS */
    int *color = calloc(anchors.len, sizeof(int));
    bool *cyclic = calloc(anchors.len, sizeof(bool));
    if (!color || !cyclic) {
        free(color); free(cyclic);
        atbl_free(&anchors);
        return YAM_ERR_MEMORY;
    }

    for (int i = 0; i < anchors.len; i++) {
        if (color[i] == ACYCLE_WHITE) {
            if (alias_has_cycle(i, &anchors, color, p->events)) {
                for (int k = 0; k < anchors.len; k++)
                    if (color[k] == ACYCLE_GRAY) cyclic[k] = true;
            }
        }
    }
    atbl_free(&anchors);

    /* expand non-cyclic aliases in passes */
    for (int pass = 0; pass < 32; pass++) {
        atbl_build(&anchors, p->events, p->evt_len);

        evt_buf out;
        ebuf_init(&out, p->evt_len * 2);
        bool changed = false;

        for (int i = 0; i < p->evt_len; i++) {
            if (p->events[i].type != YAM_EVT_ALIAS) {
                ebuf_push(&out, p->events[i]);
                continue;
            }

            merge_anchor *a = atbl_lookup(&anchors, p->events[i].value);
            if (!a || is_cyclic_name(&anchors, cyclic, p->events[i].value)) {
                ebuf_push(&out, p->events[i]);
                continue;
            }

            changed = true;
            for (int j = a->start; j < a->end; j++) {
                yam_event copy = p->events[j];
                if (j == a->start) copy.anchor = YAM_STR_NULL;
                ebuf_push(&out, copy);
            }
        }

        atbl_free(&anchors);

        if (out.oom) {
            ebuf_free(&out);
            free(color);
            free(cyclic);
            return YAM_ERR_MEMORY;
        }

        if (!changed) {
            ebuf_free(&out);
            break;
        }

        free(p->events);
        p->events = out.data;
        p->evt_len = out.len;
        p->evt_cap = out.cap;
        p->evt_cursor = 0;
    }

    free(color);
    free(cyclic);
    return YAM_OK;
}

/* ── Flow-as-key lookahead ──────────────────────────────── */

/* Scan raw bytes from offset to find the matching ] or } at depth 0,
 * then check if ':' follows (skipping whitespace and comments).
 * Returns true if the flow collection is a complex block key. */
static bool flow_is_block_key(const char *input, size_t len, size_t offset) {
    int depth = 0;
    bool in_sq = false, in_dq = false;
    for (size_t i = offset; i < len; i++) {
        char c = input[i];
        if (in_sq) {
            if (c == '\'' && i + 1 < len && input[i + 1] == '\'') i++;
            else if (c == '\'') in_sq = false;
            continue;
        }
        if (in_dq) {
            if (c == '\\') { i++; continue; }
            if (c == '"') in_dq = false;
            continue;
        }
        if (c == '#') { while (i < len && input[i] != '\n') i++; continue; }
        if (c == '\'') { in_sq = true; continue; }
        if (c == '"') { in_dq = true; continue; }
        if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') {
            depth--;
            if (depth == 0) {
                for (size_t j = i + 1; j < len; j++) {
                    char nc = input[j];
                    if (nc == ' ' || nc == '\t') continue;
                    if (nc == '\n' || nc == '\r') continue;
                    if (nc == '#') {
                        while (j < len && input[j] != '\n') j++;
                        continue;
                    }
                    return nc == ':';
                }
                return false;
            }
        }
    }
    return false;
}

/* ── Incremental state machine ───────────────────────────── */

static inline void inc_emit(yam_parser *p, yam_event evt) {
    if (p->max_events > 0 && p->events_delivered + p->out_len >= p->max_events) {
        p->oom = true;
        return;
    }
    if (p->out_len < 8)
        p->out_buf[p->out_len++] = evt;
}

static inline void inc_push_frame(yam_parser *p, ctx_type type, int indent,
                                   parser_state return_state) {
    if (p->frame_len >= p->frame_cap) {
        int nc = p->frame_cap * 2;
        state_frame *nf = realloc(p->frames, nc * sizeof(state_frame));
        if (!nf) { p->oom = true; return; }
        p->frames = nf;
        p->frame_cap = nc;
    }
    p->frames[p->frame_len++] = (state_frame){type, indent, return_state};
}

static inline state_frame inc_pop_frame(yam_parser *p) {
    if (p->frame_len > 0) return p->frames[--p->frame_len];
    return (state_frame){0, -1, ST_DONE};
}

static inline state_frame *inc_top_frame(yam_parser *p) {
    if (p->frame_len > 0) return &p->frames[p->frame_len - 1];
    return NULL;
}

static yam_status parser_step(yam_parser *p) {
    yam_token_type tt;
    int col;
    yam_mark mark;
    yam_event evt;
    state_frame *top;

    if (p->oom) return YAM_ERR_MEMORY;

    switch (p->state) {

    case ST_STREAM_START:
        evt = evt_simple(YAM_EVT_STREAM_START);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.end;
        consume_token(p);
        inc_emit(p, evt);
        p->state = ST_STREAM_DOC_LOOP;
        return YAM_OK;

    case ST_STREAM_DOC_LOOP:
        peek_token(p);
        tt = tok_type(p);
        if (tt == YAM_TOK_STREAM_END) {
            p->state = ST_STREAM_END;
            return YAM_OK;
        }
        p->state = ST_DOC_DIRECTIVES;
        return YAM_OK;

    case ST_STREAM_END:
        evt = evt_simple(YAM_EVT_STREAM_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.end;
        consume_token(p);
        inc_emit(p, evt);
        p->stream_ended = true;
        p->state = ST_DONE;
        return YAM_OK;

    case ST_DOC_DIRECTIVES:
        /* check for %TAG / %YAML directives — fall back to eager */
        peek_token(p);
        tt = tok_type(p);
        if ((tt == YAM_TOK_TAG && p->current.value.len > 0 &&
             p->current.value.data[0] == '%') ||
            (tt == YAM_TOK_SCALAR && p->current.value.len > 0 &&
             p->current.value.data[0] == '%')) {
            p->state = ST_EAGER_DRAIN;
            return YAM_OK;
        }
        if (tt == YAM_TOK_DOC_START) {
            p->state = ST_DOC_START_EXPLICIT;
        } else if (tt == YAM_TOK_DOC_END) {
            /* doc end without content, emit implicit doc */
            consume_token(p);
            p->state = ST_STREAM_DOC_LOOP;
        } else if (tt == YAM_TOK_STREAM_END) {
            p->state = ST_STREAM_END;
        } else {
            p->state = ST_DOC_START_IMPLICIT;
        }
        return YAM_OK;

    case ST_DOC_START_EXPLICIT: {
        /* close previous doc if open */
        if (p->doc_open) {
            /* unroll any open collections */
            if (p->frame_len > 0) {
                p->unroll_return = ST_DOC_START_EXPLICIT;
                p->state = ST_UNROLL;
                return YAM_OK;
            }
            evt = evt_simple(YAM_EVT_DOC_END);
            evt.implicit = true;
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
            p->doc_open = false;
        }
        peek_token(p);
        mark = p->current.start;
        consume_token(p);
        evt = evt_simple(YAM_EVT_DOC_START);
        evt.implicit = false;
        evt.start = mark;
        evt.end = p->current.start;
        inc_emit(p, evt);
        p->doc_open = true;
        p->state = ST_DOC_CONTENT;
        return YAM_OK;
    }

    case ST_DOC_START_IMPLICIT: {
        if (p->doc_open) {
            if (p->frame_len > 0) {
                p->unroll_return = ST_DOC_START_IMPLICIT;
                p->state = ST_UNROLL;
                return YAM_OK;
            }
            evt = evt_simple(YAM_EVT_DOC_END);
            evt.implicit = true;
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
            p->doc_open = false;
        }
        peek_token(p);
        evt = evt_simple(YAM_EVT_DOC_START);
        evt.implicit = true;
        evt.start = p->current.start;
        evt.end = p->current.end;
        inc_emit(p, evt);
        p->doc_open = true;
        p->state = ST_DOC_CONTENT;
        return YAM_OK;
    }

    case ST_DOC_CONTENT:
        peek_token(p);
        tt = tok_type(p);
        if (tt == YAM_TOK_DOC_END) {
            /* empty document body */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
            p->state = ST_DOC_END_EXPLICIT;
            return YAM_OK;
        }
        if (tt == YAM_TOK_DOC_START) {
            /* empty document body, new doc follows */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
            /* close this doc implicitly */
            evt = evt_simple(YAM_EVT_DOC_END);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
            p->doc_open = false;
            p->state = ST_STREAM_DOC_LOOP;
            return YAM_OK;
        }
        if (tt == YAM_TOK_STREAM_END) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
            evt = evt_simple(YAM_EVT_DOC_END);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
            p->doc_open = false;
            p->state = ST_STREAM_END;
            return YAM_OK;
        }
        /* non-trivial content — parse as block node */
        p->node_return = ST_DOC_END_EXPLICIT;
        p->state = ST_BLOCK_NODE;
        return YAM_OK;

    case ST_BLOCK_NODE:
        /* Consume properties (anchor, tag) */
        peek_token(p);
        tt = tok_type(p);

        /* Consume anchor/tag properties */
        if (tt == YAM_TOK_TAG || tt == YAM_TOK_ANCHOR) {
            /* Fall back to eager for nodes with properties — the double-props
             * logic is complex and interacts with collection start detection.
             * We'll handle the simple (no props) cases incrementally first. */
            p->state = ST_EAGER_DRAIN;
            return YAM_OK;
        }

        if (tt == YAM_TOK_ALIAS) {
            p->state = ST_BLOCK_NODE_ALIAS;
            return YAM_OK;
        }

        if (tt == YAM_TOK_BLOCK_SEQ_ENTRY) {
            p->state = ST_BLOCK_NODE_SEQ;
            return YAM_OK;
        }

        if (tt == YAM_TOK_BLOCK_MAP_KEY) {
            p->state = ST_BLOCK_NODE_EXPLICIT_KEY;
            return YAM_OK;
        }

        if (tt == YAM_TOK_BLOCK_MAP_VALUE) {
            p->state = ST_BLOCK_NODE_BARE_VALUE;
            return YAM_OK;
        }

        if (tt == YAM_TOK_FLOW_SEQ_START || tt == YAM_TOK_FLOW_MAP_START) {
            /* Check if this flow collection is used as a complex block key
             * ([flow]: value). Map values can't be keys; for other positions
             * scan ahead to check if ':' follows the matching close bracket. */
            if (p->node_return == ST_BLOCK_MAP_LOOP ||
                !flow_is_block_key(p->input, p->input_len,
                                   p->current.start.offset)) {
                p->state = ST_FLOW_NODE;
            } else {
                p->state = ST_EAGER_DRAIN;
            }
            return YAM_OK;
        }

        if (tt == YAM_TOK_SCALAR) {
            p->state = ST_BLOCK_NODE_SCALAR;
            return YAM_OK;
        }

        /* DOC_START, DOC_END, STREAM_END → empty node */
        p->state = ST_BLOCK_NODE_EMPTY;
        return YAM_OK;

    case ST_BLOCK_NODE_SCALAR: {
        peek_token(p);
        mark = p->current.start;
        col = tok_col(p);
        evt = (yam_event){
            .type = YAM_EVT_SCALAR,
            .value = p->current.value,
            .scalar_style = p->current.scalar_style,
            .start = p->current.start,
            .end = p->current.end,
        };
        consume_token(p);

        /* peek for ':' → this scalar is a mapping key
         * Only if ':' col >= scalar col (otherwise ':' belongs to parent map) */
        peek_token(p);
        tt = tok_type(p);
        if (tt == YAM_TOK_BLOCK_MAP_VALUE && tok_col(p) >= col) {
            p->saved_node = evt;
            p->saved_node_col = col;
            p->have_saved_node = true;
            p->state = ST_BLOCK_NODE_SCALAR_KEY;
            return YAM_OK;
        }

        /* plain scalar value */
        inc_emit(p, evt);

        /* return to parent context */
        p->state = p->node_return;
        return YAM_OK;
    }

    case ST_BLOCK_NODE_SCALAR_KEY: {
        /* We have a saved scalar that's a key → emit MAPPING_START + key scalar */
        evt = evt_simple(YAM_EVT_MAPPING_START);
        evt.implicit = true;
        evt.start = p->saved_node.start;
        evt.end = p->saved_node.start;
        inc_emit(p, evt);

        /* emit the key scalar */
        inc_emit(p, p->saved_node);
        p->have_saved_node = false;

        /* push block map context — return to caller's node_return after map ends */
        inc_push_frame(p, CTX_BLOCK_MAP, p->saved_node_col, p->node_return);

        /* consume ':' and parse value */
        p->state = ST_BLOCK_MAP_VALUE;
        return YAM_OK;
    }

    case ST_BLOCK_NODE_ALIAS: {
        peek_token(p);
        mark = p->current.start;
        col = tok_col(p);
        evt = (yam_event){
            .type = YAM_EVT_ALIAS,
            .value = p->current.value,
            .start = p->current.start,
            .end = p->current.end,
        };
        consume_token(p);

        /* peek for ':' → alias as mapping key
         * Only if ':' col >= alias col (otherwise ':' belongs to parent map) */
        peek_token(p);
        tt = tok_type(p);
        if (tt == YAM_TOK_BLOCK_MAP_VALUE && tok_col(p) >= col) {
            p->saved_node = evt;
            p->saved_node_col = col;
            p->have_saved_node = true;
            p->state = ST_BLOCK_NODE_ALIAS_KEY;
            return YAM_OK;
        }

        inc_emit(p, evt);

        /* return to parent context */
        p->state = p->node_return;
        return YAM_OK;
    }

    case ST_BLOCK_NODE_ALIAS_KEY: {
        evt = evt_simple(YAM_EVT_MAPPING_START);
        evt.implicit = true;
        evt.start = p->saved_node.start;
        evt.end = p->saved_node.start;
        inc_emit(p, evt);
        inc_emit(p, p->saved_node);
        p->have_saved_node = false;
        inc_push_frame(p, CTX_BLOCK_MAP, p->saved_node_col, p->node_return);
        p->state = ST_BLOCK_MAP_VALUE;
        return YAM_OK;
    }

    case ST_BLOCK_NODE_SEQ: {
        peek_token(p);
        int seq_indent = tok_col(p);
        evt = evt_simple(YAM_EVT_SEQUENCE_START);
        evt.start = p->current.start;
        evt.end = p->current.start;

        inc_emit(p, evt);
        inc_push_frame(p, CTX_BLOCK_SEQ, seq_indent, p->node_return);
        p->state = ST_BLOCK_SEQ_LOOP;
        return YAM_OK;
    }

    case ST_BLOCK_NODE_EXPLICIT_KEY: {
        /* ? key: starts a mapping */
        peek_token(p);
        int map_indent = tok_col(p);
        evt = evt_simple(YAM_EVT_MAPPING_START);
        evt.start = p->current.start;
        evt.end = p->current.start;

        inc_emit(p, evt);
        inc_push_frame(p, CTX_BLOCK_MAP, map_indent, p->node_return);
        p->state = ST_BLOCK_MAP_LOOP;
        return YAM_OK;
    }

    case ST_BLOCK_NODE_BARE_VALUE: {
        /* : at start → mapping with empty key */
        peek_token(p);
        int map_indent = tok_col(p);
        evt = evt_simple(YAM_EVT_MAPPING_START);
        evt.start = p->current.start;
        evt.end = p->current.start;

        inc_emit(p, evt);
        inc_push_frame(p, CTX_BLOCK_MAP, map_indent, p->node_return);

        /* emit empty key */
        evt = evt_simple(YAM_EVT_SCALAR);
        evt.implicit = true;
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, evt);

        p->state = ST_BLOCK_MAP_VALUE;
        return YAM_OK;
    }

    case ST_BLOCK_NODE_EMPTY: {
        peek_token(p);
        evt = evt_simple(YAM_EVT_SCALAR);
        evt.implicit = true;
        evt.start = p->current.start;
        evt.end = p->current.end;
        inc_emit(p, evt);

        /* return to parent context */
        p->state = p->node_return;
        return YAM_OK;
    }

    case ST_BLOCK_MAP_LOOP: {
        peek_token(p);
        tt = tok_type(p);
        col = tok_col(p);
        top = inc_top_frame(p);
        int map_indent = top ? top->indent : 0;

        /* check if we're still in this mapping's indent level */
        if (tt == YAM_TOK_BLOCK_MAP_KEY && col == map_indent) {
            p->state = ST_BLOCK_MAP_EXPLICIT_KEY;
            return YAM_OK;
        }

        if (tt == YAM_TOK_SCALAR || tt == YAM_TOK_ALIAS) {
            /* Simple key: must be at map indent and followed by ':' */
            if (col == map_indent) {
                p->state = ST_BLOCK_MAP_SIMPLE_KEY;
                return YAM_OK;
            }
        }

        if (tt == YAM_TOK_BLOCK_MAP_VALUE && col == map_indent) {
            /* empty key, value */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_MAP_VALUE;
            return YAM_OK;
        }

        if (tt == YAM_TOK_TAG || tt == YAM_TOK_ANCHOR) {
            /* Properties in mapping → fall back to eager */
            p->state = ST_EAGER_DRAIN;
            return YAM_OK;
        }

        if (tt == YAM_TOK_FLOW_SEQ_START || tt == YAM_TOK_FLOW_MAP_START) {
            /* Flow key in block mapping → eager fallback */
            p->state = ST_EAGER_DRAIN;
            return YAM_OK;
        }

        /* end of this mapping */
        p->state = ST_BLOCK_MAP_END;
        return YAM_OK;
    }

    case ST_BLOCK_MAP_EXPLICIT_KEY: {
        /* consume '?' */
        peek_token(p);
        consume_token(p);

        /* parse key node */
        peek_token(p);
        tt = tok_type(p);
        top = inc_top_frame(p);
        int ek_indent = top ? top->indent : 0;

        if (tt == YAM_TOK_BLOCK_MAP_VALUE && tok_col(p) == ek_indent) {
            /* empty key (: at map indent) */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_MAP_VALUE;
        } else {
            /* The key is a block node */
            p->node_return = ST_BLOCK_MAP_POST_KEY;
            p->state = ST_BLOCK_NODE;
        }
        return YAM_OK;
    }

    case ST_BLOCK_MAP_SIMPLE_KEY: {
        peek_token(p);
        tt = tok_type(p);

        if (tt == YAM_TOK_SCALAR) {
            mark = p->current.start;
            col = tok_col(p);
            evt = (yam_event){
                .type = YAM_EVT_SCALAR,
                .value = p->current.value,
                .scalar_style = p->current.scalar_style,
                .start = p->current.start,
                .end = p->current.end,
            };
            consume_token(p);

            /* must be followed by ':' */
            peek_token(p);
            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
                inc_emit(p, evt);
                p->state = ST_BLOCK_MAP_VALUE;
            } else {
                /* Not a key — fall back to eager since this shouldn't happen
                 * in well-formed YAML at the map indent level */
                p->state = ST_EAGER_DRAIN;
            }
        } else if (tt == YAM_TOK_ALIAS) {
            mark = p->current.start;
            evt = (yam_event){
                .type = YAM_EVT_ALIAS,
                .value = p->current.value,
                .start = p->current.start,
                .end = p->current.end,
            };
            consume_token(p);

            peek_token(p);
            if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
                inc_emit(p, evt);
                p->state = ST_BLOCK_MAP_VALUE;
            } else {
                p->state = ST_EAGER_DRAIN;
            }
        } else {
            p->state = ST_EAGER_DRAIN;
        }
        return YAM_OK;
    }

    case ST_BLOCK_MAP_POST_KEY: {
        /* After an explicit key's node, look for ':' */
        peek_token(p);
        tt = tok_type(p);
        top = inc_top_frame(p);
        int map_indent = top ? top->indent : 0;

        if (tt == YAM_TOK_BLOCK_MAP_VALUE && tok_col(p) == map_indent) {
            p->state = ST_BLOCK_MAP_VALUE;
        } else {
            /* missing value → emit empty value, continue loop */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_MAP_LOOP;
        }
        return YAM_OK;
    }

    case ST_BLOCK_MAP_VALUE: {
        /* consume ':' */
        peek_token(p);
        if (tok_type(p) != YAM_TOK_BLOCK_MAP_VALUE) {
            /* missing value — emit empty */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return YAM_OK;
        }
        consume_token(p);

        /* peek at what follows */
        peek_token(p);
        tt = tok_type(p);
        col = tok_col(p);
        top = inc_top_frame(p);
        int map_indent = top ? top->indent : 0;

        /* empty value: next token at same/less indent, or is doc/stream marker */
        if (tt == YAM_TOK_DOC_START || tt == YAM_TOK_DOC_END ||
            tt == YAM_TOK_STREAM_END) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return YAM_OK;
        }
        /* next key/value/scalar/anchor/tag at map indent → empty value */
        if (col == map_indent && (tt == YAM_TOK_SCALAR ||
            tt == YAM_TOK_ANCHOR || tt == YAM_TOK_TAG ||
            tt == YAM_TOK_BLOCK_MAP_KEY || tt == YAM_TOK_BLOCK_MAP_VALUE)) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return YAM_OK;
        }
        /* any token at lesser indent than map → empty value
         * (seq entries at same indent are valid values per YAML spec) */
        if (col < map_indent && tt != YAM_TOK_BLOCK_SEQ_ENTRY) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return YAM_OK;
        }
        if (tt == YAM_TOK_BLOCK_SEQ_ENTRY && col < map_indent) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return YAM_OK;
        }

        /* the value is a block node — set return to map loop */
        p->node_return = ST_BLOCK_MAP_LOOP;
        p->state = ST_BLOCK_NODE;
        return YAM_OK;
    }

    case ST_BLOCK_MAP_VALUE_NODE:
        /* After parsing the value node, continue the map loop */
        p->state = ST_BLOCK_MAP_LOOP;
        return YAM_OK;

    case ST_BLOCK_MAP_END: {
        state_frame frame = inc_pop_frame(p);
        evt = evt_simple(YAM_EVT_MAPPING_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.end;
        inc_emit(p, evt);

        /* handle doc close for top-level mapping */
        p->state = frame.return_state;
        if (p->state == ST_DOC_END_EXPLICIT) {
            peek_token(p);
            tt = tok_type(p);
            if (tt == YAM_TOK_DOC_END) {
                consume_token(p);
                evt = evt_simple(YAM_EVT_DOC_END);
                evt.implicit = false;
                evt.start = p->current.start;
                evt.end = p->current.end;
                inc_emit(p, evt);
                p->doc_open = false;
                p->state = ST_STREAM_DOC_LOOP;
            } else {
                evt = evt_simple(YAM_EVT_DOC_END);
                evt.implicit = true;
                evt.start = p->current.start;
                evt.end = p->current.end;
                inc_emit(p, evt);
                p->doc_open = false;
                p->state = ST_STREAM_DOC_LOOP;
            }
        }
        return YAM_OK;
    }

    case ST_BLOCK_SEQ_LOOP: {
        peek_token(p);
        tt = tok_type(p);
        col = tok_col(p);
        top = inc_top_frame(p);
        int seq_indent = top ? top->indent : 0;

        if (tt == YAM_TOK_BLOCK_SEQ_ENTRY && col == seq_indent) {
            p->state = ST_BLOCK_SEQ_ENTRY;
            return YAM_OK;
        }

        /* end of sequence */
        p->state = ST_BLOCK_SEQ_END;
        return YAM_OK;
    }

    case ST_BLOCK_SEQ_ENTRY: {
        /* consume '-' */
        peek_token(p);
        consume_token(p);

        /* peek at what follows */
        peek_token(p);
        tt = tok_type(p);
        col = tok_col(p);
        top = inc_top_frame(p);
        int seq_indent = top ? top->indent : 0;

        /* empty entry: next '-' at same indent, or end of seq */
        if (tt == YAM_TOK_BLOCK_SEQ_ENTRY && col == seq_indent) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_SEQ_LOOP;
            return YAM_OK;
        }
        if (tt == YAM_TOK_DOC_START || tt == YAM_TOK_DOC_END ||
            tt == YAM_TOK_STREAM_END) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_BLOCK_SEQ_LOOP;
            return YAM_OK;
        }

        /* entry has a value node */
        p->node_return = ST_BLOCK_SEQ_LOOP;
        p->state = ST_BLOCK_NODE;
        return YAM_OK;
    }

    case ST_BLOCK_SEQ_ENTRY_NODE:
        p->state = ST_BLOCK_SEQ_LOOP;
        return YAM_OK;

    case ST_BLOCK_SEQ_END: {
        state_frame frame = inc_pop_frame(p);
        evt = evt_simple(YAM_EVT_SEQUENCE_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.end;
        inc_emit(p, evt);

        p->state = frame.return_state;
        if (p->state == ST_DOC_END_EXPLICIT) {
            peek_token(p);
            tt = tok_type(p);
            if (tt == YAM_TOK_DOC_END) {
                consume_token(p);
                evt = evt_simple(YAM_EVT_DOC_END);
                evt.implicit = false;
                evt.start = p->current.start;
                evt.end = p->current.end;
                inc_emit(p, evt);
                p->doc_open = false;
                p->state = ST_STREAM_DOC_LOOP;
            } else {
                evt = evt_simple(YAM_EVT_DOC_END);
                evt.implicit = true;
                evt.start = p->current.start;
                evt.end = p->current.end;
                inc_emit(p, evt);
                p->doc_open = false;
                p->state = ST_STREAM_DOC_LOOP;
            }
        }
        return YAM_OK;
    }

    case ST_UNROLL: {
        /* Pop one frame per step, emit the corresponding end event */
        if (p->frame_len > 0) {
            state_frame frame = inc_pop_frame(p);
            if (frame.type == CTX_BLOCK_MAP || frame.type == CTX_FLOW_MAP) {
                evt = evt_simple(YAM_EVT_MAPPING_END);
            } else {
                evt = evt_simple(YAM_EVT_SEQUENCE_END);
            }
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
            return YAM_OK;
        }
        /* all frames popped, continue to the return state */
        p->state = p->unroll_return;
        return YAM_OK;
    }

    case ST_DOC_END_EXPLICIT: {
        peek_token(p);
        tt = tok_type(p);
        if (tt == YAM_TOK_DOC_END) {
            consume_token(p);
            evt = evt_simple(YAM_EVT_DOC_END);
            evt.implicit = false;
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
        } else {
            evt = evt_simple(YAM_EVT_DOC_END);
            evt.implicit = true;
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.end;
            inc_emit(p, evt);
        }
        p->doc_open = false;
        p->state = ST_STREAM_DOC_LOOP;
        return YAM_OK;
    }

    /* ── Flow node dispatch (incremental) ──────────────────── */

    case ST_FLOW_NODE: {
        peek_token(p);
        tt = tok_type(p);

        /* consume properties if present */
        if (tt == YAM_TOK_TAG || tt == YAM_TOK_ANCHOR) {
            yam_status st = consume_props(p);
            if (st != YAM_OK) return st;
            peek_token(p);
            tt = tok_type(p);
        }

        if (tt == YAM_TOK_SCALAR) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.value = p->current.value;
            evt.scalar_style = p->current.scalar_style;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);
            inc_emit(p, evt);
            p->state = p->node_return;
            return YAM_OK;
        }

        if (tt == YAM_TOK_ALIAS) {
            evt = evt_simple(YAM_EVT_ALIAS);
            evt.value = p->current.value;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);
            inc_emit(p, evt);
            p->state = p->node_return;
            return YAM_OK;
        }

        if (tt == YAM_TOK_FLOW_SEQ_START) {
            col = tok_col(p);
            evt = evt_simple(YAM_EVT_SEQUENCE_START);
            evt.flow = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);
            inc_emit(p, evt);
            inc_push_frame(p, CTX_FLOW_SEQ, col, p->node_return);
            p->state = ST_FLOW_SEQ_LOOP;
            return YAM_OK;
        }

        if (tt == YAM_TOK_FLOW_MAP_START) {
            col = tok_col(p);
            evt = evt_simple(YAM_EVT_MAPPING_START);
            evt.flow = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);
            inc_emit(p, evt);
            inc_push_frame(p, CTX_FLOW_MAP, col, p->node_return);
            p->state = ST_FLOW_MAP_LOOP;
            return YAM_OK;
        }

        /* empty node */
        evt = evt_simple(YAM_EVT_SCALAR);
        evt.start = p->current.start;
        evt.end = p->current.start;
        attach_props(p, &evt);
        inc_emit(p, evt);
        p->state = p->node_return;
        return YAM_OK;
    }

    /* ── Flow mapping states (incremental) ─────────────────── */

    case ST_FLOW_MAP_LOOP: {
        if (p->oom) return YAM_ERR_MEMORY;
        peek_token(p);
        tt = tok_type(p);

        if (tt == YAM_TOK_FLOW_MAP_END) {
            p->state = ST_FLOW_MAP_END;
            return YAM_OK;
        }

        /* explicit key: ? */
        if (tt == YAM_TOK_BLOCK_MAP_KEY) {
            consume_token(p);
            peek_token(p);
            tt = tok_type(p);
            if (tt == YAM_TOK_BLOCK_MAP_VALUE ||
                tt == YAM_TOK_FLOW_ENTRY ||
                tt == YAM_TOK_FLOW_MAP_END) {
                /* empty key */
                evt = evt_simple(YAM_EVT_SCALAR);
                evt.start = p->current.start;
                evt.end = p->current.start;
                inc_emit(p, evt);
                p->state = ST_FLOW_MAP_VALUE;
            } else {
                p->node_return = ST_FLOW_MAP_VALUE;
                p->state = ST_FLOW_NODE;
            }
            return YAM_OK;
        }

        /* implicit key: dispatch to flow node, then check for : */
        p->node_return = ST_FLOW_MAP_VALUE;
        p->state = ST_FLOW_NODE;
        return YAM_OK;
    }

    case ST_FLOW_MAP_VALUE: {
        peek_token(p);
        tt = tok_type(p);

        if (tt == YAM_TOK_BLOCK_MAP_VALUE) {
            consume_token(p);
            peek_token(p);
            tt = tok_type(p);
            if (tt == YAM_TOK_FLOW_ENTRY ||
                tt == YAM_TOK_FLOW_MAP_END) {
                /* empty value */
                evt = evt_simple(YAM_EVT_SCALAR);
                evt.start = p->current.start;
                evt.end = p->current.start;
                inc_emit(p, evt);
                p->state = ST_FLOW_MAP_SEP;
            } else {
                p->node_return = ST_FLOW_MAP_SEP;
                p->state = ST_FLOW_NODE;
            }
        } else {
            /* no ':', emit empty value */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_FLOW_MAP_SEP;
        }
        return YAM_OK;
    }

    case ST_FLOW_MAP_SEP: {
        peek_token(p);
        tt = tok_type(p);

        if (tt == YAM_TOK_FLOW_ENTRY) {
            consume_token(p);
            p->state = ST_FLOW_MAP_LOOP;
            return YAM_OK;
        }
        if (tt == YAM_TOK_FLOW_MAP_END) {
            p->state = ST_FLOW_MAP_LOOP; /* will close next iteration */
            return YAM_OK;
        }
        PARSE_ERROR(p, "expected ',' or '}' in flow mapping");
    }

    case ST_FLOW_MAP_END: {
        yam_mark close = p->current.start;
        yam_mark close_end = p->current.end;
        consume_token(p); /* consume } */
        evt = evt_simple(YAM_EVT_MAPPING_END);
        evt.start = close;
        evt.end = close_end;
        inc_emit(p, evt);
        state_frame frame = inc_pop_frame(p);
        p->state = frame.return_state;
        return YAM_OK;
    }

    /* ── Flow sequence states (incremental) ────────────────── */

    case ST_FLOW_SEQ_LOOP: {
        if (p->oom) return YAM_ERR_MEMORY;
        peek_token(p);
        tt = tok_type(p);

        if (tt == YAM_TOK_FLOW_SEQ_END) {
            p->state = ST_FLOW_SEQ_END;
            return YAM_OK;
        }

        /* explicit pair: ? key : value */
        if (tt == YAM_TOK_BLOCK_MAP_KEY) {
            p->state = ST_FLOW_SEQ_EXPLICIT_KEY;
            return YAM_OK;
        }

        /* scalar or alias: potential implicit pair key */
        if (tt == YAM_TOK_SCALAR || tt == YAM_TOK_ALIAS) {
            p->state = ST_FLOW_SEQ_ENTRY;
            return YAM_OK;
        }

        /* nested flow collection as entry — could be implicit pair key
         * ([{a}: val]) which we can't handle incrementally.  Scan ahead
         * to the matching close bracket: if ':' follows it's a pair key
         * and we fall back to eager; otherwise parse incrementally. */
        if (tt == YAM_TOK_FLOW_SEQ_START || tt == YAM_TOK_FLOW_MAP_START) {
            if (flow_is_block_key(p->input, p->input_len,
                                  p->current.start.offset)) {
                p->state = ST_EAGER_DRAIN;
            } else {
                p->node_return = ST_FLOW_SEQ_SEP;
                p->state = ST_FLOW_NODE;
            }
            return YAM_OK;
        }

        /* implicit pair with empty key: [ : value ] */
        if (tt == YAM_TOK_BLOCK_MAP_VALUE) {
            evt = evt_simple(YAM_EVT_MAPPING_START);
            evt.flow = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);

            /* emit empty key */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);

            consume_token(p); /* consume : */

            /* parse value */
            peek_token(p);
            tt = tok_type(p);
            if (tt == YAM_TOK_FLOW_ENTRY ||
                tt == YAM_TOK_FLOW_SEQ_END) {
                evt = evt_simple(YAM_EVT_SCALAR);
                evt.start = p->current.start;
                evt.end = p->current.start;
                inc_emit(p, evt);
                p->state = ST_FLOW_SEQ_IMPLICIT_END;
            } else {
                p->node_return = ST_FLOW_SEQ_IMPLICIT_END;
                p->state = ST_FLOW_NODE;
            }
            return YAM_OK;
        }

        /* TAG/ANCHOR: consume props, re-enter loop */
        if (tt == YAM_TOK_TAG || tt == YAM_TOK_ANCHOR) {
            yam_status st = consume_props(p);
            if (st != YAM_OK) return st;
            /* props consumed, stay in ST_FLOW_SEQ_LOOP to re-dispatch */
            return YAM_OK;
        }

        /* comma: empty entry */
        if (tt == YAM_TOK_FLOW_ENTRY) {
            consume_token(p);
            /* stay in LOOP — next iteration handles the entry */
            return YAM_OK;
        }

        PARSE_ERROR(p, "unexpected token in flow sequence");
    }

    case ST_FLOW_SEQ_ENTRY: {
        /* Parse scalar/alias, peek for ':' to detect implicit pair */
        peek_token(p);
        tt = tok_type(p);

        if (tt == YAM_TOK_SCALAR) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.value = p->current.value;
            evt.scalar_style = p->current.scalar_style;
            evt.start = p->current.start;
            evt.end = p->current.end;
        } else { /* ALIAS */
            evt = evt_simple(YAM_EVT_ALIAS);
            evt.value = p->current.value;
            evt.start = p->current.start;
            evt.end = p->current.end;
        }
        attach_props(p, &evt);
        consume_token(p);

        /* peek for ':' — implicit pair detection */
        peek_token(p);
        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
            p->saved_node = evt;
            p->have_saved_node = true;
            p->state = ST_FLOW_SEQ_ENTRY_KEY;
            return YAM_OK;
        }

        /* plain entry, not a pair */
        inc_emit(p, evt);
        p->state = ST_FLOW_SEQ_SEP;
        return YAM_OK;
    }

    case ST_FLOW_SEQ_ENTRY_KEY: {
        /* Implicit pair detected: emit MAPPING_START + saved key */
        evt = evt_simple(YAM_EVT_MAPPING_START);
        evt.flow = true;
        evt.start = p->saved_node.start;
        evt.end = p->saved_node.start;
        inc_emit(p, evt);
        inc_emit(p, p->saved_node);
        p->have_saved_node = false;

        /* consume ':' */
        consume_token(p);

        /* check for empty value */
        peek_token(p);
        tt = tok_type(p);
        if (tt == YAM_TOK_FLOW_ENTRY || tt == YAM_TOK_FLOW_SEQ_END) {
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_FLOW_SEQ_IMPLICIT_END;
        } else {
            p->node_return = ST_FLOW_SEQ_IMPLICIT_END;
            p->state = ST_FLOW_NODE;
        }
        return YAM_OK;
    }

    case ST_FLOW_SEQ_IMPLICIT_END: {
        evt = evt_simple(YAM_EVT_MAPPING_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, evt);
        p->state = ST_FLOW_SEQ_SEP;
        return YAM_OK;
    }

    case ST_FLOW_SEQ_EXPLICIT_KEY: {
        /* ? key : value pair in flow sequence */
        evt = evt_simple(YAM_EVT_MAPPING_START);
        evt.flow = true;
        evt.start = p->current.start;
        evt.end = p->current.end;
        inc_emit(p, evt);

        consume_token(p); /* consume ? */

        peek_token(p);
        tt = tok_type(p);
        if (tt == YAM_TOK_BLOCK_MAP_VALUE ||
            tt == YAM_TOK_FLOW_ENTRY ||
            tt == YAM_TOK_FLOW_SEQ_END) {
            /* empty key */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
            p->state = ST_FLOW_SEQ_EXPLICIT_VALUE;
        } else {
            p->node_return = ST_FLOW_SEQ_EXPLICIT_VALUE;
            p->state = ST_FLOW_NODE;
        }
        return YAM_OK;
    }

    case ST_FLOW_SEQ_EXPLICIT_VALUE: {
        peek_token(p);
        tt = tok_type(p);
        if (tt == YAM_TOK_BLOCK_MAP_VALUE) {
            consume_token(p);
            peek_token(p);
            tt = tok_type(p);
            if (tt == YAM_TOK_FLOW_ENTRY ||
                tt == YAM_TOK_FLOW_SEQ_END) {
                evt = evt_simple(YAM_EVT_SCALAR);
                evt.start = p->current.start;
                evt.end = p->current.start;
                inc_emit(p, evt);
            } else {
                p->node_return = ST_FLOW_SEQ_EXPLICIT_END;
                p->state = ST_FLOW_NODE;
                return YAM_OK;
            }
        } else {
            /* no value indicator, emit empty */
            evt = evt_simple(YAM_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, evt);
        }
        p->state = ST_FLOW_SEQ_EXPLICIT_END;
        return YAM_OK;
    }

    case ST_FLOW_SEQ_EXPLICIT_END: {
        evt = evt_simple(YAM_EVT_MAPPING_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, evt);
        p->state = ST_FLOW_SEQ_SEP;
        return YAM_OK;
    }

    case ST_FLOW_SEQ_CHECK_COLON: {
        /* After nested flow collection entry, check for ':' */
        peek_token(p);
        if (tok_type(p) == YAM_TOK_BLOCK_MAP_VALUE) {
            /* nested collection as implicit pair key — rare, fall back */
            p->state = ST_EAGER_DRAIN;
            return YAM_OK;
        }
        p->state = ST_FLOW_SEQ_SEP;
        return YAM_OK;
    }

    case ST_FLOW_SEQ_SEP: {
        peek_token(p);
        tt = tok_type(p);

        if (tt == YAM_TOK_FLOW_ENTRY) {
            consume_token(p);
            p->state = ST_FLOW_SEQ_LOOP;
            return YAM_OK;
        }
        if (tt == YAM_TOK_FLOW_SEQ_END) {
            p->state = ST_FLOW_SEQ_LOOP; /* will close next iteration */
            return YAM_OK;
        }
        PARSE_ERROR(p, "expected ',' or ']' in flow sequence");
    }

    case ST_FLOW_SEQ_END: {
        yam_mark close = p->current.start;
        yam_mark close_end = p->current.end;
        consume_token(p); /* consume ] */
        evt = evt_simple(YAM_EVT_SEQUENCE_END);
        evt.start = close;
        evt.end = close_end;
        inc_emit(p, evt);
        state_frame frame = inc_pop_frame(p);
        p->state = frame.return_state;
        return YAM_OK;
    }

    case ST_EAGER_DRAIN:
        /* Signal to yam_parse_next to fall back to eager mode */
        return YAM_OK;

    case ST_DONE:
        return YAM_OK;

    }

    return YAM_ERR_PARSE;
}

/* ── Public API ──────────────────────────────────────────── */

yam_parser *yam_parser_new(const char *input, size_t len, yam_arena *a) {
    yam_parser *p = calloc(1, sizeof(yam_parser));
    if (!p) return NULL;

    p->scanner = yam_scanner_new(input, len, a);
    if (!p->scanner) { free(p); return NULL; }

    p->arena = a;
    p->input = input;
    p->input_len = len;
    p->have_token = false;

    /* defer large allocation — incremental mode rarely needs events[];
     * eager fallback will grow via enqueue() / realloc as needed */
    p->evt_cap = 64;
    p->events = malloc(p->evt_cap * sizeof(yam_event));
    if (!p->events) { yam_scanner_free(p->scanner); free(p); return NULL; }
    p->evt_len = 0;
    p->evt_cursor = 0;

    p->ctx_cap = 16;
    p->contexts = malloc(p->ctx_cap * sizeof(ctx_entry));
    if (!p->contexts) { yam_scanner_free(p->scanner); free(p->events); free(p); return NULL; }
    p->ctx_len = 0;

    p->pending_anchor = YAM_STR_NULL;
    p->pending_tag = YAM_STR_NULL;
    p->has_anchor = false;
    p->has_tag = false;
    p->stream_started = false;
    p->stream_ended = false;
    p->doc_open = false;
    p->merge_enabled = false;
    p->resolve_enabled = false;
    p->max_events = 10000; /* safety limit */
    p->oom = false;

    /* incremental state machine — use eager mode when merge/alias needed */
    p->incremental = true;
    p->state = ST_STREAM_START;
    p->frame_cap = 16;
    p->frames = malloc(p->frame_cap * sizeof(state_frame));
    if (!p->frames) {
        yam_scanner_free(p->scanner);
        free(p->contexts); free(p->events); free(p);
        return NULL;
    }
    p->frame_len = 0;
    p->out_len = 0;
    p->out_cursor = 0;
    p->have_saved_node = false;

    return p;
}

yam_status yam_parse_next(yam_parser *p, yam_event *evt) {
    if (!p->incremental) {
        /* ── Eager path (merge/alias enabled, or fell back from incremental) ── */
        if (dequeue(p, evt)) return YAM_OK;
        if (p->stream_ended) {
            *evt = evt_simple(YAM_EVT_NONE);
            return YAM_OK;
        }
        if (!p->stream_started) {
            yam_status st = parse_stream(p);
            if (st != YAM_OK) return st;
            if (p->merge_enabled) {
                st = resolve_merges(p);
                if (st != YAM_OK) return st;
            }
            if (p->resolve_enabled) {
                st = resolve_aliases(p);
                if (st != YAM_OK) return st;
            }
            /* skip events already delivered during incremental phase */
            if (p->events_delivered > 0 && p->evt_cursor < p->events_delivered) {
                p->evt_cursor = p->events_delivered;
                p->events_delivered = 0;
            }
            if (dequeue(p, evt)) return YAM_OK;
        }
        *evt = evt_simple(YAM_EVT_NONE);
        return YAM_OK;
    }

    /* ── Incremental path (state machine) ── */

    /* drain buffered incremental events */
    if (p->out_cursor < p->out_len) {
        *evt = p->out_buf[p->out_cursor++];
        if (p->out_cursor >= p->out_len) {
            p->out_len = 0;
            p->out_cursor = 0;
        }
        p->events_delivered++;
        return YAM_OK;
    }

    if (p->state == ST_DONE) {
        *evt = evt_simple(YAM_EVT_NONE);
        return YAM_OK;
    }

    /* step the state machine until it produces output or finishes */
    p->out_len = 0;
    p->out_cursor = 0;
    while (p->out_len == 0) {
        yam_status st = parser_step(p);
        if (st != YAM_OK) return st;
        if (p->oom) {
            if (p->out_len > 0) break; /* drain valid events first */
            return YAM_ERR_MEMORY;
        }
        /* check for scanner error ignored by state machine */
        if (p->scan_error) return p->scan_error;

        if (p->state == ST_EAGER_DRAIN) {
            /* fall back to eager: re-parse from scratch */
            int skip = p->events_delivered;
            p->incremental = false;
            p->state = ST_DONE;
            p->stream_started = false;
            p->stream_ended = false;
            p->doc_open = false;
            p->have_token = false;
            p->evt_len = 0;
            p->evt_cursor = 0;
            p->ctx_len = 0;
            p->has_anchor = false;
            p->has_tag = false;
            p->pending_anchor = YAM_STR_NULL;
            p->pending_tag = YAM_STR_NULL;
            p->oom = false;
            p->events_delivered = 0;
            /* reset scanner to beginning */
            yam_scanner_free(p->scanner);
            p->scanner = yam_scanner_new(p->input, p->input_len, p->arena);
            if (!p->scanner) return YAM_ERR_MEMORY;
            /* eager parse_stream will be called on the recursive call;
             * skip events already delivered during incremental phase */
            p->events_delivered = skip;
            return yam_parse_next(p, evt);
        }

        if (p->state == ST_DONE && p->out_len == 0) {
            *evt = evt_simple(YAM_EVT_NONE);
            return YAM_OK;
        }
    }

    *evt = p->out_buf[0];
    p->out_cursor = 1;
    if (p->out_cursor >= p->out_len) {
        p->out_len = 0;
        p->out_cursor = 0;
    }
    p->events_delivered++;
    return YAM_OK;
}

void yam_parser_set_schema(yam_parser *p, const yam_schema *schema) {
    if (p) {
        p->schema = schema;
        if (schema) p->incremental = false;
    }
}

void yam_parser_set_merge(yam_parser *p, bool enable) {
    if (p) {
        p->merge_enabled = enable;
        if (enable) p->incremental = false;
    }
}

void yam_parser_set_resolve(yam_parser *p, bool enable) {
    if (p) {
        p->resolve_enabled = enable;
        if (enable) p->incremental = false;
    }
}

void yam_parser_set_max_events(yam_parser *p, int max) {
    if (p) p->max_events = max;
}

const char *yam_parser_error(yam_parser *p) {
    if (!p || p->error_msg[0] == '\0') return NULL;
    return p->error_msg;
}

yam_mark yam_parser_error_mark(yam_parser *p) {
    if (!p) return (yam_mark){0, 0, 0};
    return p->error_mark;
}

void yam_parser_free(yam_parser *p) {
    if (!p) return;
    yam_scanner_free(p->scanner);
    free(p->contexts);
    free(p->events);
    free(p->frames);
    free(p);
}
