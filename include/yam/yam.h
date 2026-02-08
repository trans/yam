/**
 * @file yam.h
 * @brief yam — YAML 1.2 parser/emitter
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
#define YAM_VERSION_MINOR 2
#define YAM_VERSION_PATCH 0

/* ── String view (zero-copy reference into source) ───────── */

/** Non-owning reference to a UTF-8 string. Points into the input buffer
 *  or arena-allocated memory; valid until the arena is freed/reset. */
typedef struct {
    const char *data;   /**< Pointer to string bytes (not NUL-terminated). */
    size_t      len;    /**< Length in bytes. */
} yam_str;

/** Null/empty string view. */
#define YAM_STR_NULL ((yam_str){NULL, 0})

/** Create a yam_str from a string literal. */
#define YAM_STR_LIT(s) ((yam_str){(s), sizeof(s) - 1})

/* ── Error codes ─────────────────────────────────────────── */

/** Status codes returned by parser, scanner, and emitter functions. */
typedef enum {
    YAM_OK = 0,         /**< Success. */
    YAM_ERR_MEMORY,     /**< Allocation failure. */
    YAM_ERR_INPUT,      /**< Invalid input (e.g. NULL pointer). */
    YAM_ERR_SCAN,       /**< Scanner error (malformed YAML). */
    YAM_ERR_PARSE,      /**< Parser error (structural YAML error). */
    YAM_ERR_EMIT,       /**< Emitter error (invalid event sequence). */
} yam_status;

/* ── Source location ─────────────────────────────────────── */

/** Byte-level position within the input buffer. */
typedef struct {
    size_t offset;  /**< Byte offset from start of input. */
    size_t line;    /**< 1-based line number. */
    size_t col;     /**< 1-based column in bytes. */
} yam_mark;

/* ── Token types (scanner output) ────────────────────────── */

/** Token types produced by the scanner. */
typedef enum {
    YAM_TOK_NONE = 0,

    /* structural */
    YAM_TOK_STREAM_START,
    YAM_TOK_STREAM_END,
    YAM_TOK_DOC_START,       /**< @c --- */
    YAM_TOK_DOC_END,         /**< @c ... */

    /* indicators */
    YAM_TOK_BLOCK_SEQ_ENTRY, /**< @c - */
    YAM_TOK_BLOCK_MAP_KEY,   /**< @c ? */
    YAM_TOK_BLOCK_MAP_VALUE, /**< @c : */
    YAM_TOK_FLOW_SEQ_START,  /**< @c [ */
    YAM_TOK_FLOW_SEQ_END,    /**< @c ] */
    YAM_TOK_FLOW_MAP_START,  /**< @c { */
    YAM_TOK_FLOW_MAP_END,    /**< @c } */
    YAM_TOK_FLOW_ENTRY,      /**< @c , */

    /* content */
    YAM_TOK_SCALAR,
    YAM_TOK_TAG,             /**< @c !tag or @c !!type */
    YAM_TOK_ANCHOR,          /**< @c &name */
    YAM_TOK_ALIAS,           /**< @c *name */
} yam_token_type;

/* ── Scalar style ────────────────────────────────────────── */

/** How a scalar was (or should be) represented in YAML text. */
typedef enum {
    YAM_SCALAR_PLAIN,          /**< Unquoted. */
    YAM_SCALAR_SINGLE_QUOTED,  /**< @c 'single' */
    YAM_SCALAR_DOUBLE_QUOTED,  /**< @c "double" */
    YAM_SCALAR_LITERAL,        /**< Block literal @c | */
    YAM_SCALAR_FOLDED,         /**< Block folded @c > */
} yam_scalar_style;

/* ── Token ───────────────────────────────────────────────── */

/** A single lexical token from the scanner. */
typedef struct {
    yam_token_type  type;
    yam_str         value;        /**< Scalar/tag/anchor text. */
    yam_scalar_style scalar_style; /**< Only meaningful for YAM_TOK_SCALAR. */
    yam_mark        start;        /**< Position of first byte. */
    yam_mark        end;          /**< Position past last byte. */
} yam_token;

/* ── Event types (parser output) ─────────────────────────── */

/** High-level event types produced by the parser.
 *  Events arrive in a well-formed sequence:
 *  STREAM_START (DOC_START node DOC_END)* STREAM_END,
 *  where @e node is a scalar, alias, or collection (mapping/sequence). */
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

/** A parsed YAML event. All string fields point into the arena and remain
 *  valid until the arena is freed or reset. */
typedef struct {
    yam_event_type   type;
    yam_str          value;       /**< Scalar value or alias name. */
    yam_str          anchor;      /**< Anchor name (@c &name) if present. */
    yam_str          tag;         /**< Tag (@c !tag) if present. */
    yam_scalar_style scalar_style;
    bool             implicit;    /**< True for implicit doc start/end. */
    bool             flow;        /**< True for flow collections ({} / []). */
    yam_mark         start;
    yam_mark         end;
} yam_event;

/* ── Arena allocator ─────────────────────────────────────── */

/** Opaque bump allocator. All memory allocated from an arena is freed in
 *  one call to yam_arena_free(). No per-object deallocation needed. */
typedef struct yam_arena yam_arena;

/** Create a new arena with the given initial block capacity (min 4096). */
yam_arena  *yam_arena_new(size_t initial_cap);

/** Allocate @p size bytes with @p align alignment from the arena. */
void       *yam_arena_alloc(yam_arena *a, size_t size, size_t align);

/** Duplicate @p len bytes from @p src into the arena (NUL-terminated). */
char       *yam_arena_dup(yam_arena *a, const char *src, size_t len);

/** Reset the arena for reuse, keeping the largest block allocated. */
void        yam_arena_reset(yam_arena *a);

/** Free the arena and all memory allocated from it. */
void        yam_arena_free(yam_arena *a);

/* ── File input ─────────────────────────────────────────── */

/** Read an entire file into the arena. Returns a yam_str with .data=NULL
 *  on failure. The buffer is not NUL-terminated. */
yam_str     yam_read_file(const char *path, yam_arena *a);

/* ── Scanner ─────────────────────────────────────────────── */

/** Opaque low-level tokenizer. Most users should use the parser instead. */
typedef struct yam_scanner yam_scanner;

/** Create a scanner over the given input buffer. The buffer must remain
 *  valid for the scanner's lifetime and is not copied. */
yam_scanner *yam_scanner_new(const char *input, size_t len, yam_arena *a);

/** Retrieve the next token. Returns YAM_OK on success, YAM_ERR_SCAN on
 *  malformed input, or YAM_ERR_MEMORY on allocation failure. */
yam_status   yam_scan_next(yam_scanner *s, yam_token *tok);

/** Error message from the last failed scan, or NULL. */
const char  *yam_scanner_error(yam_scanner *s);

/** Source location of the last scan error. */
yam_mark     yam_scanner_error_mark(yam_scanner *s);

/** Free the scanner (does not free the arena). */
void         yam_scanner_free(yam_scanner *s);

/* ── Tag constants ───────────────────────────────────────── */

extern const yam_str YAM_TAG_NULL;    /**< tag:yaml.org,2002:null  */
extern const yam_str YAM_TAG_BOOL;    /**< tag:yaml.org,2002:bool  */
extern const yam_str YAM_TAG_INT;     /**< tag:yaml.org,2002:int   */
extern const yam_str YAM_TAG_FLOAT;   /**< tag:yaml.org,2002:float */
extern const yam_str YAM_TAG_STR;     /**< tag:yaml.org,2002:str   */
extern const yam_str YAM_TAG_SEQ;     /**< tag:yaml.org,2002:seq   */
extern const yam_str YAM_TAG_MAP;     /**< tag:yaml.org,2002:map   */
extern const yam_str YAM_TAG_MERGE;   /**< tag:yaml.org,2002:merge */

/* ── Schema ──────────────────────────────────────────────── */

/** How a schema rule matches a plain scalar value. */
typedef enum {
    YAM_MATCH_EXACT,     /**< strcmp match. */
    YAM_MATCH_ICASE,     /**< Case-insensitive match. */
    YAM_MATCH_BUILTIN,   /**< Procedural matcher (int, float). */
} yam_match_type;

/** A single tag resolution rule: if a plain scalar matches @p pattern
 *  according to @p match, resolve it to @p tag. */
typedef struct {
    yam_match_type  match;
    const char     *pattern;   /**< String for EXACT/ICASE, name for BUILTIN. */
    yam_str         tag;       /**< Resolved tag. */
} yam_schema_rule;

/** Tag schema for resolving plain scalars to typed tags.
 *  Use one of the preset constructors or build a custom schema. */
typedef struct {
    const yam_schema_rule *rules;
    int                    rule_count;
    yam_str                default_plain_tag;   /**< Unmatched plain scalars. */
    yam_str                default_quoted_tag;   /**< All quoted scalars. */
    yam_str                default_seq_tag;      /**< Untagged sequences. */
    yam_str                default_map_tag;      /**< Untagged mappings. */
} yam_schema;

/** YAML 1.2 Failsafe schema: everything is !!str / !!seq / !!map. */
yam_schema yam_schema_failsafe(void);

/** YAML 1.2 JSON schema: null, true/false, integers, floats. */
yam_schema yam_schema_json(void);

/** YAML 1.2 Core schema: JSON + Null/NULL/~, True/TRUE, 0x/0o ints, etc. */
yam_schema yam_schema_core(void);

/** Resolve a scalar event's tag using the given schema. */
yam_str    yam_schema_resolve(const yam_schema *schema, const yam_event *evt);

/* ── Schema builder ──────────────────────────────────────── */

/** Opaque builder for constructing custom tag schemas. */
typedef struct yam_schema_builder yam_schema_builder;

/** Create a new schema builder (allocates from the arena). */
yam_schema_builder *yam_schema_builder_new(yam_arena *a);

/** Add a tag resolution rule. */
void    yam_schema_builder_add(yam_schema_builder *b,
                               yam_match_type match,
                               const char *pattern, yam_str tag);

/** Add boolean resolution rules (e.g. "true"/"yes" -> !!bool). */
void    yam_schema_builder_add_bools(yam_schema_builder *b,
                                     const char **true_terms, int ntrue,
                                     const char **false_terms, int nfalse);

/** Add null resolution rules (e.g. "null"/"~" -> !!null). */
void    yam_schema_builder_add_nulls(yam_schema_builder *b,
                                     const char **terms, int nterms);

/** Add the built-in integer matcher (decimal, hex, octal). */
void    yam_schema_builder_add_int(yam_schema_builder *b);

/** Add the built-in float matcher (decimal, .inf, .nan). */
void    yam_schema_builder_add_float(yam_schema_builder *b);

/** Finalize and return the schema. The builder can be freed after this. */
yam_schema yam_schema_builder_finish(yam_schema_builder *b);

/** Free the schema builder. */
void    yam_schema_builder_free(yam_schema_builder *b);

/* ── Parser ──────────────────────────────────────────────── */

/** Opaque event parser. Consumes tokens from the scanner and produces
 *  a well-formed stream of yam_event values. */
typedef struct yam_parser yam_parser;

/** Create a parser over the given input buffer. The buffer must remain
 *  valid for the parser's lifetime and is not copied.
 *  @return Parser instance, or NULL on allocation failure. */
yam_parser *yam_parser_new(const char *input, size_t len, yam_arena *a);

/** Retrieve the next event. The full stream is parsed eagerly on the
 *  first call; subsequent calls drain the event queue.
 *  @return YAM_OK on success, or an error status. Check with
 *          yam_parser_error() for a message on failure. */
yam_status  yam_parse_next(yam_parser *p, yam_event *evt);

/** Set a tag schema for automatic tag resolution on scalars. */
void        yam_parser_set_schema(yam_parser *p, const yam_schema *schema);

/** Enable/disable merge key (@c <<) expansion. Disabled by default. */
void        yam_parser_set_merge(yam_parser *p, bool enable);

/** Enable/disable alias resolution (inline expansion of @c *alias
 *  references). Disabled by default. Cyclic aliases are kept as
 *  YAM_EVT_ALIAS events. */
void        yam_parser_set_resolve(yam_parser *p, bool enable);

/** Set the maximum number of events before the parser stops with an error.
 *  Default is 10,000. Set to 0 to disable the limit.
 *  @see README "Event Limit" for sizing guidance. */
void        yam_parser_set_max_events(yam_parser *p, int max);

/** Error message from the last failed parse, or NULL. */
const char *yam_parser_error(yam_parser *p);

/** Source location of the last parse error. */
yam_mark    yam_parser_error_mark(yam_parser *p);

/** Free the parser (does not free the arena). */
void        yam_parser_free(yam_parser *p);

/* ── Emitter ─────────────────────────────────────────────── */

/** Opaque YAML emitter. Feed it events to produce YAML text. */
typedef struct yam_emitter yam_emitter;

/** Output style for the emitter. */
typedef enum {
    YAM_EMIT_BLOCK,      /**< Default block style (indented). */
    YAM_EMIT_FLOW,       /**< Flow style ({} / []). */
    YAM_EMIT_MINIMAL,    /**< Minimal whitespace. */
} yam_emit_style;

/** Emitter configuration. Use YAM_EMIT_OPTS_DEFAULT for sensible defaults. */
typedef struct {
    yam_emit_style style;
    int            indent;       /**< Spaces per indent level (default 2). */
} yam_emit_opts;

/** Default emitter options: block style, 2-space indent. */
#define YAM_EMIT_OPTS_DEFAULT ((yam_emit_opts){YAM_EMIT_BLOCK, 2})

/** Create an emitter with the given options. Output is written to an
 *  internal buffer retrievable with yam_emitter_output(). */
yam_emitter *yam_emitter_new(yam_emit_opts opts, yam_arena *a);

/** Feed one event to the emitter. Events must arrive in the same
 *  well-formed order as produced by the parser. */
yam_status   yam_emit(yam_emitter *e, const yam_event *evt);

/** Retrieve the emitter's output buffer. Valid until the arena is freed. */
yam_str      yam_emitter_output(yam_emitter *e);

/** Free the emitter (does not free the arena). */
void         yam_emitter_free(yam_emitter *e);

/* ── Convenience ─────────────────────────────────────────── */

/** Return a human-readable name for a status code. */
const char *yam_status_str(yam_status s);

/** Return a human-readable name for a token type. */
const char *yam_token_type_str(yam_token_type t);

/** Return a human-readable name for an event type. */
const char *yam_event_type_str(yam_event_type t);

#ifdef __cplusplus
}
#endif

#endif /* YAM_H */
