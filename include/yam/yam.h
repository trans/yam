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

/* ── Symbol export ───────────────────────────────────────── */

/* The shared library is built with hidden visibility; only declarations
 * marked YAM_API are exported. */
#if defined(__GNUC__) || defined(__clang__)
#  define YAM_API __attribute__((visibility("default")))
#else
#  define YAM_API
#endif

/* ── ABI ─────────────────────────────────────────────────────
 * Structs whose fields are listed in this header (yam_token, yam_event)
 * are only ever allocated by the library and handed out by const pointer,
 * so fields can be appended in later versions without breaking compiled
 * callers. Configuration and schemas are opaque, set through functions.
 * yam_str and yam_mark are plain value types and will not change. */

/* ── Version ─────────────────────────────────────────────── */

#define YAM_VERSION_MAJOR 0
#define YAM_VERSION_MINOR 3
#define YAM_VERSION_PATCH 1

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
    YAM_ERR_LIMIT,      /**< A safety limit was exceeded (event count,
                             nesting depth, or alias expansion). */
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
    YAM_TOK_DIRECTIVE,       /**< @c %YAML / @c %TAG line; value is the whole line */
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

/** A single lexical token from the scanner. Tokens are owned by the
 *  scanner; see yam_scan_next(). */
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

/** A parsed YAML event. Events are owned by the parser; see
 *  yam_parse_next(). String fields point into the input buffer or the
 *  arena and remain valid until those are freed or reset. */
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
YAM_API yam_arena  *yam_arena_new(size_t initial_cap);

/** Allocate @p size bytes with @p align alignment from the arena.
 *  @p align must be a power of two (0 means 1). Returns NULL if it isn't,
 *  if the size is too large, or if memory runs out; the arena remains
 *  usable after a failed allocation. */
YAM_API void       *yam_arena_alloc(yam_arena *a, size_t size, size_t align);

/** Duplicate @p len bytes from @p src into the arena (NUL-terminated). */
YAM_API char       *yam_arena_dup(yam_arena *a, const char *src, size_t len);

/** Reset the arena for reuse, keeping the largest block allocated. */
YAM_API void        yam_arena_reset(yam_arena *a);

/** Free the arena and all memory allocated from it. */
YAM_API void        yam_arena_free(yam_arena *a);

/* ── File input ─────────────────────────────────────────── */

/** Read an entire file into the arena. Returns a yam_str with .data=NULL
 *  on failure. The buffer is not NUL-terminated. */
YAM_API yam_str     yam_read_file(const char *path, yam_arena *a);

/* ── Scanner ─────────────────────────────────────────────── */

/** Opaque low-level tokenizer. Most users should use the parser instead. */
typedef struct yam_scanner yam_scanner;

/** Create a scanner over the given input buffer. The buffer must remain
 *  valid for the scanner's lifetime and is not copied. */
YAM_API yam_scanner *yam_scanner_new(const char *input, size_t len, yam_arena *a);

/** Retrieve the next token. On YAM_OK, @p *tok points to a token owned by
 *  the scanner, valid until the next call or yam_scanner_free(); on error
 *  it is set to NULL. Returns YAM_OK, YAM_ERR_SCAN on malformed input, or
 *  YAM_ERR_MEMORY on allocation failure. */
YAM_API yam_status   yam_scan_next(yam_scanner *s, const yam_token **tok);

/** Error message from the last failed scan, or NULL. */
YAM_API const char  *yam_scanner_error(yam_scanner *s);

/** Source location of the last scan error. */
YAM_API yam_mark     yam_scanner_error_mark(yam_scanner *s);

/** Free the scanner (does not free the arena). */
YAM_API void         yam_scanner_free(yam_scanner *s);

/* ── Tag constants ───────────────────────────────────────── */

YAM_API extern const yam_str YAM_TAG_NULL;    /**< tag:yaml.org,2002:null  */
YAM_API extern const yam_str YAM_TAG_BOOL;    /**< tag:yaml.org,2002:bool  */
YAM_API extern const yam_str YAM_TAG_INT;     /**< tag:yaml.org,2002:int   */
YAM_API extern const yam_str YAM_TAG_FLOAT;   /**< tag:yaml.org,2002:float */
YAM_API extern const yam_str YAM_TAG_STR;     /**< tag:yaml.org,2002:str   */
YAM_API extern const yam_str YAM_TAG_SEQ;     /**< tag:yaml.org,2002:seq   */
YAM_API extern const yam_str YAM_TAG_MAP;     /**< tag:yaml.org,2002:map   */
YAM_API extern const yam_str YAM_TAG_MERGE;   /**< tag:yaml.org,2002:merge */

/* ── Schema ──────────────────────────────────────────────── */

/** How a schema rule matches a plain scalar value. */
typedef enum {
    YAM_MATCH_EXACT,     /**< strcmp match. */
    YAM_MATCH_ICASE,     /**< Case-insensitive match. */
    YAM_MATCH_BUILTIN,   /**< Procedural matcher (int, float). */
} yam_match_type;

/** Opaque tag schema for resolving plain scalars to typed tags. Use one
 *  of the presets or build a custom schema. */
typedef struct yam_schema yam_schema;

/** YAML 1.2 Failsafe schema: everything is !!str / !!seq / !!map.
 *  Presets are static and never need freeing. */
YAM_API const yam_schema *yam_schema_failsafe(void);

/** YAML 1.2 JSON schema: null, true/false, integers, floats. */
YAM_API const yam_schema *yam_schema_json(void);

/** YAML 1.2 Core schema: JSON + Null/NULL/~, True/TRUE, 0x/0o ints, etc. */
YAM_API const yam_schema *yam_schema_core(void);

/** Resolve the tag of a scalar with the given value and style: quoted
 *  scalars are strings, plain ones are matched against the schema's rules. */
YAM_API yam_str    yam_schema_resolve(const yam_schema *schema, yam_str value,
                                      yam_scalar_style style);

/* ── Schema builder ──────────────────────────────────────── */

/** Opaque builder for constructing custom tag schemas. */
typedef struct yam_schema_builder yam_schema_builder;

/** Create a new schema builder (allocates from the arena). */
YAM_API yam_schema_builder *yam_schema_builder_new(yam_arena *a);

/** Add a tag resolution rule. */
YAM_API void    yam_schema_builder_add(yam_schema_builder *b,
                               yam_match_type match,
                               const char *pattern, yam_str tag);

/** Add boolean resolution rules (e.g. "true"/"yes" -> !!bool). */
YAM_API void    yam_schema_builder_add_bools(yam_schema_builder *b,
                                     const char **true_terms, int ntrue,
                                     const char **false_terms, int nfalse);

/** Add null resolution rules (e.g. "null"/"~" -> !!null). */
YAM_API void    yam_schema_builder_add_nulls(yam_schema_builder *b,
                                     const char **terms, int nterms);

/** Add the built-in integer matcher (decimal, hex, octal). */
YAM_API void    yam_schema_builder_add_int(yam_schema_builder *b);

/** Add the built-in float matcher (decimal, .inf, .nan). */
YAM_API void    yam_schema_builder_add_float(yam_schema_builder *b);

/** Finalize and return the schema, allocated in the builder's arena (it
 *  lives until the arena is freed). The builder can be freed after this.
 *  Returns NULL on allocation failure. */
YAM_API const yam_schema *yam_schema_builder_finish(yam_schema_builder *b);

/** Free the schema builder. */
YAM_API void    yam_schema_builder_free(yam_schema_builder *b);

/* ── Parser ──────────────────────────────────────────────── */

/** Opaque event parser. Consumes tokens from the scanner and produces
 *  a well-formed stream of events. */
typedef struct yam_parser yam_parser;

/** Create a parser over the given input buffer. The buffer must remain
 *  valid for the parser's lifetime and is not copied.
 *  @return Parser instance, or NULL on allocation failure. */
YAM_API yam_parser *yam_parser_new(const char *input, size_t len, yam_arena *a);

/** Retrieve the next event. On YAM_OK, @p *evt points to an event owned
 *  by the parser, valid until the next call or yam_parser_free(); copy it
 *  (or the fields you need) to keep it longer. On error @p *evt is set to
 *  NULL. After YAM_EVT_STREAM_END every call returns a YAM_EVT_NONE event.
 *
 *  Events are produced incrementally as input is consumed, except when
 *  merge keys, alias resolution, a schema, directives, or node properties
 *  require looking at the whole stream first.
 *  @return YAM_OK on success, or an error status; yam_parser_error() and
 *          yam_parser_error_mark() describe the error. */
YAM_API yam_status  yam_parse_next(yam_parser *p, const yam_event **evt);

/** Set a tag schema for automatic tag resolution on scalars. The schema
 *  must outlive the parser. */
YAM_API void        yam_parser_set_schema(yam_parser *p, const yam_schema *schema);

/** Enable/disable merge key (@c <<) expansion. Disabled by default.
 *  A merge value must be a mapping, an alias to one, or a sequence of
 *  those; anything else is a YAM_ERR_PARSE. */
YAM_API void        yam_parser_set_merge(yam_parser *p, bool enable);

/** Enable/disable alias resolution (inline expansion of @c *alias
 *  references). Disabled by default. An alias refers to the most recent
 *  anchor of that name before it in the same document. Cyclic aliases,
 *  and aliases with no preceding anchor, are kept as YAM_EVT_ALIAS
 *  events. */
YAM_API void        yam_parser_set_resolve(yam_parser *p, bool enable);

/** Set the maximum number of events before the parser stops with an error.
 *  Default is 10,000. Set to 0 to disable the limit.
 *  Exceeding it returns YAM_ERR_LIMIT; alias/merge expansion is bounded
 *  by the same limit. @see README "Safety Limits" for sizing guidance. */
YAM_API void        yam_parser_set_max_events(yam_parser *p, int max);

/** Set the maximum nesting depth of collections. Exceeding it stops the
 *  parser with YAM_ERR_LIMIT. Default is 256. Set to 0 to disable the
 *  limit, but note that some inputs (tags, anchors, merge keys, alias
 *  resolution) are parsed recursively, using roughly 1 KB of stack per
 *  level, so very deep input can then overflow the stack. */
YAM_API void        yam_parser_set_max_depth(yam_parser *p, int max);

/** Error message from the last failed parse, or NULL. */
YAM_API const char *yam_parser_error(yam_parser *p);

/** Source location of the last parse error. */
YAM_API yam_mark    yam_parser_error_mark(yam_parser *p);

/** Free the parser (does not free the arena). */
YAM_API void        yam_parser_free(yam_parser *p);

/* ── Emitter ─────────────────────────────────────────────── */

/** Opaque YAML emitter. Feed it events to produce YAML text. */
typedef struct yam_emitter yam_emitter;

/** Output style for the emitter. */
typedef enum {
    YAM_EMIT_BLOCK,      /**< Default block style (indented). */
    YAM_EMIT_FLOW,       /**< Flow style ({} / []). */
    YAM_EMIT_MINIMAL,    /**< Minimal whitespace. */
} yam_emit_style;

/** Create an emitter: block style, 2-space indent. Output is written to
 *  an internal buffer retrievable with yam_emitter_output().
 *  @return Emitter instance, or NULL on allocation failure. */
YAM_API yam_emitter *yam_emitter_new(yam_arena *a);

/** Set the output style (default YAM_EMIT_BLOCK). */
YAM_API void         yam_emitter_set_style(yam_emitter *e, yam_emit_style style);

/** Set the spaces per indentation level, 1-10 (default 2). */
YAM_API void         yam_emitter_set_indent(yam_emitter *e, int indent);

/* Events are fed in the same well-formed order the parser produces:
 * STREAM_START (DOC_START node DOC_END)* STREAM_END. Anchor and tag
 * arguments may be YAM_STR_NULL. Tags are full tags (e.g.
 * "tag:yaml.org,2002:str", written as !!str) or local tags ("!foo"). */

/** Re-emit an event obtained from yam_parse_next() (e.g. to reformat a
 *  document). To build output yourself, use the functions below. */
YAM_API yam_status   yam_emit(yam_emitter *e, const yam_event *evt);

YAM_API yam_status   yam_emit_stream_start(yam_emitter *e);
YAM_API yam_status   yam_emit_stream_end(yam_emitter *e);

/** Start a document; @p implicit omits the "---" marker. */
YAM_API yam_status   yam_emit_document_start(yam_emitter *e, bool implicit);

/** End a document; @p implicit omits the "..." marker. */
YAM_API yam_status   yam_emit_document_end(yam_emitter *e, bool implicit);

/** Emit a scalar. YAM_SCALAR_PLAIN writes the value plain when that reads
 *  back as the same text, and quotes it otherwise; the other styles are
 *  honored where valid (block styles fall back to double-quoted in flow
 *  context). */
YAM_API yam_status   yam_emit_scalar(yam_emitter *e, yam_str value,
                                     yam_scalar_style style,
                                     yam_str anchor, yam_str tag);

/** Emit an alias (@c *name). */
YAM_API yam_status   yam_emit_alias(yam_emitter *e, yam_str name);

/** Start a mapping; @p flow forces flow style ({...}) for it. */
YAM_API yam_status   yam_emit_mapping_start(yam_emitter *e, yam_str anchor,
                                            yam_str tag, bool flow);
YAM_API yam_status   yam_emit_mapping_end(yam_emitter *e);

/** Start a sequence; @p flow forces flow style ([...]) for it. */
YAM_API yam_status   yam_emit_sequence_start(yam_emitter *e, yam_str anchor,
                                             yam_str tag, bool flow);
YAM_API yam_status   yam_emit_sequence_end(yam_emitter *e);

/** Retrieve the emitter's output buffer. Valid until the arena is freed. */
YAM_API yam_str      yam_emitter_output(yam_emitter *e);

/** Free the emitter (does not free the arena). */
YAM_API void         yam_emitter_free(yam_emitter *e);

/* ── Convenience ─────────────────────────────────────────── */

/** Return a human-readable name for a status code. */
YAM_API const char *yam_status_str(yam_status s);

/** Return a human-readable name for a token type. */
YAM_API const char *yam_token_type_str(yam_token_type t);

/** Return a human-readable name for an event type. */
YAM_API const char *yam_event_type_str(yam_event_type t);

#ifdef __cplusplus
}
#endif

#endif /* YAM_H */
