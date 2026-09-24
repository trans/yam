/*
 * bench_parser.c — Throughput benchmark for yam vs libyaml (parser)
 *
 * Generates a large YAML document and measures parse throughput.
 * Usage: ./bench_parser [size_mb]
 */

#define _POSIX_C_SOURCE 199309L

#include "yam/yam.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench_gen.h"

/* ── Timing ──────────────────────────────────────────────── */

static double time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── Benchmark: yam parser ───────────────────────────────── */

static double bench_yam(const char *input, size_t len, int *event_count) {
    double t0 = time_sec();

    yam_arena *a = yam_arena_new(1 << 20); /* 1MB arena */
    yam_parser *p = yam_parser_new(input, len, a);
    yam_parser_set_max_events(p, 0);

    const yam_event *evt;
    int count = 0;
    while (yam_parse_next(p, &evt) == YAM_OK) {
        if (evt->type == YAM_EVT_STREAM_END || evt->type == YAM_EVT_NONE) break;
        count++;
    }

    yam_parser_free(p);
    yam_arena_free(a);

    *event_count = count;
    return time_sec() - t0;
}

/* ── Benchmark: libyaml parser (if available) ────────────── */

#ifdef HAS_LIBYAML
#include <yaml.h>

static double bench_libyaml(const char *input, size_t len, int *event_count) {
    double t0 = time_sec();

    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(&parser, (const unsigned char *)input, len);

    yaml_event_t event;
    int count = 0;
    int done = 0;

    while (!done) {
        yaml_parser_parse(&parser, &event);
        if (event.type == YAML_STREAM_END_EVENT) done = 1;
        count++;
        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);

    *event_count = count;
    return time_sec() - t0;
}
#endif

/* ── Run one benchmark ───────────────────────────────────── */

static void run_bench(const char *label, char *yaml, size_t len) {
    printf("  %s: %.2f MB (%zu bytes)\n\n", label, len / (1024.0 * 1024.0), len);

    /* warmup */
    int events;
    bench_yam(yaml, len, &events);

    int iterations = 5;
    double total = 0;
    double best = 1e9;

    printf("  %-10s  %-12s  %-12s  %-10s\n", "Run", "Time (ms)", "MB/s", "Events");
    printf("  ──────────────────────────────────────────────────\n");

    for (int i = 0; i < iterations; i++) {
        double elapsed = bench_yam(yaml, len, &events);
        double mbps = (len / (1024.0 * 1024.0)) / elapsed;
        total += elapsed;
        if (elapsed < best) best = elapsed;

        printf("  %-10d  %-12.2f  %-12.1f  %-10d\n",
               i + 1, elapsed * 1000, mbps, events);
    }

    double avg = total / iterations;
    double avg_mbps = (len / (1024.0 * 1024.0)) / avg;
    double best_mbps = (len / (1024.0 * 1024.0)) / best;

    printf("  ──────────────────────────────────────────────────\n");
    printf("  avg         %-12.2f  %-12.1f\n", avg * 1000, avg_mbps);
    printf("  best        %-12.2f  %-12.1f\n\n", best * 1000, best_mbps);

#ifdef HAS_LIBYAML
    printf("  libyaml comparison:\n");
    printf("  %-10s  %-12s  %-12s  %-10s\n", "Run", "Time (ms)", "MB/s", "Events");
    printf("  ──────────────────────────────────────────────────\n");

    total = 0;
    best = 1e9;

    for (int i = 0; i < iterations; i++) {
        double elapsed = bench_libyaml(yaml, len, &events);
        double mbps = (len / (1024.0 * 1024.0)) / elapsed;
        total += elapsed;
        if (elapsed < best) best = elapsed;

        printf("  %-10d  %-12.2f  %-12.1f  %-10d\n",
               i + 1, elapsed * 1000, mbps, events);
    }

    avg = total / iterations;
    avg_mbps = (len / (1024.0 * 1024.0)) / avg;
    best_mbps = (len / (1024.0 * 1024.0)) / best;

    printf("  ──────────────────────────────────────────────────\n");
    printf("  avg         %-12.2f  %-12.1f\n", avg * 1000, avg_mbps);
    printf("  best        %-12.2f  %-12.1f\n\n", best * 1000, best_mbps);
#endif
}

/* ── Main ────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    size_t target_mb = 10;
    if (argc > 1) target_mb = atoi(argv[1]);
    if (target_mb < 1) target_mb = 1;
    if (target_mb > 100) target_mb = 100;

    size_t target_size = target_mb * 1024 * 1024;

    printf("\nyam parser benchmark (%zu MB)\n", target_mb);
    printf("═══════════════════════════════════════════════════════════\n\n");

#if defined(__SSE4_2__)
    printf("  SIMD: SSE4.2\n\n");
#elif defined(__SSE2__)
    printf("  SIMD: SSE2 (no SSE4.2)\n\n");
#elif defined(__ARM_NEON)
    printf("  SIMD: NEON\n\n");
#else
    printf("  SIMD: none (scalar fallback)\n\n");
#endif

    /* ── Pure block YAML (incremental parser) ──────────── */
    size_t len;
    char *yaml = generate_block_yaml(target_size, &len);
    if (!yaml) { fprintf(stderr, "allocation failed\n"); return 1; }
    run_bench("Pure block YAML", yaml, len);
    free(yaml);

    /* ── Mixed YAML (flow + quotes → eager fallback) ───── */
    yaml = generate_mixed_yaml(target_size, &len);
    if (!yaml) { fprintf(stderr, "allocation failed\n"); return 1; }
    run_bench("Mixed YAML (flow + quotes)", yaml, len);
    free(yaml);

    /* ── JSON (valid YAML, all flow → eager fallback) ── */
    yaml = generate_json(target_size, &len);
    if (!yaml) { fprintf(stderr, "allocation failed\n"); return 1; }
    run_bench("JSON", yaml, len);
    free(yaml);

    /* ── Config-style YAML (comments, block scalars, aliases) ── */
    yaml = generate_config_yaml(target_size, &len);
    if (!yaml) { fprintf(stderr, "allocation failed\n"); return 1; }
    run_bench("Config-style YAML", yaml, len);
    free(yaml);

    return 0;
}
