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

.PHONY: all clean test test-suite test-schema test-all bench

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

test: $(TEST)
	@echo "─── Running scanner tests ───"
	@./$(TEST)

test-suite: $(TEST_SUITE)
	@echo "─── Running YAML Test Suite ───"
	@./$(TEST_SUITE) $(ARGS)

test-schema: $(TEST_SCHEMA)
	@echo "─── Running schema tests ───"
	@./$(TEST_SCHEMA)

test-all: test test-suite test-schema

bench: $(OBJDIR)/bench_scanner
	@./$(OBJDIR)/bench_scanner $(SIZE)

bench-cmp: $(OBJDIR)/bench_scanner_cmp
	@./$(OBJDIR)/bench_scanner_cmp $(SIZE)

clean:
	rm -rf $(OBJDIR)
