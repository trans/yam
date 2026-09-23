/*
 * yam_internal.h — Declarations shared between yam's source files.
 *
 * Not installed. Everything here is hidden from the shared library's
 * exported symbols (the library is built with -fvisibility=hidden, and
 * only functions marked YAM_API in yam.h are exported), so it can change
 * freely without affecting the ABI.
 */

#ifndef YAM_INTERNAL_H
#define YAM_INTERNAL_H

#include "yam/yam.h"

/* ── Schema ──────────────────────────────────────────────── */

/* A single tag resolution rule: if a plain scalar matches `pattern`
 * according to `match`, it resolves to `tag`. */
typedef struct {
    yam_match_type  match;
    const char     *pattern;   /* string for EXACT/ICASE, name for BUILTIN */
    yam_str         tag;
} yam_schema_rule;

struct yam_schema {
    const yam_schema_rule *rules;
    int                    rule_count;
    yam_str                default_plain_tag;   /* unmatched plain scalars */
    yam_str                default_quoted_tag;  /* all quoted scalars */
    yam_str                default_seq_tag;     /* untagged sequences */
    yam_str                default_map_tag;     /* untagged mappings */
};

/* ── Scanner ─────────────────────────────────────────────── */

/* Scan the next token into caller-provided storage. yam_scan_next() wraps
 * this for the public API; the parser calls it directly. */
yam_status yam_scan_token(yam_scanner *s, yam_token *tok);

#endif /* YAM_INTERNAL_H */
