/*
 * fuzz_parser.c — libFuzzer harness for the yam parser.
 *
 * The first input byte selects parser options (merge keys, alias
 * resolution, core schema), so both the default incremental parser and the
 * eager path used by merge/resolve are exercised. The remaining bytes are
 * copied into an exact-size heap buffer — not NUL-terminated — so any read
 * past the end of the input is caught by ASAN. The arena is recreated per
 * iteration so we don't lose coverage of arena init/teardown.
 *
 * Besides crashes, the harness traps if the parser emits more events than
 * its event limit allows: that means a safety limit was bypassed (e.g. an
 * input that makes the parser spin out empty nodes).
 *
 * Build (via `just fuzz`):
 *   clang -fsanitize=fuzzer,address,undefined -g -O1 -Iinclude \
 *         fuzz/fuzz_parser.c src/yam_*.c -o build/fuzz_parser
 *
 * Run:
 *   ./build/fuzz_parser -max_total_time=60 \
 *       -artifact_prefix=fuzz/crashes/ fuzz/corpus
 *
 * Crash/oom inputs land under fuzz/crashes/; the corpus grows under
 * fuzz/corpus/ and is gitignored.
 */

#include "yam/yam.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 10000

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    uint8_t flags = data[0];
    data++;
    size--;

    char *buf = malloc(size ? size : 1);
    if (!buf) return 0;
    memcpy(buf, data, size);

    yam_arena *a = yam_arena_new(4096);
    if (!a) { free(buf); return 0; }

    yam_parser *p = yam_parser_new(buf, size, a);
    if (!p) { yam_arena_free(a); free(buf); return 0; }

    yam_parser_set_max_events(p, MAX_EVENTS);
    if (flags & 1) yam_parser_set_merge(p, true);
    if (flags & 2) yam_parser_set_resolve(p, true);
    yam_schema core = yam_schema_core();
    if (flags & 4) yam_parser_set_schema(p, &core);

    yam_event evt;
    int events = 0;
    while (yam_parse_next(p, &evt) == YAM_OK) {
        if (evt.type == YAM_EVT_STREAM_END || evt.type == YAM_EVT_NONE) break;
        if (++events > MAX_EVENTS) __builtin_trap(); /* limit bypassed */
    }

    yam_parser_free(p);
    yam_arena_free(a);
    free(buf);
    return 0;
}
