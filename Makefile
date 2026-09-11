# Frame-DSL build.
#
#   make          build the hello-world binary
#   make deps     fetch the CLibs libraries into third_party/
#   make test     build and run the test suite
#   make clean    remove build output, leaving third_party/ alone

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

# CLibs' own sources are built with this instead. Our warning policy is ours to answer for; failing
# the build on a warning in code this project only consumes would make every upstream release a
# build break to work around here. format-truncation is silenced by name rather than broadly:
# testsuite.c builds two paths under a 256-byte buffer that glibc's _FORTIFY_SOURCE cannot prove
# short enough, and it is not this project's warning to fix.
THIRD_PARTY_CFLAGS ?= -std=c11 -O2 -Wno-format-truncation

BUILD := build
THIRD_PARTY := third_party
LOGGER := $(THIRD_PARTY)/logger
TESTING := $(THIRD_PARTY)/testing

# Written by fetch_deps.sh once every library is unpacked. It depends on the script rather than on
# a version variable here, so bumping CLIBS_VERSION in the script is what triggers a re-fetch.
DEPS := $(THIRD_PARTY)/.fetched

TARGET := $(BUILD)/hello_world
SRC := $(wildcard src/*.c)

TEST_RUNNER := $(BUILD)/run_tests
TEST_SRC := $(wildcard test/*.c)
TEST_REGISTRY := $(BUILD)/inc/testsuite_registry.h
TESTSUITE_OBJ := $(BUILD)/testsuite.o

.PHONY: all deps test clean

all: $(TARGET)

deps: $(DEPS)

$(DEPS): scripts/fetch_deps.sh
	@scripts/fetch_deps.sh

$(TARGET): $(SRC) | $(DEPS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(LOGGER) $^ -o $@

# Regenerated whenever a story file is added or removed, which is the whole point of generating it.
$(TEST_REGISTRY): $(TEST_SRC) | $(DEPS)
	@mkdir -p $(@D)
	$(TESTING)/gen_test_registry test $@

$(TESTSUITE_OBJ): $(TESTING)/testsuite.c | $(DEPS)
	@mkdir -p $(@D)
	$(CC) $(THIRD_PARTY_CFLAGS) -I$(LOGGER) -I$(TESTING) -c $< -o $@

$(TEST_RUNNER): $(TEST_SRC) $(TEST_REGISTRY) $(TESTSUITE_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(LOGGER) -I$(TESTING) -I$(BUILD)/inc \
		$(TEST_SRC) $(TESTSUITE_OBJ) -o $@

test: $(TEST_RUNNER)
	./$(TEST_RUNNER)

clean:
	rm -rf $(BUILD)
