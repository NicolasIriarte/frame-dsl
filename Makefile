# Frame-DSL build.
#
#   make              build build/frame-dsl
#   make deps         unpack the vendored CLibs libraries
#   make test         run both levels of tests
#   make test-unit    unit tests only
#   make test-system  system tests only
#   make clean        remove build output
#
# Tests come at two levels. Unit tests reach into a module directly and are compiled with src/ on
# their include path. System tests use the project the way a user does -- the public interface and
# the binary -- and are compiled WITHOUT src/, so a system test that reaches for an internal header
# fails to build. That boundary is enforced by the compiler rather than by anyone remembering it.
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

# Everything but the entry point is compiled once into objects, so the binary and both test runners
# link the same code rather than recompiling it three ways.
LIB_SRC := $(filter-out src/main.c, $(wildcard src/*.c))
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/obj/%.o)

UNIT_SRC := $(wildcard test/unit/*.c)
UNIT_RUNNER := $(BUILD)/run_unit_tests
UNIT_REGISTRY := $(BUILD)/inc/unit/testsuite_registry.h

SYSTEM_SRC := $(wildcard test/system/*.c)
SYSTEM_RUNNER := $(BUILD)/run_system_tests
SYSTEM_REGISTRY := $(BUILD)/inc/system/testsuite_registry.h

# One per level, not one shared. testsuite.c sizes its user-story array from
# TESTSUITE_USER_STORY_COUNT, which it picks up from whichever testsuite_registry.h is on its include
# path when it is compiled -- so the count is baked into the object. A single object built without a
# registry sizes the array for zero stories and halts on the second one registered.
TESTSUITE_UNIT_OBJ := $(BUILD)/testsuite_unit.o
TESTSUITE_SYSTEM_OBJ := $(BUILD)/testsuite_system.o

.PHONY: all deps test test-unit test-system clean

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

$(BUILD)/obj/%.o: src/%.c | $(DEPS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(LOGGER) -Isrc -c $< -o $@

$(TARGET): src/main.c $(LIB_OBJ) | $(DEPS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(LOGGER) -Isrc $^ -o $@

$(TESTSUITE_UNIT_OBJ): $(TESTING)/testsuite.c $(UNIT_REGISTRY) | $(DEPS)
	@mkdir -p $(@D)
	$(CC) $(THIRD_PARTY_CFLAGS) -I$(LOGGER) -I$(TESTING) -I$(BUILD)/inc/unit -c $< -o $@

$(TESTSUITE_SYSTEM_OBJ): $(TESTING)/testsuite.c $(SYSTEM_REGISTRY) | $(DEPS)
	@mkdir -p $(@D)
	$(CC) $(THIRD_PARTY_CFLAGS) -I$(LOGGER) -I$(TESTING) -I$(BUILD)/inc/system -c $< -o $@

# Regenerated whenever a story file is added or removed, which is the whole point of generating it.
$(UNIT_REGISTRY): $(UNIT_SRC) | $(DEPS)
	@mkdir -p $(@D)
	$(TESTING)/gen_test_registry test/unit $@

$(SYSTEM_REGISTRY): $(SYSTEM_SRC) | $(DEPS)
	@mkdir -p $(@D)
	$(TESTING)/gen_test_registry test/system $@

$(UNIT_RUNNER): $(UNIT_SRC) $(UNIT_REGISTRY) $(LIB_OBJ) $(TESTSUITE_UNIT_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(LOGGER) -I$(TESTING) -I$(BUILD)/inc/unit -Isrc \
		$(UNIT_SRC) $(LIB_OBJ) $(TESTSUITE_UNIT_OBJ) -o $@

# Note what is missing: -Isrc. A system test cannot include an internal header, because the compiler
# cannot find one. FRAMEDSL_BIN and EXAMPLES_DIR are passed in so a test drives the real binary and
# the real example files rather than a copy that can drift.
$(SYSTEM_RUNNER): $(SYSTEM_SRC) $(SYSTEM_REGISTRY) $(LIB_OBJ) $(TESTSUITE_SYSTEM_OBJ) $(TARGET)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(LOGGER) -I$(TESTING) -I$(BUILD)/inc/system \
		-D_POSIX_C_SOURCE=200809L \
		-DFRAMEDSL_BIN='"$(TARGET)"' -DEXAMPLES_DIR='"examples"' \
		$(SYSTEM_SRC) $(LIB_OBJ) $(TESTSUITE_SYSTEM_OBJ) -o $@

test: test-unit test-system

test-unit: $(UNIT_RUNNER)
	@TESTSUITE_OUTPUT_DIR=$(BUILD)/tests/unit ./$(UNIT_RUNNER)

test-system: $(SYSTEM_RUNNER)
	@TESTSUITE_OUTPUT_DIR=$(BUILD)/tests/system ./$(SYSTEM_RUNNER)

clean:
	rm -rf $(BUILD)
