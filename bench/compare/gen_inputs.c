/*
 * gen_inputs.c — Write the benchmark inputs to files, so every library
 * in the comparison parses exactly the same bytes.
 * Usage: ./gen_inputs <dir> [size_mb]
 */

#include "../bench_gen.h"

#include <string.h>

static int write_input(const char *dir, const char *name, char *buf, size_t len) {
    if (!buf) { fprintf(stderr, "allocation failed\n"); return 1; }
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "cannot write %s\n", path);
        free(buf);
        return 1;
    }
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dir> [size_mb]\n", argv[0]); return 2; }
    size_t mb = argc > 2 ? (size_t)atoi(argv[2]) : 10;
    if (mb < 1) mb = 1;
    size_t size = mb * 1024 * 1024, len;
    char *b;
    b = generate_block_yaml(size, &len);  if (write_input(argv[1], "block.yaml", b, len)) return 1;
    b = generate_mixed_yaml(size, &len);  if (write_input(argv[1], "mixed.yaml", b, len)) return 1;
    b = generate_json(size, &len);        if (write_input(argv[1], "json.yaml", b, len)) return 1;
    b = generate_config_yaml(size, &len); if (write_input(argv[1], "config.yaml", b, len)) return 1;
    return 0;
}
