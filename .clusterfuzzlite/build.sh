#!/bin/bash -eu
# Build yam's fuzz target for ClusterFuzzLite. $CC, $CFLAGS (with the
# sanitizer flags), $LIB_FUZZING_ENGINE and $OUT come from the base image.

for f in src/*.c; do
    $CC $CFLAGS -std=c11 -Iinclude -c "$f" -o "$WORK/$(basename "$f" .c).o"
done
$CC $CFLAGS -std=c11 -Iinclude -c fuzz/fuzz_parser.c -o "$WORK/fuzz_parser.o"
# linked with $CXX even for C, as the libFuzzer runtime is C++
$CXX $CXXFLAGS "$WORK"/*.o $LIB_FUZZING_ENGINE -o "$OUT/fuzz_parser"

cp fuzz/yaml.dict "$OUT/fuzz_parser.dict"

# Seed corpus: the YAML Test Suite sources, when the submodule is present
# (each file is YAML; its first byte doubles as the harness's option flags).
if [ -d yaml-test-suite/src ]; then
    (cd yaml-test-suite/src && zip -q "$OUT/fuzz_parser_seed_corpus.zip" ./*.yaml)
fi
