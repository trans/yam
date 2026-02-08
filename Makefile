CC      ?= gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wpedantic -march=native
CFLAGS  += -Iinclude

# SIMD: -march=native picks up SSE4.2/AVX/NEON as available
# For explicit control:
#   make CFLAGS+=-msse4.2     (x86)
#   make CFLAGS+=-mfpu=neon   (ARM)

SRCDIR  := src
OBJDIR  := build
TESTDIR := test

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

LIB     := $(OBJDIR)/libyam.a
TEST    := $(OBJDIR)/test_scanner
TEST_SUITE := $(OBJDIR)/test_yaml_suite
TEST_SCHEMA := $(OBJDIR)/test_schema
TEST_EMITTER := $(OBJDIR)/test_emitter
TEST_MERGE := $(OBJDIR)/test_merge
TEST_RESOLVE := $(OBJDIR)/test_resolve

.PHONY: all clean test test-suite test-schema test-emitter test-merge test-resolve test-all bench

all: $(LIB) $(TEST)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	ar rcs $@ $^

$(TEST): $(TESTDIR)/test_scanner.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(OBJDIR)/bench_scanner: $(TESTDIR)/bench_scanner.c $(LIB)
	$(CC) $(CFLAGS) $< -L$(OBJDIR) -lyam -o $@

$(OBJDIR)/bench_scanner_cmp: $(TESTDIR)/bench_scanner.c $(LIB)
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

test-all: test test-suite test-schema test-emitter test-merge test-resolve

bench: $(OBJDIR)/bench_scanner
	@./$(OBJDIR)/bench_scanner $(SIZE)

bench-cmp: $(OBJDIR)/bench_scanner_cmp
	@./$(OBJDIR)/bench_scanner_cmp $(SIZE)

clean:
	rm -rf $(OBJDIR)
