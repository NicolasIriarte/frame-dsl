# Frame-DSL build.
#
#   make              build build/frame-dsl
#   make deps         unpack the vendored CLibs libraries
#   make example      build build/uart_device, the worked example
#   make lib          build build/libframedsl.a, to compile your own program against
#   make test         run both levels of tests
#   make test-unit    unit tests only
#   make test-system  system tests only
#   make format       reformat every first-party C source in place
#   make format-check check formatting without changing anything (what CI runs)
#                     both take CLANG_FORMAT=<binary> when the default one is too old
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

# examples/uart_device.c is the worked example: built against include/ alone and linked with the
# library, exactly as somebody else's program would be. A system test runs it, so it cannot bit-rot
# without a test failing first -- it is an example, and the test is only there to keep it true.
EXAMPLE_UART_DEVICE := $(BUILD)/uart_device
BENCH := $(BUILD)/bench

# The library as one file, so a program outside this repository -- a copy of the example being
# played with, most likely -- compiles with `-Lbuild -lframedsl` instead of naming object files.
LIB_ARCHIVE := $(BUILD)/libframedsl.a
AR ?= ar

# One per level, not one shared. testsuite.c sizes its user-story array from
# TESTSUITE_USER_STORY_COUNT, which it picks up from whichever testsuite_registry.h is on its include
# path when it is compiled -- so the count is baked into the object. A single object built without a
# registry sizes the array for zero stories and halts on the second one registered.
TESTSUITE_UNIT_OBJ := $(BUILD)/testsuite_unit.o
TESTSUITE_SYSTEM_OBJ := $(BUILD)/testsuite_system.o

COMPILE_COMMANDS := compile_commands.json

# Every first-party C source. Not third_party/ (not ours to reformat) and not build/ (generated).
FORMAT_SRC := $(wildcard src/*.c src/*.h include/*.h test/unit/*.c test/system/*.c examples/*.c bench/*.c)

# .clang-format uses keys that arrived in clang-format 21 (BreakAfterOpenBracketFunction,
# BreakBeforeCloseBracketFunction). An older binary does not format differently -- it refuses to read
# the style file at all, once per source, and says "unknown key" rather than anything about versions.
# Overridable because the version that is simply `clang-format` differs per distribution: Debian
# bookworm and the CI image both ship 14, and there the binary to name is clang-format-22.
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_MIN := 21

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
	-DCORPUS_DIR=\"test/corpus\" -DUART_DEVICE_BIN=\"$(EXAMPLE_UART_DEVICE)\"

.PHONY: all deps test test-unit test-system test-editors compile-commands format format-check \
	format-tool hooks clean bench

# Where the editor support lives, and the Emacs invocation that tests it. -Q so that whatever the
# person running this has in their own configuration cannot make the mode look like it works.
EDITORS := editors
EMACS ?= emacs
EMACS_TEST_FLAGS := -Q --batch -L $(EDITORS) -l $(EDITORS)/frame-dsl-mode-tests.el \
	-f ert-run-tests-batch-and-exit

# compile_commands.json is built by default so a fresh clone is browsable after `make`, with no
# second step for somebody to discover. hooks is here for the same reason: a fresh clone's first
# build wires up the pre-commit hook, with nothing separate to remember.
all: $(TARGET) $(COMPILE_COMMANDS) hooks

deps: $(DEPS)

example: $(EXAMPLE_UART_DEVICE)

lib: $(LIB_ARCHIVE)

# Not part of `test`, and nothing in it fails on a slow result: timing on a shared machine is too noisy
# to fail a build on. BENCH_SECONDS is the minimum each measurement runs for.
BENCH_SECONDS ?= 0.25
bench: $(BENCH)
	$(BENCH) $(BENCH_SECONDS)

# Points git at .githooks/ so the pre-commit hook runs without anyone installing it by hand. Silent,
# and silently skipped outside a git checkout -- e.g. building from a released tarball, which has no
# .git to configure.
hooks:
	@git rev-parse --git-dir >/dev/null 2>&1 && git config core.hooksPath .githooks || true

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

$(LIB_ARCHIVE): $(LIB_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

# No -Isrc, same as SYSTEM_FLAGS below: the example is a user of the library, not part of it. It
# links the archive rather than the objects for the same reason -- that is the line someone copying
# this file out of the repository will write.
$(EXAMPLE_UART_DEVICE): examples/uart_device.c $(LIB_ARCHIVE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Iinclude examples/uart_device.c -L$(BUILD) -lframedsl -o $@

# Built like the example: a user of the library, against the public header and the archive. -O2 comes
# from CFLAGS, as it would for anybody building their own program.
$(BENCH): bench/bench.c $(LIB_ARCHIVE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -Iinclude bench/bench.c -L$(BUILD) -lframedsl -o $@

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
                  $(EXAMPLE_UART_DEVICE) $(COMPILE_COMMANDS)
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

# Checked once, before either recipe below runs, so that "your clang-format is too old" is said once
# and in those words -- instead of the same unknown-key complaint about .clang-format repeated for
# every file, which is what the tool itself produces and which names no version at all.
format-tool:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { \
		echo "$(CLANG_FORMAT): not on PATH. Install clang-format $(CLANG_FORMAT_MIN) or newer," >&2; \
		echo "or point this at one: make format CLANG_FORMAT=clang-format-22" >&2; \
		exit 1; \
	}
	@major=$$($(CLANG_FORMAT) --version | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p'); \
	if [ -z "$$major" ] || [ "$$major" -lt $(CLANG_FORMAT_MIN) ]; then \
		echo "$(CLANG_FORMAT) is $$($(CLANG_FORMAT) --version), and .clang-format needs" >&2; \
		echo "$(CLANG_FORMAT_MIN) or newer to read it at all. Install a newer one and name it:" >&2; \
		echo "  make format CLANG_FORMAT=clang-format-22" >&2; \
		exit 1; \
	fi

# Formats every first-party source in place. What a contributor runs by hand; the pre-commit hook
# and CI both drive clang-format themselves rather than calling this, so that a missing tool on
# either path is diagnosed there instead of as a generic make failure here.
format: format-tool
	$(CLANG_FORMAT) -i $(FORMAT_SRC)

# Non-mutating: exits non-zero if formatting `format` would still change something, without
# changing it. This is what CI runs, at the top of the job, before the build.
format-check: format-tool
	@fail=0; \
	for f in $(FORMAT_SRC); do \
		$(CLANG_FORMAT) --dry-run --Werror "$$f" || fail=1; \
	done; \
	exit $$fail

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
