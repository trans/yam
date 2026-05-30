/*
 * smoke_install.c — End-to-end check that an installed yam package works.
 *
 * Compiled inside a clean target-distro container against the system-
 * installed libyam (via pkg-config). Proves, in order:
 *
 *   1. Headers landed where the compiler can find them (yam/yam.h).
 *   2. pkg-config resolves both Cflags and Libs (the .pc file is valid).
 *   3. The dynamic linker finds libyam.so.0 at run time (ldconfig ran,
 *      and the lib is on the standard search path).
 *   4. The library actually parses a real document end-to-end.
 *
 * Exit 0 on success; non-zero on any of the above failing. Output line is
 * stable so the install-test recipes can grep it.
 */

#include <yam/yam.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    yam_arena *a = yam_arena_new(4096);
    if (!a) { fprintf(stderr, "arena alloc failed\n"); return 2; }

    const char *yaml = "k: v\n";
    yam_parser *p = yam_parser_new(yaml, strlen(yaml), a);
    if (!p) { fprintf(stderr, "parser new failed\n"); yam_arena_free(a); return 2; }

    int scalars = 0;
    yam_event e;
    while (yam_parse_next(p, &e) == YAM_OK) {
        if (e.type == YAM_EVT_STREAM_END) break;
        if (e.type == YAM_EVT_SCALAR) scalars++;
    }

    yam_parser_free(p);
    yam_arena_free(a);

    printf("yam %d.%d.%d: parsed %d scalar(s) from \"k: v\"\n",
           YAM_VERSION_MAJOR, YAM_VERSION_MINOR, YAM_VERSION_PATCH, scalars);

    return scalars == 2 ? 0 : 1;
}
