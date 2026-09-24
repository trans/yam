# yam API Reference

Header: `#include "yam/yam.h"`

All types, constants, and functions are prefixed with `yam_` or `YAM_`.
Memory is managed through arenas -- allocate an arena, pass it to
constructors, and free it when done. No per-object cleanup needed except
for the parser, scanner, and emitter handles themselves.

**ABI stability.** Tokens and events are allocated only by the library and
read through `const` pointers; schemas and emitter options are opaque and
set through functions. New fields and options can therefore be added
without breaking compiled programs. `yam_str` and `yam_mark` are plain
value types that will not change. Only the functions declared in `yam.h`
are exported from the shared library.

---

## Core Types

### yam_str

```c
typedef struct {
    const char *data;
    size_t      len;
} yam_str;
```

Non-owning string view. Points into the input buffer or arena memory.
Not NUL-terminated. Valid until the arena is freed or reset.

| Macro | Description |
|-------|-------------|
| `YAM_STR_NULL` | Empty string view (`{NULL, 0}`). |
| `YAM_STR_LIT("text")` | Create from a string literal. |

### yam_status

```c
YAM_OK           // success
YAM_ERR_MEMORY   // allocation failure
YAM_ERR_INPUT    // invalid input (NULL pointer, etc.)
YAM_ERR_SCAN     // malformed YAML
YAM_ERR_PARSE    // structural YAML error
YAM_ERR_EMIT     // invalid event sequence
YAM_ERR_LIMIT    // a safety limit was exceeded (events, depth, alias expansion)
```

Returned by parser, scanner, and emitter functions. Use
`yam_status_str()` for a human-readable name.

### yam_mark

```c
typedef struct {
    size_t offset;  // byte offset from start of input
    size_t line;    // 1-based
    size_t col;     // 1-based, in bytes
} yam_mark;
```

### yam_event

Read-only; events are owned by the parser (see `yam_parse_next`).

```c
typedef struct {
    yam_event_type   type;
    yam_str          value;        // scalar text or alias name
    yam_str          anchor;       // &anchor if present
    yam_str          tag;          // !tag if present
    yam_scalar_style scalar_style;
    bool             implicit;     // implicit doc start/end
    bool             flow;         // true for {} / []
    yam_mark         start;
    yam_mark         end;
} yam_event;
```

Event types arrive in a well-formed stream:

```
STREAM_START
  (DOC_START node DOC_END)*
STREAM_END
```

Where *node* is one of:
- `SCALAR`
- `ALIAS`
- `MAPPING_START (key-node value-node)* MAPPING_END`
- `SEQUENCE_START node* SEQUENCE_END`

---

## Arena

The arena is a bump allocator. All memory from a parse session is freed
in one call.

| Function | Description |
|----------|-------------|
| `yam_arena *yam_arena_new(size_t cap)` | Create arena (min 4096 bytes). |
| `void *yam_arena_alloc(arena, size, align)` | Allocate raw bytes. |
| `char *yam_arena_dup(arena, src, len)` | Copy string into arena (NUL-terminated). |
| `void yam_arena_reset(arena)` | Reset for reuse (keeps largest block). |
| `void yam_arena_free(arena)` | Free arena and all allocations. |

---

## File Input

```c
yam_str yam_read_file(const char *path, yam_arena *a);
```

Reads an entire file into the arena. Returns `.data = NULL` on failure.

---

## Parser

The parser is the primary API. It consumes YAML text and produces events.

### Lifecycle

```c
yam_arena  *a = yam_arena_new(4096);
yam_parser *p = yam_parser_new(input, len, a);

// configure before first yam_parse_next() call
yam_parser_set_schema(p, yam_schema_core()); // optional: tag resolution
yam_parser_set_merge(p, true);        // optional: expand << merge keys
yam_parser_set_resolve(p, true);      // optional: inline *alias expansion
yam_parser_set_max_events(p, 50000);  // optional: raise event limit

const yam_event *evt;
while (yam_parse_next(p, &evt) == YAM_OK) {
    if (evt->type == YAM_EVT_STREAM_END) break;
    // process evt...
}

yam_parser_free(p);
yam_arena_free(a);
```

`yam_parse_next` sets `evt` to an event owned by the parser, valid until the
next call or `yam_parser_free`; on error it is set to NULL. The event's
strings point into the input or the arena and outlive the event. After
`STREAM_END`, further calls return `YAM_EVT_NONE` events.

Events are produced incrementally as the input is consumed, except when
merge keys, alias resolution, a schema, directives, or node properties
require seeing the whole stream first.

### Functions

| Function | Description |
|----------|-------------|
| `yam_parser *yam_parser_new(input, len, arena)` | Create parser. Input buffer must outlive parser. Returns NULL on alloc failure. |
| `yam_status yam_parse_next(p, const yam_event **evt)` | Get next event (owned by the parser). |
| `void yam_parser_set_schema(p, schema)` | Set tag schema for auto-resolution. The schema must outlive the parser. |
| `void yam_parser_set_merge(p, bool)` | Enable `<<` merge key expansion. The value must be a mapping, an alias to one, or a sequence of those. |
| `void yam_parser_set_resolve(p, bool)` | Enable `*alias` inline expansion. An alias refers to the most recent preceding anchor in the same document; cyclic and unresolvable aliases are kept as ALIAS events. |
| `void yam_parser_set_max_events(p, max)` | Set event limit (default 10,000; 0 = unlimited). |
| `void yam_parser_set_max_depth(p, max)` | Set nesting depth limit (default 256; 0 = unlimited). |
| `const char *yam_parser_error(p)` | Error message, or NULL. |
| `yam_mark yam_parser_error_mark(p)` | Error source location. |
| `void yam_parser_free(p)` | Free parser (not the arena). |

### Safety Limits

The default limit of 10,000 events handles roughly 3,000-5,000 YAML
nodes (~100-200KB of dense YAML). Raise or disable it for larger files:

```c
yam_parser_set_max_events(p, 100000);  // large files
yam_parser_set_max_events(p, 0);       // no limit
```

Nesting is limited to 256 levels by default (`yam_parser_set_max_depth`),
including nesting created by alias/merge expansion, and expansion is
bounded by the event limit. Exceeding any
limit returns `YAM_ERR_LIMIT`.

---

## Emitter

The emitter converts events back to YAML text.

### Lifecycle

```c
yam_emitter *e = yam_emitter_new(arena);     // block style, 2-space indent
yam_emitter_set_style(e, YAM_EMIT_FLOW);     // optional
yam_emitter_set_indent(e, 4);                // optional, 1-10

// re-emit parser events...
yam_emit(e, evt);

// ...or build the stream yourself
yam_emit_stream_start(e);
yam_emit_document_start(e, true);
yam_emit_scalar(e, YAM_STR_LIT("hello"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
yam_emit_document_end(e, true);
yam_emit_stream_end(e);

yam_str output = yam_emitter_output(e);
printf("%.*s", (int)output.len, output.data);

yam_emitter_free(e);
```

Events must arrive in the order the parser produces them. Anchor and tag
arguments may be `YAM_STR_NULL`; tags are full tags (`tag:yaml.org,2002:str`,
written as `!!str`) or local tags (`!foo`).

### Styles

| Style | Description |
|-------|-------------|
| `YAM_EMIT_BLOCK` | Default indented block style. |
| `YAM_EMIT_FLOW` | Flow style (`{}` / `[]`). |
| `YAM_EMIT_MINIMAL` | Minimal whitespace. |

### Functions

| Function | Description |
|----------|-------------|
| `yam_emitter *yam_emitter_new(arena)` | Create emitter (block style, 2-space indent). |
| `void yam_emitter_set_style(e, style)` | Set output style. |
| `void yam_emitter_set_indent(e, n)` | Set spaces per indent level (1-10). |
| `yam_status yam_emit(e, evt)` | Re-emit an event from `yam_parse_next`. |
| `yam_emit_stream_start(e)` / `yam_emit_stream_end(e)` | Stream boundaries. |
| `yam_emit_document_start(e, implicit)` / `yam_emit_document_end(e, implicit)` | Document boundaries; `implicit` omits `---` / `...`. |
| `yam_emit_scalar(e, value, style, anchor, tag)` | Scalar. Plain values are quoted only when needed to read back the same. |
| `yam_emit_alias(e, name)` | Alias (`*name`). |
| `yam_emit_mapping_start(e, anchor, tag, flow)` / `yam_emit_mapping_end(e)` | Mapping; `flow` forces `{...}`. |
| `yam_emit_sequence_start(e, anchor, tag, flow)` / `yam_emit_sequence_end(e)` | Sequence; `flow` forces `[...]`. |
| `yam_str yam_emitter_output(e)` | Get output buffer. |
| `void yam_emitter_free(e)` | Free emitter (not the arena). |

---

## Tag Schemas

Schemas resolve plain scalars to typed tags per YAML 1.2 Chapter 10.
Schema is opt-in -- without one, scalars have no tag. `yam_schema` is
opaque; presets are static, built schemas live in the builder's arena.

```c
yam_str tag = yam_schema_resolve(yam_schema_core(), YAM_STR_LIT("42"),
                                 YAM_SCALAR_PLAIN);   // tag:yaml.org,2002:int
```

### Presets

| Function | Resolves |
|----------|----------|
| `yam_schema_failsafe()` | Everything is `!!str` / `!!seq` / `!!map`. |
| `yam_schema_json()` | `null`, `true`/`false`, integers, floats. |
| `yam_schema_core()` | JSON + `Null`/`NULL`/`~`, `True`/`TRUE`, `0x`/`0o` ints. |

### Tag Constants

```c
YAM_TAG_NULL    // "tag:yaml.org,2002:null"
YAM_TAG_BOOL    // "tag:yaml.org,2002:bool"
YAM_TAG_INT     // "tag:yaml.org,2002:int"
YAM_TAG_FLOAT   // "tag:yaml.org,2002:float"
YAM_TAG_STR     // "tag:yaml.org,2002:str"
YAM_TAG_SEQ     // "tag:yaml.org,2002:seq"
YAM_TAG_MAP     // "tag:yaml.org,2002:map"
YAM_TAG_MERGE   // "tag:yaml.org,2002:merge"
```

### Custom Schemas

```c
yam_schema_builder *b = yam_schema_builder_new(arena);

yam_schema_builder_add_nulls(b, (const char*[]){"null", "~"}, 2);
yam_schema_builder_add_bools(b,
    (const char*[]){"true", "yes"}, 2,
    (const char*[]){"false", "no"}, 2);
yam_schema_builder_add_int(b);
yam_schema_builder_add_float(b);

const yam_schema *schema = yam_schema_builder_finish(b);  // lives in the arena
yam_schema_builder_free(b);

yam_parser_set_schema(parser, schema);
```

| Function | Description |
|----------|-------------|
| `yam_schema_builder_new(arena)` | Create builder. |
| `yam_schema_builder_add(b, match, pattern, tag)` | Add a resolution rule. |
| `yam_schema_builder_add_bools(b, true_terms, n, false_terms, n)` | Add boolean rules. |
| `yam_schema_builder_add_nulls(b, terms, n)` | Add null rules. |
| `yam_schema_builder_add_int(b)` | Add built-in integer matcher. |
| `yam_schema_builder_add_float(b)` | Add built-in float matcher. |
| `yam_schema_builder_finish(b)` | Finalize schema (copied into the arena; NULL on allocation failure). |
| `yam_schema_builder_free(b)` | Free builder. |

---

## Scanner

Low-level tokenizer. Most users should use the parser instead.

| Function | Description |
|----------|-------------|
| `yam_scanner *yam_scanner_new(input, len, arena)` | Create scanner. |
| `yam_status yam_scan_next(s, const yam_token **tok)` | Get next token (owned by the scanner, valid until the next call; NULL on error). |
| `const char *yam_scanner_error(s)` | Error message, or NULL. |
| `yam_mark yam_scanner_error_mark(s)` | Error location. |
| `void yam_scanner_free(s)` | Free scanner. |

### Token Types

| Token | YAML |
|-------|------|
| `YAM_TOK_STREAM_START` / `_END` | Stream boundaries. |
| `YAM_TOK_DOC_START` / `_END` | `---` / `...` |
| `YAM_TOK_BLOCK_SEQ_ENTRY` | `-` |
| `YAM_TOK_BLOCK_MAP_KEY` | `?` |
| `YAM_TOK_BLOCK_MAP_VALUE` | `:` |
| `YAM_TOK_FLOW_SEQ_START` / `_END` | `[` / `]` |
| `YAM_TOK_FLOW_MAP_START` / `_END` | `{` / `}` |
| `YAM_TOK_FLOW_ENTRY` | `,` |
| `YAM_TOK_SCALAR` | Scalar value (any style). |
| `YAM_TOK_TAG` | `!tag` or `!!type` |
| `YAM_TOK_ANCHOR` | `&name` |
| `YAM_TOK_ALIAS` | `*name` |
| `YAM_TOK_DIRECTIVE` | `%YAML` / `%TAG` line (value is the whole line) |

---

## Convenience

```c
const char *yam_status_str(yam_status s);
const char *yam_token_type_str(yam_token_type t);
const char *yam_event_type_str(yam_event_type t);
```

Return static strings like `"ok"`, `"SCALAR"`, `"MAPPING_START"`, etc.
