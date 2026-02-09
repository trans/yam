CC      ?= gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wpedantic -march=native
CFLAGS  += -Iinclude

# SIMD: -march=native picks up SSE4.2/AVX/NEON as available
# For explicit control:
#   make CFLAGS+=-msse4.2     (x86)
#   make CFLAGS+=-mfpu=neon   (ARM)

SRCDIR   := src
OBJDIR   := build
TESTDIR  := test
BENCHDIR := bench

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

LIB     := $(OBJDIR)/libyam.a
TEST    := $(OBJDIR)/test_scanner
TEST_SUITE := $(OBJDIR)/test_yaml_suite
TEST_SCHEMA := $(OBJDIR)/test_schema
TEST_EMITTER := $(OBJDIR)/test_emitter
TEST_MERGE := $(OBJDIR)/test_merge
TEST_RESOLVE := $(OBJDIR)/test_resolve
TEST_ERRORS := $(OBJDIR)/test_errors

.PHONY: all clean test test-suite test-schema test-emitter test-merge test-resolve test-errors test-all bench bench-parser bench-cmp bench-parser-cmp

all: $(LIB) $(TEST)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	ar rcs $@ $^

$(TEST): $(TESTDIR)/test_scanner.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(OBJDIR)/bench_scanner: $(BENCHDIR)/bench_scanner.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(OBJDIR)/bench_scanner_cmp: $(BENCHDIR)/bench_scanner.c $(LIB)
	$(CC) $(CFLAGS) -DHAS_LIBYAML $< -L$(OBJDIR) -lyam -lyaml -o $@

$(OBJDIR)/bench_parser: $(BENCHDIR)/bench_parser.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(OBJDIR)/bench_parser_cmp: $(BENCHDIR)/bench_parser.c $(LIB)
	$(CC) $(CFLAGS) -DHAS_LIBYAML $< -L$(OBJDIR) -lyam -lyaml -o $@

$(TEST_SUITE): $(TESTDIR)/test_yaml_suite.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(TEST_SCHEMA): $(TESTDIR)/test_schema.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(TEST_EMITTER): $(TESTDIR)/test_emitter.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(TEST_MERGE): $(TESTDIR)/test_merge.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(TEST_RESOLVE): $(TESTDIR)/test_resolve.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

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

$(TEST_ERRORS): $(TESTDIR)/test_errors.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

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

clean:
	rm -rf $(OBJDIR)
