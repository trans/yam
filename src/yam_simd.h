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

/* Find first structural byte (whitespace, # : , [ ] { } or NUL) that could end
 * a plain scalar. Returns its offset, or `len` if none is found. */
size_t yam_scan_plain_scalar(const char *buf, size_t len);

/* Skip spaces and tabs. Returns offset of the first non-blank, or `len`. */
size_t yam_skip_blanks(const char *buf, size_t len);

/* Find first line break (\n or \r). Returns its offset, or `len`. */
size_t yam_scan_to_break(const char *buf, size_t len);

/* Scan to end of comment (to line break or EOF) — same as scan_to_break. */
#define yam_skip_comment yam_scan_to_break

#endif /* YAM_SIMD_H */
