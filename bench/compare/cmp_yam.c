/* cmp_yam.c — yam: pull every event from the incremental parser. */

#define _POSIX_C_SOURCE 200809L
#include "yam/yam.h"
#include "cmp_common.h"

static long parse_once(char *input, size_t len) {
    yam_arena *a = yam_arena_new(1 << 20);
    yam_parser *p = yam_parser_new(input, len, a);
    yam_parser_set_max_events(p, 0);
    const yam_event *evt;
    yam_status st;
    long n = 0;
    while ((st = yam_parse_next(p, &evt)) == YAM_OK) {
        n++;
        if (evt->type == YAM_EVT_STREAM_END || evt->type == YAM_EVT_NONE) break;
    }
    yam_parser_free(p);
    yam_arena_free(a);
    return st == YAM_OK ? n : -1;
}

int main(int argc, char **argv) { return cmp_main("yam", argc, argv); }
