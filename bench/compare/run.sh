#!/bin/sh
# run.sh — Compare parse throughput of yam, libyaml, libfyaml and rapidyaml
# on the same generated inputs. Libraries that aren't available are skipped.
#
#   bench/compare/run.sh [size_mb] [runs]
#
# Environment:
#   RYML_HEADER  path to a rapidyaml single header, e.g.
#                rapidyaml.v0.16.0.singlehdr.hpp from its GitHub releases
#   RYML_SRC     unpacked rapidyaml source release, for its event parser
#                (the fair comparison: events, no tree)
#   CASES        directory of extra input files to benchmark too
#   CPU          core to pin to with taskset (default 3; empty disables)
#   CC, CXX, CFLAGS, CXXFLAGS  compilers and flags (default -O2)
set -e

SIZE=${1:-10}
RUNS=${2:-15}
CPU=${CPU-3}
CC=${CC:-cc}
CXX=${CXX:-c++}
CFLAGS=${CFLAGS:--O2}
CXXFLAGS=${CXXFLAGS:--O2 -DNDEBUG}

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/build/compare
mkdir -p "$OUT"

$CC -O2 -I"$ROOT/bench" "$HERE/gen_inputs.c" -o "$OUT/gen_inputs"
"$OUT/gen_inputs" "$OUT" "$SIZE"

LIBS="yam"
# yam is compiled from source here so it always gets these CFLAGS (the
# Makefile doesn't rebuild build/libyam.a when CFLAGS change)
$CC -std=c11 $CFLAGS -I"$ROOT/include" "$HERE/cmp_yam.c" "$ROOT"/src/*.c -o "$OUT/cmp_yam"
if $CC $CFLAGS "$HERE/cmp_libyaml.c" -lyaml -o "$OUT/cmp_libyaml" 2>/dev/null; then
    LIBS="$LIBS libyaml"
else
    echo "libyaml not found: skipped" >&2
fi
if $CC $CFLAGS "$HERE/cmp_libfyaml.c" -lfyaml -o "$OUT/cmp_libfyaml" 2>/dev/null; then
    LIBS="$LIBS libfyaml"
else
    echo "libfyaml not found: skipped" >&2
fi
if [ -n "$RYML_HEADER" ] && [ -f "$RYML_HEADER" ]; then
    $CXX $CXXFLAGS -std=c++17 -DRYML_HEADER="\"$RYML_HEADER\"" \
        "$HERE/cmp_ryml.cpp" -o "$OUT/cmp_rapidyaml"
    $CXX $CXXFLAGS -std=c++17 -DRYML_HEADER="\"$RYML_HEADER\"" -DRYML_REUSE \
        "$HERE/cmp_ryml.cpp" -o "$OUT/cmp_rapidyaml-reuse"
    LIBS="$LIBS rapidyaml rapidyaml-reuse"
else
    echo "RYML_HEADER not set: rapidyaml skipped" >&2
fi
# rapidyaml's event parser isn't in its single header: build it from the
# source release (rapidyaml.vX.src.tgz, unpacked; it bundles c4core)
if [ -n "$RYML_SRC" ] && [ -d "$RYML_SRC/src_extra" ]; then
    mkdir -p "$OUT/ryml_obj"
    for f in $(cd "$RYML_SRC" && find src src_extra ext/c4core.src -name '*.cpp'); do
        $CXX $CXXFLAGS -std=c++17 -I"$RYML_SRC/src" -I"$RYML_SRC/src_extra" \
            -I"$RYML_SRC/ext/c4core.src" -c "$RYML_SRC/$f" \
            -o "$OUT/ryml_obj/$(echo "$f" | tr / _).o" &
    done
    wait
    $CXX $CXXFLAGS -std=c++17 -I"$RYML_SRC/src" -I"$RYML_SRC/src_extra" \
        -I"$RYML_SRC/ext/c4core.src" "$HERE/cmp_ryml_events.cpp" \
        "$OUT"/ryml_obj/*.o -o "$OUT/cmp_rapidyaml-events"
    LIBS="$LIBS rapidyaml-events"
else
    echo "RYML_SRC not set: rapidyaml's event parser skipped" >&2
fi

PIN=""
[ -n "$CPU" ] && command -v taskset >/dev/null && PIN="taskset -c $CPU"

INPUTS="$OUT/block.yaml $OUT/mixed.yaml $OUT/json.yaml $OUT/config.yaml"
if [ -n "$CASES" ]; then
    for f in "$CASES"/*; do [ -f "$f" ] && INPUTS="$INPUTS $f"; done
fi

echo
echo "Parse throughput in MB/s, median of $RUNS runs${PIN:+ (pinned to CPU $CPU)}"
echo
printf '%-36s' "input"
for lib in $LIBS; do printf '%17s' "$lib"; done
echo
for f in $INPUTS; do
    printf '%-36s' "$(basename "$f")"
    for lib in $LIBS; do
        r=$($PIN "$OUT/cmp_$lib" "$f" "$RUNS" 2>/dev/null | awk '{print $3}')
        printf '%17s' "${r:-crash}"
    done
    echo
done
