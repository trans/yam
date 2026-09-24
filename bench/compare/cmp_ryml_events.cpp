// cmp_ryml_events.cpp — rapidyaml's event parser (EventHandlerInts): no tree,
// like the event parsers. Needs rapidyaml's source release (see run.sh).
// Buffers are sized once by the warmup parse and reused.
#include <c4/yml/parse_engine.def.hpp>
#include <c4/yml/extra/event_handler_ints.hpp>
#include <c4/yml/extra/ints_utils.hpp>
#include <c4/std/vector.hpp>
#include "cmp_common.h"
using namespace c4::yml;
static extra::EventHandlerInts handler;
static ParseEngine<extra::EventHandlerInts> parser(&handler);
static std::vector<extra::ievt::evt_bits> events;
static std::vector<char> arena;
static long parse_once(char *input, size_t len) {
    if (events.empty()) {
        events.resize(extra::estimate_events_ints_size(c4::csubstr(input, len)) + 64);
        arena.resize(len + 64);
    }
    handler.reset(c4::substr(input, len), c4::substr(arena.data(), arena.size()),
                  events.data(), (extra::ievt::evt_bits)events.size());
    parser.parse_in_place_ev("in", c4::substr(input, len));
    if (!handler.fits_buffers()) return -1;
    return (long)handler.required_size_events();
}
int main(int argc, char **argv) { return cmp_main("rapidyaml-events", argc, argv); }
