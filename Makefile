CC ?= cc
CFLAGS ?= -O2 -g
WARN = -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS = -lm
SOURCES = src/server.c src/store.c
.PHONY: all clean test sanitize experimental
all: bin/kvstore
bin/kvstore: $(SOURCES) src/daily.h
	mkdir -p bin
	$(CC) $(CFLAGS) $(WARN) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)
test: all
	python3 -m unittest discover -s tests -v
sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(MAKE) CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
experimental:
	$(MAKE) -f Makefile.experimental all
# Build cleanup never deletes user data.
clean:
	rm -f bin/kvstore
