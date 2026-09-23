/*
 * fuzz_parser.c — libFuzzer harness for the yam parser.
 *
 * The first input byte selects options: bit 0 merge keys, bit 1 alias
 * resolution, bit 2 core schema, bits 3-4 emitter style. So both the
 * default incremental parser and the eager path used by merge/resolve are
 * exercised. The remaining bytes are copied into an exact-size heap buffer
 * — not NUL-terminated — so any read past the end of the input is caught
 * by ASAN. The arena is recreated per iteration so we don't lose coverage
 * of arena init/teardown.
 *
 * Besides crashes, the harness traps if:
 *  - the parser emits more events than its event limit allows (a safety
 *    limit was bypassed);
 *  - input that parses is emitted as YAML that doesn't parse back to the
 *    same meaning (event types, values, anchors, tags, alias names, and
 *    for untagged scalars their core schema type).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 10000

/* Append a canonical form of one event (see test_yaml_suite.c). */
static void canon(const yam_event *e, char **buf, size_t *len, size_t *cap) {
    if (e->type == YAM_EVT_STREAM_START || e->type == YAM_EVT_STREAM_END) return;
    char head[64];
    int n = snprintf(head, sizeof head, "%d|%zu|%zu|", (int)e->type,
                     e->anchor.len, e->tag.len);
    size_t need = (size_t)n + e->anchor.len + e->tag.len + e->value.len + 16;
    if (*len + need > *cap) {
        size_t nc = (*cap + need) * 2;
        char *nb = realloc(*buf, nc);
        if (!nb) abort();
        *buf = nb;
        *cap = nc;
    }
    char *o = *buf + *len;
    memcpy(o, head, (size_t)n); o += n;
    if (e->anchor.len) { memcpy(o, e->anchor.data, e->anchor.len); o += e->anchor.len; }
    if (e->tag.len) { memcpy(o, e->tag.data, e->tag.len); o += e->tag.len; }
    if (e->type == YAM_EVT_SCALAR || e->type == YAM_EVT_ALIAS) {
        bool untagged = !e->tag.len;
        bool plain = e->scalar_style == YAM_SCALAR_PLAIN;
        if (e->type == YAM_EVT_SCALAR && untagged && plain &&
            (e->value.len == 0 || (e->value.len == 1 && e->value.data[0] == '~'))) {
            *o++ = '~';                         /* both mean null */
        } else {
            if (e->type == YAM_EVT_SCALAR && untagged) {
                yam_str t = yam_schema_resolve(yam_schema_core(), e->value, e->scalar_style);
                *o++ = t.data[t.len - 2];       /* last letters differ per type */
                *o++ = t.data[t.len - 1];
            }
            if (e->value.len) { memcpy(o, e->value.data, e->value.len); o += e->value.len; }
        }
    }
    *o++ = '\n';
    *len = (size_t)(o - *buf);
}

/* Parse `yaml` into a canonical string, optionally feeding an emitter.
 * Returns false if parsing fails. */
static bool parse_canon(const char *yaml, size_t len, uint8_t flags, yam_emitter *em,
                        char **out, size_t *out_len) {
    yam_arena *a = yam_arena_new(4096);
    if (!a) return false;
    yam_parser *p = yam_parser_new(yaml, len, a);
    if (!p) { yam_arena_free(a); return false; }
    yam_parser_set_max_events(p, MAX_EVENTS);
    if (flags & 1) yam_parser_set_merge(p, true);
    if (flags & 2) yam_parser_set_resolve(p, true);
    if (flags & 4) yam_parser_set_schema(p, yam_schema_core());

    size_t cap = 0;
    const yam_event *evt;
    yam_status st;
    int events = 0;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK) {
        if (evt->type == YAM_EVT_NONE) break;
        if (++events > MAX_EVENTS) __builtin_trap(); /* limit bypassed */
        if (em) yam_emit(em, evt);
        canon(evt, out, out_len, &cap);
        if (evt->type == YAM_EVT_STREAM_END) break;
    }
    yam_parser_free(p);
    yam_arena_free(a);
    return st == YAM_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    uint8_t flags = data[0];
    data++;
    size--;

    char *buf = malloc(size ? size : 1);
    if (!buf) return 0;
    memcpy(buf, data, size);

    yam_arena *ea = yam_arena_new(4096);
    yam_emitter *em = ea ? yam_emitter_new(ea) : NULL;
    if (em) {
        int style = (flags >> 3) & 3;
        yam_emitter_set_style(em, style == 1 ? YAM_EMIT_FLOW
                                  : style == 2 ? YAM_EMIT_MINIMAL : YAM_EMIT_BLOCK);
    }

    char *before = NULL, *after = NULL;
    size_t before_len = 0, after_len = 0;
    bool ok = parse_canon(buf, size, flags, em, &before, &before_len);

    /* round trip: the emitted YAML must parse back to the same meaning.
     * Merge/alias expansion applies to the input only: the emitted text
     * already has them expanded. */
    if (ok && em) {
        yam_str out = yam_emitter_output(em);
        if (!parse_canon(out.data ? out.data : "", out.len, flags & 4, NULL,
                         &after, &after_len))
            __builtin_trap(); /* emitted YAML does not parse */
        if (before_len != after_len ||
            (before_len && memcmp(before, after, before_len) != 0))
            __builtin_trap(); /* emitted YAML means something else */
    }

    free(before);
    free(after);
    yam_emitter_free(em);
    yam_arena_free(ea);
    free(buf);
    return 0;
}
