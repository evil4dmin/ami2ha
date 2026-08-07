# ami2ha -- portable core tests, built with the host compiler.
#
# These never touch AmigaOS APIs, so the protocol and parsing code can be
# verified in a second on the development machine instead of round-tripping
# through an emulator.

HOSTCC   ?= cc
CFLAGS   := -std=c99 -Wall -Wextra -Wno-unused-parameter -g -O1 -I../include
LDLIBS   := -lm

CORE     := $(wildcard ../src/core/*.c)
TESTS    := $(wildcard test_*.c) main.c
RUNNER   := run-tests

.PHONY: run clean

run: $(RUNNER)
	@./$(RUNNER)

$(RUNNER): $(CORE) $(TESTS)
	$(HOSTCC) $(CFLAGS) -o $@ $(CORE) $(TESTS) $(LDLIBS)

clean:
	rm -rf $(RUNNER) $(RUNNER).dSYM
