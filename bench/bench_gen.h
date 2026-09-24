/*
 * bench_gen.h — Generated benchmark inputs, shared by bench_parser.c and
 * the cross-library comparison in bench/compare/.
 */

#ifndef YAM_BENCH_GEN_H
#define YAM_BENCH_GEN_H

#include <stdio.h>
#include <stdlib.h>

/* ── Generate YAML documents ─────────────────────────────── */

/* Mixed YAML: mappings, sequences, flow collections, quoted strings */
static char *generate_mixed_yaml(size_t target_size, size_t *out_len) {
    char *buf = (char *)malloc(target_size + 4096);
    if (!buf) return NULL;

    size_t pos = 0;
    int item = 0;

    pos += sprintf(buf + pos, "---\n");
    pos += sprintf(buf + pos, "metadata:\n");
    pos += sprintf(buf + pos, "  name: benchmark-data\n");
    pos += sprintf(buf + pos, "  version: 1.2.0\n");
    pos += sprintf(buf + pos, "  generated: true\n");
    pos += sprintf(buf + pos, "entries:\n");

    while (pos < target_size) {
        pos += sprintf(buf + pos,
            "  - id: %d\n"
            "    name: \"item-%d\"\n"
            "    value: %d.%02d\n"
            "    tags: [alpha, beta, gamma]\n"
            "    nested:\n"
            "      x: %d\n"
            "      y: %d\n"
            "      label: 'entry #%d'\n",
            item, item,
            item * 17 % 1000, item * 31 % 100,
            item * 7 % 500, item * 13 % 500,
            item
        );
        item++;
    }

    pos += sprintf(buf + pos, "...\n");
    *out_len = pos;
    return buf;
}

/* Pure block YAML: only mappings and sequences, no flow or quotes */
static char *generate_block_yaml(size_t target_size, size_t *out_len) {
    char *buf = (char *)malloc(target_size + 4096);
    if (!buf) return NULL;

    size_t pos = 0;
    int item = 0;

    while (pos < target_size) {
        pos += sprintf(buf + pos,
            "- id: %d\n"
            "  name: item-%d\n"
            "  value: %d\n"
            "  nested:\n"
            "    x: %d\n"
            "    y: %d\n",
            item, item,
            item * 17 % 1000,
            item * 7 % 500, item * 13 % 500
        );
        item++;
    }

    *out_len = pos;
    return buf;
}

/* JSON (valid YAML): array of objects with quoted keys/strings */
static char *generate_json(size_t target_size, size_t *out_len) {
    char *buf = (char *)malloc(target_size + 4096);
    if (!buf) return NULL;

    size_t pos = 0;
    int item = 0;

    pos += sprintf(buf + pos, "[\n");

    while (pos < target_size) {
        if (item > 0) pos += sprintf(buf + pos, ",\n");
        pos += sprintf(buf + pos,
            "  {\"id\": %d, \"name\": \"item-%d\", \"value\": %d,"
            " \"nested\": {\"x\": %d, \"y\": %d}}",
            item, item,
            item * 17 % 1000,
            item * 7 % 500, item * 13 % 500
        );
        item++;
    }

    pos += sprintf(buf + pos, "\n]\n");
    *out_len = pos;
    return buf;
}

/* Config-style YAML: multiple documents with comments, block scalars,
 * anchors and aliases, as in CI pipelines and deployment manifests */
static char *generate_config_yaml(size_t target_size, size_t *out_len) {
    char *buf = (char *)malloc(target_size + 4096);
    if (!buf) return NULL;

    size_t pos = 0;
    int item = 0;

    while (pos < target_size) {
        pos += sprintf(buf + pos,
            "# service %d\n"
            "---\n"
            "apiVersion: apps/v1\n"
            "kind: Deployment\n"
            "metadata:\n"
            "  name: svc-%d  # generated\n"
            "  labels: &labels%d\n"
            "    app: svc-%d\n"
            "    tier: backend\n"
            "spec:\n"
            "  replicas: %d\n"
            "  selector:\n"
            "    matchLabels: *labels%d\n"
            "  template:\n"
            "    spec:\n"
            "      containers:\n"
            "        - name: main\n"
            "          image: \"registry.example.com/svc-%d:1.%d.%d\"\n"
            "          args: [\"--port\", \"80%02d\", \"--verbose\"]\n"
            "          env:\n"
            "            - name: MODE\n"
            "              value: 'production'\n"
            "          command:\n"
            "            - /bin/sh\n"
            "            - -c\n"
            "            - |\n"
            "              echo starting svc-%d\n"
            "              exec /app/server --workers %d\n"
            "          description: >\n"
            "            Folded text for service %d that\n"
            "            continues over two lines.\n",
            item, item, item, item, item % 5 + 1, item,
            item, item % 10, item % 7, item % 100,
            item, item % 16 + 1, item
        );
        item++;
    }

    *out_len = pos;
    return buf;
}

#endif
