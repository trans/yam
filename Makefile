CC      ?= gcc

# Optimization / arch flags. Overridable so distro packaging can supply its
# own portable, hardened flags. NOTE: -march=native is a convenience for local
# dev/bench only — it is NOT portable, so redistributable builds must override
# CFLAGS (the packaging targets and distro build systems do this). SIMD no
# longer depends on it: yam_simd.c selects an SSE4.2 or scalar implementation
# at runtime, so a portable build still uses SIMD on capable CPUs.
CFLAGS  ?= -O2 -march=native

WARNINGS := -Wall -Wextra -Wpedantic

# Flags always applied; do not override these from the command line.
YAM_CFLAGS  := -std=c11 $(WARNINGS) $(CFLAGS) -Iinclude
YAM_LDFLAGS := $(LDFLAGS)

SRCDIR   := src
OBJDIR   := build
TESTDIR  := test
BENCHDIR := bench

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
SHOBJS  := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.shared.o)

# ── Version (single source of truth: include/yam/yam.h) ──────────────────────
VERSION_MAJOR := $(shell sed -n 's/^\#define YAM_VERSION_MAJOR \([0-9]*\).*/\1/p' include/yam/yam.h)
VERSION_MINOR := $(shell sed -n 's/^\#define YAM_VERSION_MINOR \([0-9]*\).*/\1/p' include/yam/yam.h)
VERSION_PATCH := $(shell sed -n 's/^\#define YAM_VERSION_PATCH \([0-9]*\).*/\1/p' include/yam/yam.h)
VERSION       := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
SOVERSION     := $(VERSION_MAJOR)

# ── Install layout (overridable by packagers) ────────────────────────────────
DESTDIR      ?=
PREFIX       ?= /usr/local
LIBDIR       ?= $(PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
DISTDIR      ?= pkg

LIB        := $(OBJDIR)/libyam.a
SONAME     := libyam.so.$(SOVERSION)
SHLIB      := libyam.so.$(VERSION)
SHLIB_PATH := $(OBJDIR)/$(SHLIB)
PC         := $(OBJDIR)/yam.pc

TEST    := $(OBJDIR)/test_scanner
TEST_SUITE := $(OBJDIR)/test_yaml_suite
TEST_SCHEMA := $(OBJDIR)/test_schema
TEST_EMITTER := $(OBJDIR)/test_emitter
TEST_MERGE := $(OBJDIR)/test_merge
TEST_RESOLVE := $(OBJDIR)/test_resolve
TEST_ERRORS := $(OBJDIR)/test_errors

.PHONY: all static shared pkgconfig install uninstall dist version \
        clean test test-suite test-schema test-emitter test-merge \
        test-resolve test-errors test-all bench bench-parser bench-cmp bench-parser-cmp

all: $(LIB) $(TEST)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(YAM_CFLAGS) -c $< -o $@

$(OBJDIR)/%.shared.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(YAM_CFLAGS) -fPIC -c $< -o $@

$(LIB): $(OBJS)
	ar rcs $@ $^

# ── Shared library (libyam.so.MAJOR -> libyam.so.VERSION, + dev symlink) ──────
static: $(LIB)

shared: $(SHLIB_PATH)

$(SHLIB_PATH): $(SHOBJS)
	$(CC) $(YAM_LDFLAGS) -shared -Wl,-soname,$(SONAME) -o $@ $^
	ln -sf $(SHLIB) $(OBJDIR)/$(SONAME)
	ln -sf $(SONAME) $(OBJDIR)/libyam.so

# ── pkg-config ────────────────────────────────────────────────────────────────
pkgconfig: $(PC)

$(PC): yam.pc.in | $(OBJDIR)
	sed -e 's,@PREFIX@,$(PREFIX),g' \
	    -e 's,@LIBDIR@,$(LIBDIR),g' \
	    -e 's,@INCLUDEDIR@,$(INCLUDEDIR),g' \
	    -e 's,@VERSION@,$(VERSION),g' \
	    $< > $@

# ── Install / uninstall ───────────────────────────────────────────────────────
install: $(LIB) $(SHLIB_PATH) $(PC)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 $(SHLIB_PATH) $(DESTDIR)$(LIBDIR)/$(SHLIB)
	ln -sf $(SHLIB) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libyam.so
	install -m 0644 $(LIB) $(DESTDIR)$(LIBDIR)/libyam.a
	install -d $(DESTDIR)$(INCLUDEDIR)/yam
	install -m 0644 include/yam/*.h $(DESTDIR)$(INCLUDEDIR)/yam/
	install -d $(DESTDIR)$(PKGCONFIGDIR)
	install -m 0644 $(PC) $(DESTDIR)$(PKGCONFIGDIR)/yam.pc
	@echo "installed yam $(VERSION) to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(SHLIB) \
	      $(DESTDIR)$(LIBDIR)/$(SONAME) \
	      $(DESTDIR)$(LIBDIR)/libyam.so \
	      $(DESTDIR)$(LIBDIR)/libyam.a \
	      $(DESTDIR)$(PKGCONFIGDIR)/yam.pc
	rm -rf $(DESTDIR)$(INCLUDEDIR)/yam
	@echo "removed yam from $(DESTDIR)$(PREFIX)"

# ── Source tarball for packaging ──────────────────────────────────────────────
# Captures tracked + new (non-ignored) files from the working tree, excluding
# the yaml-test-suite submodule. On a clean CI checkout this is just the
# committed files; locally it also picks up staged/new work.
version:
	@echo $(VERSION)

dist:
	@mkdir -p $(DISTDIR)
	git ls-files -z --cached --others --exclude-standard \
	  | grep -zZv -e '^yaml-test-suite' -e '[.]png$$' \
	  | tar --null -T - --transform 's,^,yam-$(VERSION)/,' \
	        -czf $(DISTDIR)/yam-$(VERSION).tar.gz
	@echo "wrote $(DISTDIR)/yam-$(VERSION).tar.gz"

# ── Tests ─────────────────────────────────────────────────────────────────────
$(TEST): $(TESTDIR)/test_scanner.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

$(OBJDIR)/bench_scanner: $(BENCHDIR)/bench_scanner.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

$(OBJDIR)/bench_scanner_cmp: $(BENCHDIR)/bench_scanner.c $(LIB)
	$(CC) $(YAM_CFLAGS) -DHAS_LIBYAML $< $(LIB) -lyaml -o $@

$(OBJDIR)/bench_parser: $(BENCHDIR)/bench_parser.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

$(OBJDIR)/bench_parser_cmp: $(BENCHDIR)/bench_parser.c $(LIB)
	$(CC) $(YAM_CFLAGS) -DHAS_LIBYAML $< $(LIB) -lyaml -o $@

$(TEST_SUITE): $(TESTDIR)/test_yaml_suite.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

$(TEST_SCHEMA): $(TESTDIR)/test_schema.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

$(TEST_EMITTER): $(TESTDIR)/test_emitter.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

$(TEST_MERGE): $(TESTDIR)/test_merge.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

$(TEST_RESOLVE): $(TESTDIR)/test_resolve.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

$(TEST_ERRORS): $(TESTDIR)/test_errors.c $(LIB)
	$(CC) $(YAM_CFLAGS) $< $(LIB) -o $@

test: $(TEST)
	@echo "─── Running scanner tests ───"
	@./$(TEST)

test-suite: $(TEST_SUITE)
	@echo "─── Running YAML Test Suite ───"
	@./$(TEST_SUITE) $(ARGS)

test-schema: $(TEST_SCHEMA)
	@echo "─── Running schema tests ───"
	@./$(TEST_SCHEMA)

test-emitter: $(TEST_EMITTER)
	@echo "─── Running emitter tests ───"
	@./$(TEST_EMITTER)

test-merge: $(TEST_MERGE)
	@echo "─── Running merge key tests ───"
	@./$(TEST_MERGE)

test-resolve: $(TEST_RESOLVE)
	@echo "─── Running resolve tests ───"
	@./$(TEST_RESOLVE)

test-errors: $(TEST_ERRORS)
	@echo "─── Running error tests ───"
	@./$(TEST_ERRORS)

test-all: test test-suite test-schema test-emitter test-merge test-resolve test-errors

bench: $(OBJDIR)/bench_scanner $(OBJDIR)/bench_parser
	@./$(OBJDIR)/bench_scanner $(SIZE)
	@./$(OBJDIR)/bench_parser $(SIZE)

bench-parser: $(OBJDIR)/bench_parser
	@./$(OBJDIR)/bench_parser $(SIZE)

bench-cmp: $(OBJDIR)/bench_scanner_cmp
	@./$(OBJDIR)/bench_scanner_cmp $(SIZE)

bench-parser-cmp: $(OBJDIR)/bench_parser_cmp
	@./$(OBJDIR)/bench_parser_cmp $(SIZE)

# ── Fuzzer ────────────────────────────────────────────────────────────────────
# Compile the harness together with the library sources, all under
# libFuzzer + ASAN + UBSAN. Single-shot compile (no .a) so all units share
# the same instrumentation. Run via `just fuzz`.
$(OBJDIR)/fuzz_parser: fuzz/fuzz_parser.c $(SRCS) | $(OBJDIR)
	clang -std=c11 -Wall -Wextra -Iinclude \
	  -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1 \
	  $< $(SRCS) -o $@

clean:
	rm -rf $(OBJDIR)
