# yam

A YAML 1.2 parser written in C11. Fast, minimal, zero-copy.

Features a SIMD-accelerated scanner (SSE4.2 / NEON with scalar fallback),
an event-based parser, and an arena allocator. Passes 363 of 406
[YAML Test Suite](https://github.com/yaml/yaml-test-suite) cases
(43 skipped due to missing expected output in the test suite).

## Build

```
make            # build libyam.a
make test       # run scanner unit tests
make test-suite # run YAML Test Suite (requires git submodules)
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

### Event Types

| Event | Meaning |
|-------|---------|
| `STREAM_START` / `STREAM_END` | Document stream boundaries |
| `DOC_START` / `DOC_END` | Document boundaries (`---` / `...`) |
| `MAPPING_START` / `MAPPING_END` | Mapping (key-value pairs) |
| `SEQUENCE_START` / `SEQUENCE_END` | Sequence (list) |
| `SCALAR` | Scalar value |
| `ALIAS` | Alias reference (`*name`) |

## Architecture

```
input bytes
    |
    v
 Scanner -----> flat token stream (SIMD-accelerated)
    |
    v
 Parser  -----> event stream (indent tracking, block structure)
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
