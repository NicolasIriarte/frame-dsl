# Frame-DSL build.
#
#   make          build build/frame-dsl
#   make deps     unpack the vendored CLibs libraries
#   make test     build and run the test suite
#   make clean    remove build output
#
# The build needs no network and no credentials: the CLibs release tarballs are committed under
# third_party/clibs/ and unpacked into build/. To bump them, run scripts/vendor_clibs.sh by hand and
# commit what it replaces.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

# CLibs' own sources are built with this instead. Our warning policy is ours to answer for; failing
# the build on a warning in code this project only consumes would make every upstream release a
# build break to work around here. format-truncation is silenced by name rather than broadly:
# testsuite.c builds two paths under a 256-byte buffer that glibc's _FORTIFY_SOURCE cannot prove
# short enough, and it is not this project's warning to fix.
THIRD_PARTY_CFLAGS ?= -std=c11 -O2 -Wno-format-truncation

BUILD := build

# The committed tarballs, and where they are unpacked to. Unpacking is build output, so `make clean`
# discards it and the next build redoes it -- which costs a local untar rather than a download.
CLIBS_DIST := third_party/clibs
CLIBS_ARCHIVES := $(wildcard $(CLIBS_DIST)/*.tar.gz)

DEPS_DIR := $(BUILD)/deps
LOGGER := $(DEPS_DIR)/logger
TESTING := $(DEPS_DIR)/testing

# Version lives in the filenames, so the archives are globbed rather than named: bumping CLibs is
# replacing a file, with nothing here to keep in step.
DEPS := $(DEPS_DIR)/.unpacked

TARGET := $(BUILD)/frame-dsl
SRC := $(wildcard src/*.c)

TEST_RUNNER := $(BUILD)/run_tests
TEST_SRC := $(wildcard test/*.c)
TEST_REGISTRY := $(BUILD)/inc/testsuite_registry.h
TESTSUITE_OBJ := $(BUILD)/testsuite.o

.PHONY: all deps test clean

all: $(TARGET)

deps: $(DEPS)

# Each archive holds one <lib>-<version>/ directory; that component is stripped so include paths
# name the library and never the version.
$(DEPS): $(CLIBS_ARCHIVES)
	@mkdir -p $(DEPS_DIR)
	@for archive in $(CLIBS_ARCHIVES); do \
		name="$$(basename "$$archive" .tar.gz)"; \
		lib="$${name%-v*}"; \
		rm -rf "$(DEPS_DIR)/$$lib"; \
		mkdir -p "$(DEPS_DIR)/$$lib"; \
		tar xzf "$$archive" -C "$(DEPS_DIR)/$$lib" --strip-components=1; \
		echo "unpacked $$name"; \
	done
	@touch $@

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
