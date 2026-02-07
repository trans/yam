/*
 * yam_simd.h — Platform SIMD detection and intrinsics
 */

#ifndef YAM_SIMD_H
#define YAM_SIMD_H

#include <stddef.h>
#include <stdint.h>

/* ── Detect available SIMD ───────────────────────────────── */

#if defined(__SSE4_2__)
    #define YAM_SSE42 1
    #include <nmmintrin.h>
#elif defined(__SSE2__)
    #define YAM_SSE2 1
    #include <emmintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    #define YAM_NEON 1
    #include <arm_neon.h>
#endif

/* ── SIMD-accelerated scan: find first structural character ─
 *
 * Returns offset of first "interesting" byte, or `len` if none found.
 * Structural = anything that could end a plain scalar or whitespace run.
 */

static inline size_t yam_scan_plain_scalar(const char *buf, size_t len) {
    size_t i = 0;

#if defined(YAM_SSE42)
    /*
     * SSE4.2 PCMPESTRI — explicit length variant.
     * Must use explicit length because our ranges contain \0.
     * Mode: unsigned bytes, ranges, find first match.
     */
    const __m128i ranges = _mm_loadu_si128(
        (const __m128i *)yam_simd_struct_ranges
    );
    const int mode = _SIDD_UBYTE_OPS | _SIDD_CMP_RANGES
                   | _SIDD_LEAST_SIGNIFICANT;

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        int chunk_len = (len - i) < 16 ? (int)(len - i) : 16;
        int idx = _mm_cmpestri(ranges, YAM_SIMD_STRUCT_RANGES_LEN,
                               chunk, chunk_len, mode);
        if (idx < 16) return i + idx;
    }
#endif

    /* scalar fallback — still uses the lookup table, fast enough for tail */
    for (; i < len; i++) {
        uint8_t c = (uint8_t)buf[i];
        if (c <= ' ' || c == '#' || c == ':' || c == ','
            || c == '[' || c == ']' || c == '{' || c == '}') {
            return i;
        }
    }
    return len;
}

/* Scan for end of whitespace (space/tab only, not breaks) */
static inline size_t yam_skip_blanks(const char *buf, size_t len) {
    size_t i = 0;

#if defined(YAM_SSE42)
    const __m128i spaces = _mm_set1_epi8(' ');
    const __m128i tabs   = _mm_set1_epi8('\t');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i eq_sp = _mm_cmpeq_epi8(chunk, spaces);
        __m128i eq_tb = _mm_cmpeq_epi8(chunk, tabs);
        __m128i is_blank = _mm_or_si128(eq_sp, eq_tb);
        int mask = _mm_movemask_epi8(is_blank);
        if (mask != 0xFFFF) {
            /* not all blanks — find first non-blank */
            return i + __builtin_ctz(~mask);
        }
    }
#endif

    for (; i < len; i++) {
        if (buf[i] != ' ' && buf[i] != '\t') return i;
    }
    return len;
}

/* Scan for line break (\n or \r) */
static inline size_t yam_scan_to_break(const char *buf, size_t len) {
    size_t i = 0;

#if defined(YAM_SSE42)
    const __m128i nl = _mm_set1_epi8('\n');
    const __m128i cr = _mm_set1_epi8('\r');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i eq_nl = _mm_cmpeq_epi8(chunk, nl);
        __m128i eq_cr = _mm_cmpeq_epi8(chunk, cr);
        int mask = _mm_movemask_epi8(_mm_or_si128(eq_nl, eq_cr));
        if (mask) return i + __builtin_ctz(mask);
    }
#endif

    for (; i < len; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') return i;
    }
    return len;
}

/* Scan to end of comment (to line break or EOF) — alias */
#define yam_skip_comment yam_scan_to_break

#endif /* YAM_SIMD_H */
