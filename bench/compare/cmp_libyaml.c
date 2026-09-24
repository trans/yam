/* cmp_libyaml.c — libyaml: pull every event from yaml_parser_parse. */

#define _POSIX_C_SOURCE 200809L
#include <yaml.h>
#include "cmp_common.h"

static long parse_once(char *input, size_t len) {
    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(&parser, (const unsigned char *)input, len);
    yaml_event_t event;
    long n = 0;
    for (;;) {
        if (!yaml_parser_parse(&parser, &event)) { n = -1; break; }
        n++;
        int done = event.type == YAML_STREAM_END_EVENT;
        yaml_event_delete(&event);
        if (done) break;
    }
    yaml_parser_delete(&parser);
    return n;
}

int main(int argc, char **argv) { return cmp_main("libyaml", argc, argv); }
