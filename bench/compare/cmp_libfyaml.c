/* cmp_libfyaml.c — libfyaml: pull every event from fy_parser_parse. */

#define _POSIX_C_SOURCE 200809L
#include <libfyaml.h>
#include "cmp_common.h"

static long parse_once(char *input, size_t len) {
    struct fy_parse_cfg cfg = { .flags = FYPCF_QUIET };
    struct fy_parser *fyp = fy_parser_create(&cfg);
    if (!fyp || fy_parser_set_string(fyp, input, len) != 0) return -1;
    struct fy_event *ev;
    long n = 0;
    while ((ev = fy_parser_parse(fyp)) != NULL) {
        n++;
        fy_parser_event_free(fyp, ev);
    }
    long r = fy_parser_get_stream_error(fyp) ? -1 : n;
    fy_parser_destroy(fyp);
    return r;
}

int main(int argc, char **argv) { return cmp_main("libfyaml", argc, argv); }
