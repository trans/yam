// cmp_ryml.cpp — rapidyaml: parse in place into a tree. rapidyaml has no
// public event API; building its tree is its normal parse path.
// Needs the single header from a rapidyaml release (see run.sh).
//
// Built twice by run.sh:
//   rapidyaml        a fresh parser and tree per parse (like the others)
//   rapidyaml-reuse  one parser and tree reused across parses, with the
//                    tree reserved to the needed size: the best case, as
//                    in rapidyaml's own "inplace_reuse_reserve" benchmark

#define RYML_SINGLE_HDR_DEFINE_NOW
#include RYML_HEADER
#include "cmp_common.h"

#ifdef RYML_REUSE
static ryml::EventHandlerTree handler;
static ryml::Parser parser(&handler);
static ryml::Tree tree;

static long parse_once(char *input, size_t len) {
    tree.clear();
    tree.clear_arena();
    ryml::parse_in_place(&parser, ryml::substr(input, len), &tree);
    tree.reserve(tree.size());   // a no-op after the warmup parse
    return (long)tree.size();
}

int main(int argc, char **argv) { return cmp_main("rapidyaml-reuse", argc, argv); }
#else
static long parse_once(char *input, size_t len) {
    ryml::Tree tree;
    ryml::parse_in_place(ryml::substr(input, len), &tree);
    return (long)tree.size();
}

int main(int argc, char **argv) { return cmp_main("rapidyaml", argc, argv); }
#endif
