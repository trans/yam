# yam

A YAML 1.2 parser and emitter written in C11. Fast, minimal, zero-copy.

Features a SIMD-accelerated scanner (SSE4.2 / NEON with scalar fallback),
an event-based parser, an emitter with block/flow/minimal output styles,
merge key expansion, alias resolution, file input, structured error messages,
and an arena allocator. Passes 363 of 406
[YAML Test Suite](https://github.com/yaml/yaml-test-suite) cases
(43 skipped due to missing expected output in the test suite).
663 tests total, 0 failures.

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
yam_event   evt;

while (yam_parse_next(parser, &evt) == YAM_OK) {
    if (evt.type == YAM_EVT_STREAM_END) break;

    printf("%s", yam_event_type_str(evt.type));
    if (evt.anchor.data) printf(" &%.*s", (int)evt.anchor.len, evt.anchor.data);
    if (evt.tag.data)    printf(" <%.*s>", (int)evt.tag.len, evt.tag.data);
    if (evt.type == YAM_EVT_SCALAR)
        printf(" %.*s", (int)evt.value.len, evt.value.data);
    printf("\n");
}

yam_parser_free(parser);
yam_arena_free(arena);
```

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
yam_emitter *e = yam_emitter_new(YAM_EMIT_OPTS_DEFAULT, arena);
yam_event evt;
while (yam_parse_next(p, &evt) == YAM_OK) {
    yam_emit(e, &evt);
    if (evt.type == YAM_EVT_STREAM_END) break;
}

yam_str out = yam_emitter_output(e);
fwrite(out.data, 1, out.len, stdout);

yam_emitter_free(e);
yam_parser_free(p);
yam_arena_free(arena);
```

### Scanner (token API)

For lower-level access, the scanner produces a flat token stream without
synthetic block structure tokens:

```c
yam_scanner *scanner = yam_scanner_new(yaml, len, arena);
yam_token    tok;

while (yam_scan_next(scanner, &tok) == YAM_OK) {
    if (tok.type == YAM_TOK_STREAM_END) break;
    printf("%-20s %.*s\n",
           yam_token_type_str(tok.type),
           (int)tok.value.len, tok.value.data);
}

yam_scanner_free(scanner);
```

## API Overview

| Type | Description |
|------|-------------|
| `yam_str` | Non-owning string view (`data`, `len`) |
| `yam_mark` | Source position (`offset`, `line`, `col`) |
| `yam_token` | Scanner output (`type`, `value`, `scalar_style`, `start`, `end`) |
| `yam_event` | Parser output (`type`, `value`, `anchor`, `tag`, `scalar_style`, `flow`) |
| `yam_schema` | Tag resolution schema (failsafe, JSON, core, or custom) |
| `yam_schema_rule` | Single resolution rule (`match`, `pattern`, `tag`) |
| `yam_emitter` | Event-to-YAML emitter |
| `yam_emit_opts` | Emitter options (`style`, `indent`, `width`, `unicode`) |

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

The emitter converts an event stream back into YAML text. Three output
styles are available:

| Style | Description |
|-------|-------------|
| `YAM_EMIT_BLOCK` | Indented block style (default) |
| `YAM_EMIT_FLOW` | Flow style (`{key: value}`, `[a, b]`) |
| `YAM_EMIT_MINIMAL` | Compact flow with minimal whitespace |

Configure with `yam_emit_opts`:

```c
yam_emit_opts opts = {
    .style   = YAM_EMIT_BLOCK,
    .indent  = 2,      /* spaces per indent level */
    .width   = 80,     /* line width hint */
    .unicode = true,   /* allow non-ASCII unescaped */
};
yam_emitter *e = yam_emitter_new(opts, arena);
```

Or use the defaults:

```c
yam_emitter *e = yam_emitter_new(YAM_EMIT_OPTS_DEFAULT, arena);
```

The emitter automatically quotes scalars that would be ambiguous as plain
text (YAML keywords like `true`/`null`, numeric values, strings containing
`: ` or ` #`, etc.).

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
`database` as direct entries. Explicit keys override merged ones. Merge with
a sequence of aliases merges all of them (first wins on conflicts). Quoted
`"<<"` is treated as a normal key.

## Alias Resolution

Enable alias resolution to expand `*alias` references inline instead of
emitting `ALIAS` events:

```c
yam_parser_set_resolve(parser, true);
```

Scalar, mapping, and sequence aliases are all expanded. Circular and unknown
references are kept as `ALIAS` events (no error, no infinite loop). Combines
with merge keys when both are enabled.

## Error Handling

Both the scanner and parser provide error messages with source locations:

```c
yam_parser *p = yam_parser_new(yaml, strlen(yaml), arena);
yam_event evt;

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
yam_schema core = yam_schema_core();
yam_parser_set_schema(parser, &core);

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

yam_schema schema = yam_schema_builder_finish(b);
yam_schema_builder_free(b);

yam_parser_set_schema(parser, &schema);
```

Rules are matched in order (first match wins). Match types: `YAM_MATCH_EXACT`,
`YAM_MATCH_ICASE`, and `YAM_MATCH_BUILTIN` (procedural int/float matchers).

## Architecture

```
input bytes
    |
    v
 Scanner -----> flat token stream (SIMD-accelerated)
    |
    v
 Parser  -----> event stream (indent tracking, block structure)
    |                          merge keys, alias resolution
    v
 Emitter -----> YAML text (block / flow / minimal)
    |
    v
 Arena   -----> zero-copy string views into source + arena for decoded scalars
```

The scanner is intentionally "pure" -- it produces raw tokens without synthetic
block start/end markers. The parser layer handles indent-based block structure,
simple key resolution, and property (anchor/tag) attachment.

## Performance

Scanner throughput on a 10 MB generated YAML document (AMD Zen, GCC -O2 -march=native):

```
  Run         Time (ms)     MB/s          Tokens
  ──────────────────────────────────────────────────
  avg         26.15         382.4
  best        24.63         406.0

  SIMD: SSE4.2
```

Run `make bench` to test on your hardware. Use `make bench-cmp` to include
a libyaml comparison (requires libyaml installed).

## License

MIT
