/*
 * yam_simd.h — Platform SIMD scan primitives
 *
 * These functions scan a buffer for the next "interesting" byte. Each has a
 * SIMD (SSE4.2) and a scalar implementation; the active one is chosen at load
 * time by a runtime CPU check (see yam_simd.c). This lets a single compiled
 * binary use SIMD on capable hardware while staying safe on older CPUs, so it
 * is suitable for redistributable packages built without -march=native.
 */

#ifndef YAM_SIMD_H
#define YAM_SIMD_H

#include <stddef.h>
#include <stdint.h>

/* Find first structural byte (whitespace, # : , [ ] { } or NUL) that could end
 * a plain scalar. Returns its offset, or `len` if none is found. */
size_t yam_scan_plain_scalar(const char *buf, size_t len);

/* Skip spaces and tabs. Returns offset of the first non-blank, or `len`. */
size_t yam_skip_blanks(const char *buf, size_t len);

/* Find first line break (\n or \r). Returns its offset, or `len`. */
size_t yam_scan_to_break(const char *buf, size_t len);

/* Scan to end of comment (to line break or EOF) — same as scan_to_break. */
#define yam_skip_comment yam_scan_to_break

/* Bitmask of the bytes the flow-key lookahead looks at in the 64 bytes at
 * p (all must be readable): bit k is set if p[k] is one of ' " # [ ] { }
 * \n \r. Inline, since it runs once per 64 input bytes in a hot loop; SSE2
 * is part of x86-64, so this needs no runtime dispatch. */

#if defined(__SSE2__)
#include <emmintrin.h>

static inline uint64_t yam_flow_mask16(__m128i v) {
    const __m128i lower = _mm_or_si128(v, _mm_set1_epi8(0x20)); /* [ → {, ] → } */
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(lower, _mm_set1_epi8('{')),
                             _mm_cmpeq_epi8(lower, _mm_set1_epi8('}')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('"')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\'')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('#')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\n')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\r')));
    return (uint16_t)_mm_movemask_epi8(m);
}

static inline uint64_t yam_flow_mask64(const char *p) {
    return yam_flow_mask16(_mm_loadu_si128((const __m128i *)p)) |
           yam_flow_mask16(_mm_loadu_si128((const __m128i *)(p + 16))) << 16 |
           yam_flow_mask16(_mm_loadu_si128((const __m128i *)(p + 32))) << 32 |
           yam_flow_mask16(_mm_loadu_si128((const __m128i *)(p + 48))) << 48;
}
#else
static inline uint64_t yam_flow_mask64(const char *p) {
    uint64_t m = 0;
    for (int k = 0; k < 64; k++) {
        char c = p[k];
        if (c == '\'' || c == '"' || c == '#' || c == '[' || c == ']' ||
            c == '{' || c == '}' || c == '\n' || c == '\r')
            m |= (uint64_t)1 << k;
    }
    return m;
}
#endif

#endif /* YAM_SIMD_H */
