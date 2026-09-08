CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -Iinclude -Ivendor
LDFLAGS ?= -lsqlite3 -lpthread

CLANG_FORMAT ?= /Library/Developer/CommandLineTools/usr/bin/clang-format
CLANG_TIDY ?= $(shell command -v clang-tidy 2>/dev/null || echo "/opt/homebrew/opt/llvm/bin/clang-tidy")

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

SRCS = src/config.c \
       src/filter.c \
       src/db.c \
       src/hasher.c \
       src/compressor.c \
       src/scanner_targets.c \
       src/scanner.c \
       src/report.c \
       src/queue.c \
       vendor/cJSON.c

OBJS = $(SRCS:.c=.o)
MAIN_OBJ = src/main.o
TARGET = fscomp

TEST_SRCS = tests/test_suite.c
TEST_TARGET = test_suite

FORMAT_FILES = include/*.h src/*.h src/*.c tests/*.c

.PHONY: all clean test asan format tidy install uninstall upgrade

all: $(TARGET)

$(TARGET): $(OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

vendor/cJSON.o: vendor/cJSON.c
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(OBJS) $(TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(TEST_SRCS) $(LDFLAGS)

asan:
	$(CC) -std=c11 -Wall -Wextra -fsanitize=address,undefined -g -Iinclude -Ivendor -Wno-deprecated-declarations $(SRCS) $(TEST_SRCS) -o test_asan $(LDFLAGS)
	./test_asan
	rm -f test_asan

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

tidy:
	$(CLANG_TIDY) -p . --extra-arg="-isysroot" --extra-arg="$(shell xcrun --show-sdk-path)" src/*.c tests/*.c

install: $(TARGET)
	mkdir -p $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)

uninstall:
	rm -f $(BINDIR)/$(TARGET)

upgrade:
	@if [ -d .git ] && git rev-parse --abbrev-ref --symbolic-full-name @{u} >/dev/null 2>&1; then \
		echo "Pulling latest changes..."; \
		git pull --ff-only || exit 1; \
	fi
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) install
	@echo "Upgrade complete: $(BINDIR)/$(TARGET)"

clean:
	rm -f $(OBJS) $(MAIN_OBJ) $(TARGET) $(TEST_TARGET) test_asan tests/*.o
