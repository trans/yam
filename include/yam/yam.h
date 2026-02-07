/*
 * yam — YAML 1.2 parser/emitter
 *
 * Zero-copy, SIMD-accelerated, arena-allocated.
 * Spec: https://yaml.org/spec/1.2.2/
 */

#ifndef YAM_H
#define YAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────── */

#define YAM_VERSION_MAJOR 0
#define YAM_VERSION_MINOR 1
#define YAM_VERSION_PATCH 0

/* ── String view (zero-copy reference into source) ───────── */

typedef struct {
    const char *data;
    size_t      len;
} yam_str;

#define YAM_STR_NULL ((yam_str){NULL, 0})
#define YAM_STR_LIT(s) ((yam_str){(s), sizeof(s) - 1})

/* ── Error codes ─────────────────────────────────────────── */

typedef enum {
    YAM_OK = 0,
    YAM_ERR_MEMORY,
    YAM_ERR_INPUT,
    YAM_ERR_SCAN,
    YAM_ERR_PARSE,
    YAM_ERR_EMIT,
} yam_status;

/* ── Source location ─────────────────────────────────────── */

typedef struct {
    size_t offset;  /* byte offset from start */
    size_t line;    /* 1-based */
    size_t col;     /* 1-based, in bytes */
} yam_mark;

/* ── Token types (scanner output) ────────────────────────── */

typedef enum {
    YAM_TOK_NONE = 0,

    /* structural */
    YAM_TOK_STREAM_START,
    YAM_TOK_STREAM_END,
    YAM_TOK_DOC_START,       /* --- */
    YAM_TOK_DOC_END,         /* ... */

    /* indicators */
    YAM_TOK_BLOCK_SEQ_ENTRY, /* - */
    YAM_TOK_BLOCK_MAP_KEY,   /* ? */
    YAM_TOK_BLOCK_MAP_VALUE, /* : */
    YAM_TOK_FLOW_SEQ_START,  /* [ */
    YAM_TOK_FLOW_SEQ_END,    /* ] */
    YAM_TOK_FLOW_MAP_START,  /* { */
    YAM_TOK_FLOW_MAP_END,    /* } */
    YAM_TOK_FLOW_ENTRY,      /* , */

    /* content */
    YAM_TOK_SCALAR,
    YAM_TOK_TAG,             /* !tag or !!type */
    YAM_TOK_ANCHOR,          /* &name */
    YAM_TOK_ALIAS,           /* *name */
} yam_token_type;

/* ── Scalar style ────────────────────────────────────────── */

typedef enum {
    YAM_SCALAR_PLAIN,
    YAM_SCALAR_SINGLE_QUOTED,
    YAM_SCALAR_DOUBLE_QUOTED,
    YAM_SCALAR_LITERAL,      /* | */
    YAM_SCALAR_FOLDED,       /* > */
} yam_scalar_style;

/* ── Token ───────────────────────────────────────────────── */

typedef struct {
    yam_token_type  type;
    yam_str         value;        /* scalar/tag/anchor text */
    yam_scalar_style scalar_style; /* only meaningful for YAM_TOK_SCALAR */
    yam_mark        start;
    yam_mark        end;
} yam_token;

/* ── Event types (parser output) ─────────────────────────── */

typedef enum {
    YAM_EVT_NONE = 0,
    YAM_EVT_STREAM_START,
    YAM_EVT_STREAM_END,
    YAM_EVT_DOC_START,
    YAM_EVT_DOC_END,
    YAM_EVT_MAPPING_START,
    YAM_EVT_MAPPING_END,
    YAM_EVT_SEQUENCE_START,
    YAM_EVT_SEQUENCE_END,
    YAM_EVT_SCALAR,
    YAM_EVT_ALIAS,
} yam_event_type;

/* ── Event ───────────────────────────────────────────────── */

typedef struct {
    yam_event_type   type;
    yam_str          value;       /* scalar value or alias name */
    yam_str          anchor;      /* anchor name if present */
    yam_str          tag;         /* tag if present */
    yam_scalar_style scalar_style;
    bool             implicit;    /* implicit doc start/end, implicit tag */
    bool             flow;        /* true for flow collections ({} / []) */
    yam_mark         start;
    yam_mark         end;
} yam_event;

/* ── Arena allocator ─────────────────────────────────────── */

typedef struct yam_arena yam_arena;

yam_arena  *yam_arena_new(size_t initial_cap);
void       *yam_arena_alloc(yam_arena *a, size_t size, size_t align);
char       *yam_arena_dup(yam_arena *a, const char *src, size_t len);
void        yam_arena_reset(yam_arena *a);
void        yam_arena_free(yam_arena *a);

/* ── Scanner ─────────────────────────────────────────────── */

typedef struct yam_scanner yam_scanner;

yam_scanner *yam_scanner_new(const char *input, size_t len, yam_arena *a);
yam_status   yam_scan_next(yam_scanner *s, yam_token *tok);
void         yam_scanner_free(yam_scanner *s);

/* ── Tag constants ───────────────────────────────────────── */

extern const yam_str YAM_TAG_NULL;    /* tag:yaml.org,2002:null  */
extern const yam_str YAM_TAG_BOOL;    /* tag:yaml.org,2002:bool  */
extern const yam_str YAM_TAG_INT;     /* tag:yaml.org,2002:int   */
extern const yam_str YAM_TAG_FLOAT;   /* tag:yaml.org,2002:float */
extern const yam_str YAM_TAG_STR;     /* tag:yaml.org,2002:str   */
extern const yam_str YAM_TAG_SEQ;     /* tag:yaml.org,2002:seq   */
extern const yam_str YAM_TAG_MAP;     /* tag:yaml.org,2002:map   */

/* ── Schema ──────────────────────────────────────────────── */

typedef enum {
    YAM_MATCH_EXACT,     /* strcmp match                     */
    YAM_MATCH_ICASE,     /* case-insensitive match           */
    YAM_MATCH_BUILTIN,   /* procedural matcher (int, float)  */
} yam_match_type;

typedef struct {
    yam_match_type  match;
    const char     *pattern;   /* string for EXACT/ICASE, name for BUILTIN */
    yam_str         tag;       /* resolved tag */
} yam_schema_rule;

typedef struct {
    const yam_schema_rule *rules;
    int                    rule_count;
    yam_str                default_plain_tag;  /* unmatched plain → str */
    yam_str                default_quoted_tag;  /* quoted always → str  */
    yam_str                default_seq_tag;     /* untagged seq → seq   */
    yam_str                default_map_tag;     /* untagged map → map   */
} yam_schema;

yam_schema yam_schema_failsafe(void);
yam_schema yam_schema_json(void);
yam_schema yam_schema_core(void);

yam_str    yam_schema_resolve(const yam_schema *schema, const yam_event *evt);

/* ── Schema builder ──────────────────────────────────────── */

typedef struct yam_schema_builder yam_schema_builder;

yam_schema_builder *yam_schema_builder_new(yam_arena *a);
void    yam_schema_builder_add(yam_schema_builder *b,
                               yam_match_type match,
                               const char *pattern, yam_str tag);
void    yam_schema_builder_add_bools(yam_schema_builder *b,
                                     const char **true_terms, int ntrue,
                                     const char **false_terms, int nfalse);
void    yam_schema_builder_add_nulls(yam_schema_builder *b,
                                     const char **terms, int nterms);
void    yam_schema_builder_add_int(yam_schema_builder *b);
void    yam_schema_builder_add_float(yam_schema_builder *b);
yam_schema yam_schema_builder_finish(yam_schema_builder *b);
void    yam_schema_builder_free(yam_schema_builder *b);

/* ── Parser ──────────────────────────────────────────────── */

typedef struct yam_parser yam_parser;

yam_parser *yam_parser_new(const char *input, size_t len, yam_arena *a);
yam_status  yam_parse_next(yam_parser *p, yam_event *evt);
void        yam_parser_set_schema(yam_parser *p, const yam_schema *schema);
void        yam_parser_free(yam_parser *p);

/* ── Emitter ─────────────────────────────────────────────── */

typedef struct yam_emitter yam_emitter;

typedef enum {
    YAM_EMIT_BLOCK,      /* default block style */
    YAM_EMIT_FLOW,       /* flow style */
    YAM_EMIT_MINIMAL,    /* minimal whitespace */
} yam_emit_style;

typedef struct {
    yam_emit_style style;
    int            indent;       /* spaces per indent level, default 2 */
    int            width;        /* line width hint, default 80 */
    bool           unicode;      /* allow non-ASCII unescaped */
} yam_emit_opts;

#define YAM_EMIT_OPTS_DEFAULT ((yam_emit_opts){YAM_EMIT_BLOCK, 2, 80, true})

yam_emitter *yam_emitter_new(yam_emit_opts opts, yam_arena *a);
yam_status   yam_emit(yam_emitter *e, const yam_event *evt);
yam_str      yam_emitter_output(yam_emitter *e);
void         yam_emitter_free(yam_emitter *e);

/* ── Convenience ─────────────────────────────────────────── */

const char *yam_status_str(yam_status s);
const char *yam_token_type_str(yam_token_type t);
const char *yam_event_type_str(yam_event_type t);

#ifdef __cplusplus
}
#endif

#endif /* YAM_H */
