# Frame-DSL build.
#
#   make              build build/frame-dsl
#   make deps         unpack the vendored CLibs libraries
#   make test         run both levels of tests
#   make test-unit    unit tests only
#   make test-system  system tests only
#   make compile-commands  regenerate compile_commands.json alone
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
MAIN_OBJ := $(BUILD)/obj/main.o

# One .d per object, written by the compiler and read back at the bottom of this file.
DEPFILES := $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

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

COMPILE_COMMANDS := compile_commands.json

# The three ways this project compiles a C file, defined once. The rules below use these, and so does
# the compile_commands.json generator -- which is the whole reason they are variables. A generator
# that restated the flags would be wrong the first time one of these rules was edited, and wrong
# silently: the symptom is a confused language server, not a failed build.
#
# SYSTEM_FLAGS is the one to look at. It has no -Isrc, which is what stops a system test reaching
# inside, and it carries defines whose values are quoted strings. Those are backslash-escaped rather
# than wrapped in single quotes, so the quotes survive every step between here and the generator:
# make's expansion, the recipe's shell, an environment variable, and the word-splitting that turns
# the lot back into arguments. Nesting single quotes inside single quotes loses them silently, and
# the symptom is a macro expanding to a bare identifier rather than to a string.
SRC_FLAGS := $(CFLAGS) -I$(LOGGER) -Iinclude -Isrc
UNIT_FLAGS := $(CFLAGS) -I$(LOGGER) -I$(TESTING) -I$(BUILD)/inc/unit -Iinclude -Isrc \
	-DSOURCE_DIR=\"src\" -DEDITORS_DIR=\"editors\"
SYSTEM_FLAGS := $(CFLAGS) -I$(LOGGER) -I$(TESTING) -I$(BUILD)/inc/system -Iinclude \
	-D_POSIX_C_SOURCE=200809L -DFRAMEDSL_BIN=\"$(TARGET)\" -DEXAMPLES_DIR=\"examples\" \
	-DCORPUS_DIR=\"test/corpus\"

.PHONY: all deps test test-unit test-system test-editors compile-commands clean

# Where the editor support lives, and the Emacs invocation that tests it. -Q so that whatever the
# person running this has in their own configuration cannot make the mode look like it works.
EDITORS := editors
EMACS ?= emacs
EMACS_TEST_FLAGS := -Q --batch -L $(EDITORS) -l $(EDITORS)/frame-dsl-mode-tests.el \
	-f ert-run-tests-batch-and-exit

# compile_commands.json is built by default so a fresh clone is browsable after `make`, with no
# second step for somebody to discover.
all: $(TARGET) $(COMPILE_COMMANDS)

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

# -MMD -MP writes a .d file next to each .o listing the headers it included, and the -include below
# feeds those back in. Without it, make only watches the .c file: editing a header leaves every object
# that includes it stale, and a struct that changed size is then read through two different layouts in
# one binary. That is not a build that fails, it is a build that corrupts memory, so the dependency
# tracking is not a convenience.
$(BUILD)/obj/%.o: src/%.c | $(DEPS)
	@mkdir -p $(@D)
	$(CC) $(SRC_FLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(MAIN_OBJ) $(LIB_OBJ)
	@mkdir -p $(@D)
	$(CC) $(SRC_FLAGS) $^ -o $@

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

$(UNIT_RUNNER): $(UNIT_SRC) $(UNIT_REGISTRY) $(LIB_OBJ) $(TESTSUITE_UNIT_OBJ) $(wildcard src/*.h)
	@mkdir -p $(@D)
	$(CC) $(UNIT_FLAGS) $(UNIT_SRC) $(LIB_OBJ) $(TESTSUITE_UNIT_OBJ) -o $@

# Note what is missing: -Isrc. A system test gets include/ -- the public header a user would get --
# but not the internals, so a system test that reaches inside fails to build rather than quietly
# testing something no user can reach. FRAMEDSL_BIN and EXAMPLES_DIR are passed in so a test drives the real binary and
# the real example files rather than a copy that can drift.
$(SYSTEM_RUNNER): $(SYSTEM_SRC) $(SYSTEM_REGISTRY) $(LIB_OBJ) $(TESTSUITE_SYSTEM_OBJ) $(TARGET) \
                  $(COMPILE_COMMANDS)
	@mkdir -p $(@D)
	$(CC) $(SYSTEM_FLAGS) $(SYSTEM_SRC) $(LIB_OBJ) $(TESTSUITE_SYSTEM_OBJ) -o $@

test: test-unit test-system test-editors

test-unit: $(UNIT_RUNNER)
	@TESTSUITE_OUTPUT_DIR=$(BUILD)/tests/unit ./$(UNIT_RUNNER)

test-system: $(SYSTEM_RUNNER)
	@TESTSUITE_OUTPUT_DIR=$(BUILD)/tests/system ./$(SYSTEM_RUNNER)

# The Emacs half of the editor support. Skipped, loudly, when Emacs is not installed: this project
# builds and tests with a C compiler, make and tar, and a highlighter is not a reason to add a third
# thing somebody has to install to run the suite. What that skip gives up is covered without Emacs by
# test/unit/9_editor_support.c, which compares the mode's word lists against the compiler's.
test-editors:
	@if command -v $(EMACS) >/dev/null 2>&1; then \
		$(EMACS) $(EMACS_TEST_FLAGS); \
	else \
		echo "test-editors: no $(EMACS) on PATH, skipping editors/frame-dsl-mode.el tests"; \
	fi

compile-commands: $(COMPILE_COMMANDS)

# Depends on the Makefile because that is where the flags live, and on the sources because a file
# nobody has indexed is the same problem as a stale flag. The generated test registries are
# prerequisites too: the recorded flags point a language server at them, and a header that has not
# been generated yet reads as a broken include in an editor even though the build is fine.
#
# The source *directories* are prerequisites as well, and they are not redundant. The file lists come
# from $(wildcard), which is evaluated when the Makefile is read -- so a deleted file simply vanishes
# from the prerequisites and nothing looks out of date, leaving an entry behind for a file that no
# longer exists. A directory's timestamp moves when a file is added or removed from it, which is
# exactly the event the file list cannot report.
$(COMPILE_COMMANDS): Makefile scripts/gen_compile_commands.sh src test/unit test/system \
                     $(LIB_SRC) src/main.c $(UNIT_SRC) \
                     $(SYSTEM_SRC) $(UNIT_REGISTRY) $(SYSTEM_REGISTRY) | $(DEPS)
	@CC="$(CC)" \
	 SRC_FLAGS="$(SRC_FLAGS)" \
	 UNIT_FLAGS="$(UNIT_FLAGS)" \
	 SYSTEM_FLAGS="$(SYSTEM_FLAGS)" \
	 scripts/gen_compile_commands.sh $@
	@echo "wrote $@"

clean:
	rm -rf $(BUILD) $(COMPILE_COMMANDS)

# Last, so nothing above depends on a file that may not exist yet. The '-' makes a missing .d silent:
# on a clean tree there are none, and the objects are about to be built anyway.
-include $(DEPFILES)
