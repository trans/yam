/*
 * cmp_common.h — Timing loop shared by the comparison harnesses.
 *
 * Each harness defines parse_once(), which parses the whole input and
 * returns the number of events (or tree nodes) produced, or -1 on error.
 * The loop reads the file, warms up, times `runs` parses and prints one
 * line: "<lib> <file> <median MB/s> <best MB/s> <count>".
 */

#ifndef CMP_COMMON_H
#define CMP_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long parse_once(char *input, size_t len);

static double cmp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int cmp_main(const char *lib, int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file> [runs]\n", argv[0]); return 2; }
    int runs = argc > 2 ? atoi(argv[2]) : 15;
    if (runs < 1) runs = 1;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    size_t len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *orig = (char *)malloc(len + 1), *work = (char *)malloc(len + 1);
    if (!orig || !work || fread(orig, 1, len, f) != len) { fclose(f); return 1; }
    fclose(f);
    orig[len] = '\0';

    /* a fresh copy per run, outside the timing: in-place parsers may
     * rewrite the buffer */
    memcpy(work, orig, len + 1);
    long count = parse_once(work, len);          /* warmup */
    if (count < 0) {
        printf("%s %s ERR\n", lib, argv[1]);
        return 1;
    }

    double *t = (double *)malloc(sizeof(double) * (size_t)runs);
    for (int i = 0; i < runs; i++) {
        memcpy(work, orig, len + 1);
        double t0 = cmp_now();
        parse_once(work, len);
        t[i] = cmp_now() - t0;
    }
    qsort(t, (size_t)runs, sizeof(double), cmp_dbl);
    double mb = (double)len / (1024.0 * 1024.0);
    printf("%s %s %.1f %.1f %ld\n", lib, argv[1], mb / t[runs / 2], mb / t[0], count);
    free(t);
    free(orig);
    free(work);
    return 0;
}

#endif
