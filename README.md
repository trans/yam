# yam

A YAML 1.2 parser and emitter written in C11. Fast, minimal, zero-copy.

Features a SIMD-accelerated scanner (SSE4.2 with scalar fallback),
an event-based parser, an emitter with block/flow/minimal output styles,
merge key expansion, alias resolution, file input, structured error messages,
and an arena allocator. Against the
[YAML Test Suite](https://github.com/yaml/yaml-test-suite), it produces the
exact expected event stream for all 302 valid-YAML cases (6 more lack
expected output in the suite itself), and rejects all 94 invalid-YAML
cases. Every case is run through both parser modes.

One deliberate leniency: a line inside a flow collection or multi-line
quoted scalar that starts with the closing bracket or quote may sit at any
indentation, so the common style

```yaml
key: [
  a,
  b
]
```

parses, although strict YAML 1.2 would reject the unindented `]`.

## Build

```
make              # build libyam.a
make test         # run scanner unit tests
make test-schema  # run schema/tag resolution tests
make test-emitter # run emitter tests
make test-merge   # run merge key tests
make test-resolve # run alias resolution tests
make test-errors  # run error handling tests
make test-suite   # run YAML Test Suite (requires git submodules)
make test-all     # run all tests
```

Requires a C11 compiler. Tested with GCC and Clang on Linux and macOS.

## Quick Start

### Parser (event API)

```c
#include "yam/yam.h"

const char *yaml = "greeting: hello\nitems:\n  - one\n  - two\n";

yam_arena  *arena  = yam_arena_new(4096);
yam_parser *parser = yam_parser_new(yaml, strlen(yaml), arena);
const yam_event *evt;

while (yam_parse_next(parser, &evt) == YAM_OK) {
    if (evt->type == YAM_EVT_STREAM_END) break;

    printf("%s", yam_event_type_str(evt->type));
    if (evt->anchor.data) printf(" &%.*s", (int)evt->anchor.len, evt->anchor.data);
    if (evt->tag.data)    printf(" <%.*s>", (int)evt->tag.len, evt->tag.data);
    if (evt->type == YAM_EVT_SCALAR)
        printf(" %.*s", (int)evt->value.len, evt->value.data);
    printf("\n");
}

yam_parser_free(parser);
yam_arena_free(arena);
```

`yam_parse_next` hands back a pointer to an event owned by the parser,
valid until the next call. Its string fields (`value`, `anchor`, `tag`)
point into your input or the arena and stay valid until those are freed,
so they can be kept without copying.

### File Input

```c
yam_arena *arena = yam_arena_new(4096);
yam_str    data  = yam_read_file("config.yaml", arena);

if (data.data) {
    yam_parser *p = yam_parser_new(data.data, data.len, arena);
    /* ... parse ... */
    yam_parser_free(p);
}

yam_arena_free(arena);  /* frees file data too */
```

### Emitter (roundtrip)

```c
yam_arena   *arena = yam_arena_new(4096);
const char  *yaml  = "greeting: hello\nitems:\n  - one\n  - two\n";

/* parse */
yam_parser *p = yam_parser_new(yaml, strlen(yaml), arena);

/* emit */
yam_emitter *e = yam_emitter_new(arena);
const yam_event *evt;
while (yam_parse_next(p, &evt) == YAM_OK) {
    yam_emit(e, evt);
    if (evt->type == YAM_EVT_STREAM_END) break;
}

yam_str out = yam_emitter_output(e);
fwrite(out.data, 1, out.len, stdout);

yam_emitter_free(e);
yam_parser_free(p);
yam_arena_free(arena);
```

### Emitter (building output)

```c
yam_emitter *e = yam_emitter_new(arena);

yam_emit_stream_start(e);
yam_emit_document_start(e, true);                       /* implicit: no "---" */
yam_emit_mapping_start(e, YAM_STR_NULL, YAM_STR_NULL, false);
yam_emit_scalar(e, YAM_STR_LIT("name"), YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
yam_emit_scalar(e, YAM_STR_LIT("yam"),  YAM_SCALAR_PLAIN, YAM_STR_NULL, YAM_STR_NULL);
yam_emit_mapping_end(e);
yam_emit_document_end(e, true);
yam_emit_stream_end(e);

yam_str out = yam_emitter_output(e);   /* "name: yam\n" */
```

### Scanner (token API)

For lower-level access, the scanner produces a flat token stream without
synthetic block structure tokens:

```c
yam_scanner *scanner = yam_scanner_new(yaml, len, arena);
const yam_token *tok;

while (yam_scan_next(scanner, &tok) == YAM_OK) {
    if (tok->type == YAM_TOK_STREAM_END) break;
    printf("%-20s %.*s\n",
           yam_token_type_str(tok->type),
           (int)tok->value.len, tok->value.data);
}

yam_scanner_free(scanner);
```

## API Overview

| Type | Description |
|------|-------------|
| `yam_str` | Non-owning string view (`data`, `len`) |
| `yam_mark` | Source position (`offset`, `line`, `col`) |
| `yam_token` | Scanner output (`type`, `value`, `scalar_style`, `start`, `end`), owned by the scanner |
| `yam_event` | Parser output (`type`, `value`, `anchor`, `tag`, `scalar_style`, `flow`), owned by the parser |
| `yam_schema` | Opaque tag resolution schema (failsafe, JSON, core, or custom) |
| `yam_emitter` | Event-to-YAML emitter, configured with setters |

Tokens and events are only ever allocated by the library and read through
`const` pointers, and configuration goes through functions, so new fields
and options can be added without breaking compiled programs.

### Event Types

| Event | Meaning |
|-------|---------|
| `STREAM_START` / `STREAM_END` | Document stream boundaries |
| `DOC_START` / `DOC_END` | Document boundaries (`---` / `...`) |
| `MAPPING_START` / `MAPPING_END` | Mapping (key-value pairs) |
| `SEQUENCE_START` / `SEQUENCE_END` | Sequence (list) |
| `SCALAR` | Scalar value |
| `ALIAS` | Alias reference (`*name`) |

## Emitter

The emitter converts an event stream back into YAML text that parses back
to the same data: every valid YAML Test Suite case round-trips (parse,
emit, parse) with identical events in all three styles, and the fuzzer
checks the same property on arbitrary input. Block scalars are written as
exact literals, collections used as keys take the explicit `? key` form,
and tags are written in a form that reads back unchanged. Three output
styles are available:

| Style | Description |
|-------|-------------|
| `YAM_EMIT_BLOCK` | Indented block style (default) |
| `YAM_EMIT_FLOW` | Flow style (`{key: value}`, `[a, b]`) |
| `YAM_EMIT_MINIMAL` | Compact flow with minimal whitespace |

The default is block style with a 2-space indent. Change it with setters:

```c
yam_emitter *e = yam_emitter_new(arena);
yam_emitter_set_style(e, YAM_EMIT_FLOW);
yam_emitter_set_indent(e, 4);   /* spaces per indent level, 1-10 */
```

Plain scalars are written plain whenever that reads back as the same text,
so `8080`, `true` and `null` keep their meaning; text that would be misread
(`: ` or ` #` inside, leading indicators, line breaks, ...) is quoted. A
scalar tagged `!!str` whose text looks like a number or keyword is quoted to
stay a string.

## Merge Keys

Enable merge key expansion (`<<`) to inherit keys from anchored mappings:

```c
yam_parser_set_merge(parser, true);
```

```yaml
defaults: &defaults
  adapter: postgres
  host: localhost

production:
  <<: *defaults
  database: mydb
```

With merge enabled, `production` expands to contain `adapter`, `host`, and
`database` as direct entries. Explicit keys override merged ones. The merge
value can be an alias to a mapping, an inline mapping (`<<: {k: v}`), or a
sequence of those, which merges all of them (first wins on conflicts). Any
other value, or an alias to an undefined anchor, is a parse error. Quoted
`"<<"` is treated as a normal key.

## Alias Resolution

Enable alias resolution to expand `*alias` references inline instead of
emitting `ALIAS` events:

```c
yam_parser_set_resolve(parser, true);
```

Scalar, mapping, and sequence aliases are all expanded. An alias refers to
the most recent anchor of that name *before* it in the same document, so a
reused anchor name works as in the spec. Circular references, and aliases
with no preceding anchor (undefined, forward, or from an earlier document),
are kept as `ALIAS` events (no error, no infinite loop). Combines with merge
keys when both are enabled.

## Safety Limits

The parser enforces limits against hostile or runaway input. Exceeding any
of them stops parsing with `YAM_ERR_LIMIT` and an error message.

**Event count.** Defaults to 10,000 events. This is sufficient for typical
config files (roughly 100-200KB of dense YAML), but large documents may
need a higher limit:

```c
yam_parser_set_max_events(parser, 100000);  /* raise for large files */
yam_parser_set_max_events(parser, 0);       /* disable limit entirely */
```

Each YAML node produces 1-3 events (a key-value pair is ~2 events, plus
structure start/end events), so the default 10,000 events handles roughly
3,000-5,000 nodes. Documents with large string values use fewer events per
byte and can go well beyond 200KB at the default limit.

**Nesting depth.** Defaults to 256 levels of nested collections. It applies to
the events delivered, so it also holds when alias or merge expansion nests
deeper than the input does:

```c
yam_parser_set_max_depth(parser, 1000);
```

Setting it to 0 disables the limit, but inputs using tags, anchors, merge
keys, or alias resolution are parsed recursively (roughly 1 KB of stack per
level), so very deep input can then overflow the stack.

**Alias and merge expansion.** Expanding aliases can grow a small document
exponentially (the "billion laughs" attack). The expanded stream must fit
within the event limit; if that is disabled, expansion is capped at 16
times the unexpanded stream plus 100,000 events.

## Error Handling

Both the scanner and parser provide error messages with source locations:

```c
yam_parser *p = yam_parser_new(yaml, strlen(yaml), arena);
const yam_event *evt;

if (yam_parse_next(p, &evt) != YAM_OK) {
    const char *msg  = yam_parser_error(p);
    yam_mark    mark = yam_parser_error_mark(p);
    fprintf(stderr, "error: line %zu col %zu: %s\n",
            mark.line, mark.col, msg);
}
```

Scanner errors are available directly or propagate through the parser:

```c
const char *msg  = yam_scanner_error(scanner);
yam_mark    mark = yam_scanner_error_mark(scanner);
```

## Tag Schemas

yam supports pluggable tag resolution per YAML 1.2 Chapter 10. Three
built-in schemas ship as presets:

| Schema | Resolves |
|--------|----------|
| **Failsafe** | Everything is `!!str` / `!!seq` / `!!map` |
| **JSON** | `null`, `true`/`false`, integers, floats |
| **Core** | JSON + `Null`/`NULL`/`~`, `True`/`TRUE`/`False`/`FALSE`, `0x`/`0o` ints |

Schema is opt-in -- without `yam_parser_set_schema()`, scalars have no tag.

```c
yam_parser_set_schema(parser, yam_schema_core());

/* events now carry resolved tags:
 *   "true"  -> tag:yaml.org,2002:bool
 *   "42"    -> tag:yaml.org,2002:int
 *   "hello" -> tag:yaml.org,2002:str
 *   "null"  -> tag:yaml.org,2002:null
 * quoted scalars always resolve to !!str
 * explicit tags (!!str, !foo) are never overwritten */
```

### Custom Schemas

Build your own schema to support YAML 1.1 booleans (`yes`/`no`/`on`/`off`)
or any other resolution rules:

```c
yam_schema_builder *b = yam_schema_builder_new(arena);

const char *trues[]  = {"true","True","TRUE","yes","Yes","YES","on","On","ON"};
const char *falses[] = {"false","False","FALSE","no","No","NO","off","Off","OFF"};
yam_schema_builder_add_bools(b, trues, 9, falses, 9);

const char *nulls[] = {"null","Null","NULL","~",""};
yam_schema_builder_add_nulls(b, nulls, 5);

yam_schema_builder_add_int(b);    /* 42, 0xFF, 0o77 */
yam_schema_builder_add_float(b);  /* 3.14, .inf, .nan */

const yam_schema *schema = yam_schema_builder_finish(b);  /* lives in the arena */
yam_schema_builder_free(b);

yam_parser_set_schema(parser, schema);
```

Rules are matched in order (first match wins). Match types: `YAM_MATCH_EXACT`,
`YAM_MATCH_ICASE`, and `YAM_MATCH_BUILTIN` (procedural int/float matchers).

## Architecture

```
 Input ──> Scanner ──> Parser ──> Emitter ──> Output
               \          |          /
                └─── Arena (memory) ─┘

 Scanner : flat token stream (SIMD-accelerated)
 Parser  : event stream (indent tracking, block structure,
           merge keys, alias resolution)
 Emitter : YAML text output (block / flow / minimal)
 Arena   : bump allocator backing all components
```

The scanner is intentionally "pure" -- it produces raw tokens without synthetic
block start/end markers. The parser layer handles indent-based block structure,
simple key resolution, and property (anchor/tag) attachment.

## Performance

Throughput on a 10 MB generated YAML document (GCC -O2 -march=native, SSE4.2):

```
  Component                    Avg MB/s    Best MB/s
  ──────────────────────────────────────────────────
  Scanner                      408         419
  Parser (block YAML)          167         169
  Parser (mixed YAML)          212         214
  Parser (JSON)                200         205
```

The parser uses an incremental state machine for both block and flow
context YAML, with a byte-scanning lookahead to avoid eager fallback
for nested flow collections. The lookahead result is cached for every
collection nested inside the one scanned, so it stays linear however
deeply collections nest. Quoted scalars without escapes or line breaks
are returned as zero-copy slices of the input. Tags, schemas, merge keys, and alias
resolution fall back to eager evaluation automatically.

Run `make bench` to test on your hardware. Use `make bench-cmp` to include
a libyaml comparison (requires libyaml installed).

## License

MIT
