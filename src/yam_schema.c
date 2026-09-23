/*
 * yam_schema.c — YAML 1.2 tag schema resolution
 *
 * Provides pluggable tag resolution for untagged scalars per YAML 1.2
 * spec Chapter 10 (Recommended Schemas). Ships with three presets:
 *   - Failsafe: everything is str/seq/map
 *   - JSON:     null, true/false, int, float (strict)
 *   - Core:     extended booleans, octal/hex ints, ~, empty null
 *
 * Users can build custom schemas via the builder API, e.g. to add
 * YAML 1.1 boolean terms (on/off/yes/no).
 */

#include "yam_internal.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Tag constant definitions ────────────────────────────── */

static const char TAG_NULL[]  = "tag:yaml.org,2002:null";
static const char TAG_BOOL[]  = "tag:yaml.org,2002:bool";
static const char TAG_INT[]   = "tag:yaml.org,2002:int";
static const char TAG_FLOAT[] = "tag:yaml.org,2002:float";
static const char TAG_STR[]   = "tag:yaml.org,2002:str";
static const char TAG_SEQ[]   = "tag:yaml.org,2002:seq";
static const char TAG_MAP[]   = "tag:yaml.org,2002:map";
static const char TAG_MERGE[] = "tag:yaml.org,2002:merge";

const yam_str YAM_TAG_NULL  = { TAG_NULL,  sizeof(TAG_NULL)  - 1 };
const yam_str YAM_TAG_BOOL  = { TAG_BOOL,  sizeof(TAG_BOOL)  - 1 };
const yam_str YAM_TAG_INT   = { TAG_INT,   sizeof(TAG_INT)   - 1 };
const yam_str YAM_TAG_FLOAT = { TAG_FLOAT, sizeof(TAG_FLOAT) - 1 };
const yam_str YAM_TAG_STR   = { TAG_STR,   sizeof(TAG_STR)   - 1 };
const yam_str YAM_TAG_SEQ   = { TAG_SEQ,   sizeof(TAG_SEQ)   - 1 };
const yam_str YAM_TAG_MAP   = { TAG_MAP,   sizeof(TAG_MAP)   - 1 };
const yam_str YAM_TAG_MERGE = { TAG_MERGE, sizeof(TAG_MERGE) - 1 };

/* ── Built-in procedural matchers ────────────────────────── */

/*
 * Core/JSON int: [-+]?[0-9]+  |  0x[0-9a-fA-F]+  |  0o[0-7]+
 */
static bool match_int(const char *s, size_t len) {
    if (len == 0) return false;
    size_t i = 0;

    /* optional sign */
    if (s[0] == '+' || s[0] == '-') {
        i = 1;
        if (i >= len) return false;
    }

    /* 0x hex */
    if (i + 1 < len && s[i] == '0' && s[i + 1] == 'x') {
        i += 2;
        if (i >= len) return false;
        for (; i < len; i++) {
            char c = s[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F')))
                return false;
        }
        return true;
    }

    /* 0o octal */
    if (i + 1 < len && s[i] == '0' && s[i + 1] == 'o') {
        i += 2;
        if (i >= len) return false;
        for (; i < len; i++) {
            if (s[i] < '0' || s[i] > '7') return false;
        }
        return true;
    }

    /* decimal */
    if (i >= len || s[i] < '0' || s[i] > '9') return false;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

/*
 * Core/JSON float:
 *   [-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?
 *   [-+]?(\.inf)
 *   \.nan
 */
static bool match_float(const char *s, size_t len) {
    if (len == 0) return false;
    size_t i = 0;

    /* .nan */
    if (len == 4 && memcmp(s, ".nan", 4) == 0) return true;

    /* optional sign */
    if (s[0] == '+' || s[0] == '-') {
        i = 1;
        if (i >= len) return false;
    }

    /* .inf */
    if (i + 4 == len && memcmp(s + i, ".inf", 4) == 0) return true;

    /* must have digits or leading dot */
    bool has_dot = false;
    bool has_digit = false;
    bool has_exp = false;

    /* leading dot: .123 */
    if (s[i] == '.') {
        has_dot = true;
        i++;
        if (i >= len || s[i] < '0' || s[i] > '9') return false;
        has_digit = true;
        for (i++; i < len && s[i] >= '0' && s[i] <= '9'; i++)
            ;
    } else {
        /* digits before dot/exp */
        if (s[i] < '0' || s[i] > '9') return false;
        has_digit = true;
        for (; i < len && s[i] >= '0' && s[i] <= '9'; i++)
            ;
        if (i < len && s[i] == '.') {
            has_dot = true;
            i++;
            for (; i < len && s[i] >= '0' && s[i] <= '9'; i++)
                ;
        }
    }

    /* exponent */
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        has_exp = true;
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= len || s[i] < '0' || s[i] > '9') return false;
        for (i++; i < len && s[i] >= '0' && s[i] <= '9'; i++)
            ;
    }

    /* must consume all input, and must have dot or exponent to be float */
    return i == len && has_digit && (has_dot || has_exp);
}

/* ── Core resolution function ────────────────────────────── */

yam_str yam_schema_resolve(const yam_schema *schema, yam_str value,
                           yam_scalar_style style) {
    /* quoted scalars always resolve to default_quoted_tag (str) */
    if (style != YAM_SCALAR_PLAIN)
        return schema->default_quoted_tag;

    const char *val = value.data;
    size_t vlen = value.len;

    for (int i = 0; i < schema->rule_count; i++) {
        const yam_schema_rule *r = &schema->rules[i];
        switch (r->match) {
        case YAM_MATCH_EXACT: {
            size_t plen = strlen(r->pattern);
            if (vlen == plen && (plen == 0 || memcmp(val, r->pattern, plen) == 0))
                return r->tag;
            break;
        }
        case YAM_MATCH_ICASE: {
            size_t plen = strlen(r->pattern);
            if (vlen != plen) break;
            bool match = true;
            for (size_t j = 0; j < plen; j++) {
                if (tolower((unsigned char)val[j]) !=
                    tolower((unsigned char)r->pattern[j])) {
                    match = false;
                    break;
                }
            }
            if (match) return r->tag;
            break;
        }
        case YAM_MATCH_BUILTIN:
            if (strcmp(r->pattern, "int") == 0 && match_int(val, vlen))
                return r->tag;
            if (strcmp(r->pattern, "float") == 0 && match_float(val, vlen))
                return r->tag;
            break;
        }
    }

    return schema->default_plain_tag;
}

/* ── Failsafe schema ─────────────────────────────────────── */

#define DEFAULT_TAGS \
    .default_plain_tag  = { TAG_STR, sizeof(TAG_STR) - 1 }, \
    .default_quoted_tag = { TAG_STR, sizeof(TAG_STR) - 1 }, \
    .default_seq_tag    = { TAG_SEQ, sizeof(TAG_SEQ) - 1 }, \
    .default_map_tag    = { TAG_MAP, sizeof(TAG_MAP) - 1 }

static const yam_schema failsafe_schema = { .rules = NULL, .rule_count = 0, DEFAULT_TAGS };

const yam_schema *yam_schema_failsafe(void) {
    return &failsafe_schema;
}

/* ── JSON schema ─────────────────────────────────────────── */

static const yam_schema_rule json_rules[] = {
    { YAM_MATCH_EXACT,   "null",  { TAG_NULL,  sizeof(TAG_NULL)  - 1 } },
    { YAM_MATCH_EXACT,   "true",  { TAG_BOOL,  sizeof(TAG_BOOL)  - 1 } },
    { YAM_MATCH_EXACT,   "false", { TAG_BOOL,  sizeof(TAG_BOOL)  - 1 } },
    { YAM_MATCH_BUILTIN, "int",   { TAG_INT,   sizeof(TAG_INT)   - 1 } },
    { YAM_MATCH_BUILTIN, "float", { TAG_FLOAT, sizeof(TAG_FLOAT) - 1 } },
};

static const yam_schema json_schema = {
    .rules = json_rules,
    .rule_count = sizeof(json_rules) / sizeof(json_rules[0]),
    DEFAULT_TAGS
};

const yam_schema *yam_schema_json(void) {
    return &json_schema;
}

/* ── Core schema ─────────────────────────────────────────── */

static const yam_schema_rule core_rules[] = {
    { YAM_MATCH_EXACT, "null",  { TAG_NULL, sizeof(TAG_NULL) - 1 } },
    { YAM_MATCH_EXACT, "Null",  { TAG_NULL, sizeof(TAG_NULL) - 1 } },
    { YAM_MATCH_EXACT, "NULL",  { TAG_NULL, sizeof(TAG_NULL) - 1 } },
    { YAM_MATCH_EXACT, "~",     { TAG_NULL, sizeof(TAG_NULL) - 1 } },
    { YAM_MATCH_EXACT, "",      { TAG_NULL, sizeof(TAG_NULL) - 1 } },
    { YAM_MATCH_EXACT, "true",  { TAG_BOOL, sizeof(TAG_BOOL) - 1 } },
    { YAM_MATCH_EXACT, "True",  { TAG_BOOL, sizeof(TAG_BOOL) - 1 } },
    { YAM_MATCH_EXACT, "TRUE",  { TAG_BOOL, sizeof(TAG_BOOL) - 1 } },
    { YAM_MATCH_EXACT, "false", { TAG_BOOL, sizeof(TAG_BOOL) - 1 } },
    { YAM_MATCH_EXACT, "False", { TAG_BOOL, sizeof(TAG_BOOL) - 1 } },
    { YAM_MATCH_EXACT, "FALSE", { TAG_BOOL, sizeof(TAG_BOOL) - 1 } },
    { YAM_MATCH_BUILTIN, "int",   { TAG_INT,   sizeof(TAG_INT)   - 1 } },
    { YAM_MATCH_BUILTIN, "float", { TAG_FLOAT, sizeof(TAG_FLOAT) - 1 } },
};

static const yam_schema core_schema = {
    .rules = core_rules,
    .rule_count = sizeof(core_rules) / sizeof(core_rules[0]),
    DEFAULT_TAGS
};

const yam_schema *yam_schema_core(void) {
    return &core_schema;
}

/* ── Schema builder ──────────────────────────────────────── */

struct yam_schema_builder {
    yam_arena       *arena;
    yam_schema_rule *rules;
    int              len;
    int              cap;
    bool             oom;   /* an add failed; finish() returns NULL */
};

yam_schema_builder *yam_schema_builder_new(yam_arena *a) {
    yam_schema_builder *b = malloc(sizeof(*b));
    if (!b) return NULL;
    b->arena = a;
    b->cap = 16;
    b->len = 0;
    b->oom = false;
    b->rules = malloc(b->cap * sizeof(yam_schema_rule));
    if (!b->rules) { free(b); return NULL; }
    return b;
}

/* Make room for `need` more rules. On failure the builder is marked and
 * the caller must not add. */
static bool builder_ensure(yam_schema_builder *b, int need) {
    if (b->oom || need < 0) return false;
    if (b->len + need <= b->cap) return true;
    int new_cap = b->cap * 2;
    while (new_cap < b->len + need) new_cap *= 2;
    yam_schema_rule *new_rules = realloc(b->rules, (size_t)new_cap * sizeof(yam_schema_rule));
    if (!new_rules) { b->oom = true; return false; }
    b->rules = new_rules;
    b->cap = new_cap;
    return true;
}

void yam_schema_builder_add(yam_schema_builder *b,
                            yam_match_type match,
                            const char *pattern, yam_str tag) {
    if (!builder_ensure(b, 1)) return;
    b->rules[b->len++] = (yam_schema_rule){ match, pattern, tag };
}

void yam_schema_builder_add_bools(yam_schema_builder *b,
                                  const char **true_terms, int ntrue,
                                  const char **false_terms, int nfalse) {
    if (!builder_ensure(b, ntrue + nfalse)) return;
    for (int i = 0; i < ntrue; i++)
        b->rules[b->len++] = (yam_schema_rule){ YAM_MATCH_EXACT, true_terms[i], YAM_TAG_BOOL };
    for (int i = 0; i < nfalse; i++)
        b->rules[b->len++] = (yam_schema_rule){ YAM_MATCH_EXACT, false_terms[i], YAM_TAG_BOOL };
}

void yam_schema_builder_add_nulls(yam_schema_builder *b,
                                  const char **terms, int nterms) {
    if (!builder_ensure(b, nterms)) return;
    for (int i = 0; i < nterms; i++)
        b->rules[b->len++] = (yam_schema_rule){ YAM_MATCH_EXACT, terms[i], YAM_TAG_NULL };
}

void yam_schema_builder_add_int(yam_schema_builder *b) {
    yam_schema_builder_add(b, YAM_MATCH_BUILTIN, "int", YAM_TAG_INT);
}

void yam_schema_builder_add_float(yam_schema_builder *b) {
    yam_schema_builder_add(b, YAM_MATCH_BUILTIN, "float", YAM_TAG_FLOAT);
}

const yam_schema *yam_schema_builder_finish(yam_schema_builder *b) {
    if (b->oom) return NULL;
    /* Copy the schema, its rules, and their patterns and tags into the
     * arena, so it outlives the builder and the caller's strings */
    yam_schema *schema = yam_arena_alloc(b->arena, sizeof *schema, _Alignof(yam_schema));
    yam_schema_rule *stable = yam_arena_alloc(
        b->arena, (size_t)b->len * sizeof(yam_schema_rule) + 1, _Alignof(yam_schema_rule));
    if (!schema || !stable) return NULL;
    for (int i = 0; i < b->len; i++) {
        const yam_schema_rule *r = &b->rules[i];
        const char *pattern = yam_arena_dup(b->arena, r->pattern, strlen(r->pattern));
        const char *tag = r->tag.data ? yam_arena_dup(b->arena, r->tag.data, r->tag.len) : NULL;
        if (!pattern || (r->tag.data && !tag)) return NULL;
        stable[i] = (yam_schema_rule){ r->match, pattern, { tag, r->tag.len } };
    }
    *schema = (yam_schema){ .rules = stable, .rule_count = b->len, DEFAULT_TAGS };
    return schema;
}

void yam_schema_builder_free(yam_schema_builder *b) {
    if (!b) return;
    free(b->rules);
    free(b);
}
