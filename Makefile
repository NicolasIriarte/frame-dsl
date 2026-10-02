# Frame-DSL build.
#
#   make              build build/frame-dsl
#   make deps         unpack the vendored CLibs libraries
#   make example      build build/uart_device, the worked example
#   make lib          build build/libframedsl.a and build/libframedsl.so, to compile your own program against
#   make python       copy the shared library into python/framedsl/, for `pip install ./python`
#   make test-python  the Python bindings' tests; skipped without python3
#   make test         run both levels of tests
#   make test-unit    unit tests only
#   make test-system  system tests only
#   make format       reformat every first-party C source in place
#   make format-check check formatting without changing anything (what CI runs)
#                     both take CLANG_FORMAT=<binary> when the default one is too old
#   make compile-commands  regenerate compile_commands.json alone
#   make bench        time a dozen operations once each, in seconds
#   make baseline     the performance baseline (#102): minutes, by hand; see docs/performance.md
#   make baseline-quick    the same sweep reduced to a few points, in seconds
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
CFLAGS ?= -std=c11 -Wall -Wunused -Wextra -Werror -O2

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
BASELINE := $(BUILD)/baseline

# Fuzzing (#59). The targets are fuzz/fuzz_*.c. `make fuzz` runs them under libFuzzer, which needs
# clang; the replay binaries run every saved input through the same targets with the ordinary compiler,
# and are part of `make test`, so a regression input keeps being checked on every machine and in CI.
FUZZ_TARGETS := decode load format json
FUZZ_CC ?= clang
# Extra flags for FUZZ_CC. On a machine where clang picks a gcc installation without libstdc++'s
# development files, libFuzzer's C++ runtime will not link; point it at one that has them, e.g.
#   make fuzz FUZZ_CC=clang-18 FUZZ_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13
FUZZ_FLAGS ?=
FUZZ_SECONDS ?= 60
# libFuzzer's own per-input timeout defaults to 1200s, entirely separate from -max_total_time above: if
# one input hangs the target, that one execution blocks for up to 20 minutes before -max_total_time is
# ever checked again, since the check only runs between completed executions. Neither the decoder nor
# the loader has any reason to take longer than this on any input, so a run that does is a hang, not a
# slow machine.
FUZZ_TIMEOUT ?= 5
FUZZ_BINS := $(FUZZ_TARGETS:%=$(BUILD)/fuzz/%)
REPLAY_BINS := $(FUZZ_TARGETS:%=$(BUILD)/replay/%)
# The most lines one sanitized run may print before it is stopped and failed (#114, see
# scripts/cap_output.sh). A replay prints one line when it passes and a few hundred at most for a
# sanitizer report; libFuzzer prints a line for each input that finds something new, so a long `make
# fuzz` is given far more room. Neither comes anywhere near what the #80 loop printed each second.
REPLAY_OUTPUT_LINES ?= 10000
FUZZ_OUTPUT_LINES ?= 100000
# Non-PIE, because ASan in GCC 12 and clang 14 -- what CI's image has -- fails at random on a kernel
# with vm.mmap_rnd_bits = 32, which CI's has: wherever ASLR places a position-independent executable,
# the shadow memory can collide with it, and the program prints AddressSanitizer:DEADLYSIGNAL in a
# loop without ever reaching main (#80). About one run in five did, of any sanitized binary however
# trivial. A fixed load address takes the collision away; newer runtimes avoid it on their own.
SANITIZE_NO_PIE := -fno-pie -no-pie
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	$(SANITIZE_NO_PIE)

# The library as one file, so a program outside this repository -- a copy of the example being
# played with, most likely -- compiles with `-Lbuild -l:libframedsl.a` instead of naming object files.
# The `:` form names the archive: build/ also holds libframedsl.so (#98), and a plain -lframedsl prefers
# the shared object, leaving a program that will not start without LD_LIBRARY_PATH.
LIB_ARCHIVE := $(BUILD)/libframedsl.a
AR ?= ar

# The same library as a shared object (#98), which is what the Python bindings load through ctypes. Its
# objects are compiled a second time with -fPIC into build/pic/ rather than giving every object -fPIC:
# the tool, the tests and the benchmark keep linking exactly the code they linked before, so a timing
# taken through the archive is not moved by a change made for somebody else's loader.
#
# Versioned the usual way: the file is libframedsl.so.<version>, its soname is libframedsl.so.<major>,
# and libframedsl.so points at that for a linker's -lframedsl. Only the framedsl_* functions are exported
# (src/libframedsl.map); everything else in the library is internal and stays out of the symbol table.
PIC_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/pic/%.o)
DEPFILES += $(PIC_OBJ:.o=.d)
SO_MAJOR := 0
SO_VERSION := $(SO_MAJOR).1.0
LIB_SONAME := libframedsl.so.$(SO_MAJOR)
LIB_SHARED := $(BUILD)/libframedsl.so.$(SO_VERSION)
LIB_SHARED_LINKS := $(BUILD)/$(LIB_SONAME) $(BUILD)/libframedsl.so
LIB_EXPORTS := src/libframedsl.map

# The Python bindings (#98). A pure-Python package, python/framedsl/, over the shared library above.
PYTHON ?= python3
PYTHON_PACKAGE := python/framedsl

# One per level, not one shared. testsuite.c sizes its user-story array from
# TESTSUITE_USER_STORY_COUNT, which it picks up from whichever testsuite_registry.h is on its include
# path when it is compiled -- so the count is baked into the object. A single object built without a
# registry sizes the array for zero stories and halts on the second one registered.
TESTSUITE_UNIT_OBJ := $(BUILD)/testsuite_unit.o
TESTSUITE_SYSTEM_OBJ := $(BUILD)/testsuite_system.o

COMPILE_COMMANDS := compile_commands.json

# Every first-party C source. Not third_party/ (not ours to reformat) and not build/ (generated).
FORMAT_SRC := $(wildcard src/*.c src/*.h include/*.h test/unit/*.c test/system/*.c examples/*.c bench/*.c fuzz/*.c)
# Definition files, formatted by the tool itself (#29) rather than clang-format. fuzz/ is left out: its
# corpora are inputs, kept exactly as found.
FDSL_FORMAT_SRC := $(wildcard examples/*.fdsl examples/*/*.fdsl test/corpus/*.fdsl)

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

.PHONY: \
	all \
	baseline \
	baseline-quick \
	bench \
	changelog \
	changelog-preview \
	clean \
	compile-commands \
	conflicts \
	deps \
	dist \
	format \
	format-check \
	format-tool \
	fuzz \
	hooks \
	install \
	lib \
	python \
	test \
	test-changelog \
	test-completion \
	test-dist \
	test-editors \
	test-fuzz-replay \
	test-output-cap \
	test-python \
	test-system \
	test-unit \
	uninstall \
	vsix \
	FORCE

# Where the editor support lives, and the invocations that test it. -Q / -u NONE -N so that whatever
# the person running this has in their own configuration cannot make an editor look like it works.
EDITORS := editors
EMACS ?= emacs
EMACS_TEST_FLAGS := -Q --batch -L $(EDITORS)/emacs -l $(EDITORS)/emacs/frame-dsl-mode-tests.el \
	--eval '(setq frame-dsl-program "$(abspath $(TARGET))")' -f ert-run-tests-batch-and-exit

VIM ?= vim
NVIM ?= nvim
# PATH, not the -program equivalents above: the Vim runtime files have no config knob for it, and
# find frame-dsl the way an editor started from a shell does -- so $(TARGET)'s directory goes first.
VIM_TEST_CMD = PATH="$(abspath $(BUILD)):$$PATH" $(1) -u NONE -N -es --cmd 'set rtp^=$(EDITORS)/vim' \
	-S $(EDITORS)/vim/frame-dsl-tests.vim

# Where `make install` puts the tool. Every editor integration starts by finding `frame-dsl` on the
# PATH, so ~/.local is the usual answer for one person: make install PREFIX=~/.local
PREFIX ?= /usr/local
# Where bash-completion and zsh look for a command's completion (#85): bash-completion loads
# share/bash-completion/completions/<command> on first <TAB>, for any PREFIX it knows of (it also searches
# $XDG_DATA_HOME, so ~/.local works); zsh has share/zsh/site-functions on its $fpath for /usr and
# /usr/local, and needs it added for any other PREFIX -- see completions/_frame-dsl.
BASH_COMPLETION_DIR ?= $(PREFIX)/share/bash-completion/completions
ZSH_COMPLETION_DIR ?= $(PREFIX)/share/zsh/site-functions

# compile_commands.json is built by default so a fresh clone is browsable after `make`, with no
# second step for somebody to discover. hooks is here for the same reason: a fresh clone's first
# build wires up the pre-commit hook, with nothing separate to remember.
all: $(TARGET) $(COMPILE_COMMANDS) hooks

deps: $(DEPS)

example: $(EXAMPLE_UART_DEVICE)

lib: $(LIB_ARCHIVE) $(LIB_SHARED_LINKS)

# A copy next to the package is the second place python/framedsl/_native.py looks, after
# FRAMEDSL_LIBRARY, so `pip install ./python` after this carries the library into the installed package.
# The copy is gitignored: it is build output that happens to live in the source tree.
python: $(LIB_SHARED_LINKS)
	cp $(BUILD)/$(LIB_SONAME) $(PYTHON_PACKAGE)/$(LIB_SONAME)

# The VS Code extension, packaged with zip alone -- see scripts/build_vsix.sh. The version comes
# from editors/vscode/package.json, not this Makefile: there is exactly one place that number is
# written. Depends on the vendored client rather than building it, since producing that needs npm
# and nothing else here does; scripts/vendor_vscode_languageclient.sh is run by hand instead.
VSIX_VERSION = $(shell jq -r .version editors/vscode/package.json)
VSIX := $(BUILD)/frame-dsl-$(VSIX_VERSION).vsix
VSCODE_SRC := editors/vscode/package.json editors/vscode/language-configuration.json \
	editors/vscode/extension.js editors/vscode/syntaxes/fdsl.tmLanguage.json \
	editors/vscode/vendor/vscode-languageclient.js

vsix: $(VSIX)

$(VSIX): $(VSCODE_SRC) scripts/build_vsix.sh
	scripts/build_vsix.sh $@

# The tool, its completions, and the shared library with its header (#98) -- the library so that the
# Python bindings find it through the system loader with nothing set. The static archive is not
# installed: a program linking it statically is built against the build tree. Run ldconfig afterwards
# when PREFIX is a system directory; a PREFIX the loader does not search needs LD_LIBRARY_PATH or
# FRAMEDSL_LIBRARY instead.
install: $(TARGET) $(LIB_SHARED_LINKS)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/frame-dsl
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 755 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/
	ln -sf libframedsl.so.$(SO_VERSION) $(DESTDIR)$(PREFIX)/lib/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(DESTDIR)$(PREFIX)/lib/libframedsl.so
	install -m 644 include/framedsl.h $(DESTDIR)$(PREFIX)/include/framedsl.h
	install -d $(DESTDIR)$(BASH_COMPLETION_DIR) $(DESTDIR)$(ZSH_COMPLETION_DIR)
	install -m 644 completions/frame-dsl.bash $(DESTDIR)$(BASH_COMPLETION_DIR)/frame-dsl
	install -m 644 completions/_frame-dsl $(DESTDIR)$(ZSH_COMPLETION_DIR)/_frame-dsl

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/frame-dsl
	rm -f $(DESTDIR)$(PREFIX)/lib/libframedsl.so.$(SO_VERSION) $(DESTDIR)$(PREFIX)/lib/$(LIB_SONAME) \
	  $(DESTDIR)$(PREFIX)/lib/libframedsl.so $(DESTDIR)$(PREFIX)/include/framedsl.h
	rm -f $(DESTDIR)$(BASH_COMPLETION_DIR)/frame-dsl $(DESTDIR)$(ZSH_COMPLETION_DIR)/_frame-dsl

# Not part of `test`, and nothing in it fails on a slow result: timing on a shared machine is too noisy
# to fail a build on. BENCH_SECONDS is the minimum each measurement runs for.
BENCH_SECONDS ?= 0.25
bench: $(BENCH)
	$(BENCH) $(BENCH_SECONDS)

# The performance baseline (#102). Not part of `test`, and not run in CI, for the same reason as bench;
# CI builds it so that it keeps compiling. The results are named for the date and the commit, and go to
# build/performance/ -- copy a run worth keeping into docs/performance/ and commit it there.
# BASELINE_ARGS passes anything else through: --cpu, --samples, --only, ... (build/baseline --help).
BASELINE_DIR := $(BUILD)/performance
# Recursive (=), not :=, because VERSION_STRING is defined further down.
BASELINE_NAME = $(shell date +%Y-%m-%d)-$(VERSION_STRING)
BASELINE_ARGS ?=
baseline: $(BASELINE)
	@mkdir -p $(BASELINE_DIR)
	$(BASELINE) --commit $(VERSION_STRING) --out $(BASELINE_DIR)/$(BASELINE_NAME).csv $(BASELINE_ARGS)

baseline-quick: $(BASELINE)
	@mkdir -p $(BASELINE_DIR)
	$(BASELINE) --quick --commit $(VERSION_STRING) --out $(BASELINE_DIR)/$(BASELINE_NAME)-quick.csv $(BASELINE_ARGS)

# Each target for FUZZ_SECONDS, starting from a working copy of its seed corpus -- libFuzzer adds to the
# corpus it is given, and what it finds interesting belongs in build/, not in the repository. A crash
# is written to build/fuzz/ as crash-<hash>; copy it into fuzz/regressions/<target>/ once fixed, and
# `make test` replays it from then on.
#
# fuzz/regressions/<target>/ is created if missing: git does not track an empty directory, so a target
# with no regression yet -- decode, so far -- would not exist on a fresh clone, and both libFuzzer and
# replay (fuzz/replay.c) refuse to open a corpus directory that is not there at all.
fuzz: $(FUZZ_BINS)
	@for target in $(FUZZ_TARGETS); do \
	  mkdir -p $(BUILD)/fuzz/corpus/$$target fuzz/regressions/$$target && \
	    cp -n fuzz/corpus/$$target/* $(BUILD)/fuzz/corpus/$$target/ ; \
	  echo "fuzz: $$target for $(FUZZ_SECONDS)s"; \
	  scripts/cap_output.sh $(FUZZ_OUTPUT_LINES) $(BUILD)/fuzz/$$target -max_total_time=$(FUZZ_SECONDS) \
	    -timeout=$(FUZZ_TIMEOUT) -print_final_stats=1 -artifact_prefix=$(BUILD)/fuzz/ \
	    $(BUILD)/fuzz/corpus/$$target fuzz/regressions/$$target || exit 1; \
	done

# Every seed and every past crash, through each target, under the sanitizers. Fast: a few hundred
# inputs, run once each.
#
# Wrapped in `timeout` on top of replay.c's own alarm(): the alarm is what usually fires and says which
# input hung, but it is one process's signal handler, and a CI runner once saw it not fire at all.
# That turned out to be ASan faulting in a loop before main was reached, so before the alarm was ever
# set (#80, see SANITIZE_NO_PIE above). `timeout` is enforced by the kernel from outside the process,
# so a failure that never gets as far as the alarm still ends the build instead of hanging it.
#
# And the output is capped, because that same failure printed AddressSanitizer:DEADLYSIGNAL about
# 100,000 times a second for as long as the timeout allowed: 12 million lines inside these 120 seconds,
# and 47 million with it raised to 600 (#114). The cap stops it within a fraction of a second.
REPLAY_TIMEOUT_SECONDS ?= 120
test-fuzz-replay: $(REPLAY_BINS)
	@for target in $(FUZZ_TARGETS); do \
	  mkdir -p fuzz/regressions/$$target; \
	  printf "fuzz replay: %s: " $$target; \
	  scripts/cap_output.sh $(REPLAY_OUTPUT_LINES) timeout $(REPLAY_TIMEOUT_SECONDS) $(BUILD)/replay/$$target \
	    fuzz/corpus/$$target fuzz/regressions/$$target || exit 1; \
	done

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

# What `frame-dsl --version` prints: `git describe` of this checkout, or "unknown" when there is no
# git to ask. The stamp is rewritten only when that changes, so a new commit rebuilds main.o and
# nothing else, and a build with nothing new rebuilds nothing.
VERSION_STRING := $(shell git -C $(CURDIR) describe --always --dirty 2>/dev/null || echo unknown)
VERSION_STAMP := $(BUILD)/version.stamp

$(VERSION_STAMP): FORCE
	@mkdir -p $(@D)
	@echo '$(VERSION_STRING)' | cmp -s - $@ || echo '$(VERSION_STRING)' > $@

$(MAIN_OBJ): $(VERSION_STAMP)
$(MAIN_OBJ): SRC_FLAGS += -DFRAMEDSL_VERSION=\"$(VERSION_STRING)\"

# A release's one archive (#128): the tool, the library and its header, the completions and every
# editor's support -- the .vsix included -- under one directory named after the version. The release job
# attaches it; scripts/build_dist.sh says what is in it and why. VERSION_STRING is the tag on a tag's
# checkout, and the release job passes it explicitly as well.
DIST_ARCH := $(shell uname -m)
DIST := $(BUILD)/frame-dsl-$(VERSION_STRING)-linux-$(DIST_ARCH).tar.gz

dist: $(DIST)

$(DIST): $(TARGET) $(LIB_ARCHIVE) $(LIB_SHARED_LINKS) $(VSIX) scripts/build_dist.sh scripts/dist_install.md \
         CHANGELOG.md README.md $(wildcard manual/*.md editors/*/README.md editors/emacs/*.el) \
         $(shell find editors/vim -type f 2>/dev/null)
	scripts/build_dist.sh $@ $(BUILD) $(VSIX)

# Unpacks the archive somewhere else and uses it: every promised file, the tool, both libraries. Not part
# of `test`, since it needs the .vsix and so jq and zip; CI runs it on every push, as it builds the .vsix.
test-dist: $(DIST)
	scripts/test_dist.sh $(DIST) $(VERSION_STRING)

FORCE:

$(TARGET): $(MAIN_OBJ) $(LIB_OBJ)
	@mkdir -p $(@D)
	$(CC) $(SRC_FLAGS) $^ -o $@

$(LIB_ARCHIVE): $(LIB_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(BUILD)/pic/%.o: src/%.c | $(DEPS)
	@mkdir -p $(@D)
	$(CC) $(SRC_FLAGS) -fPIC -MMD -MP -c $< -o $@

# --no-undefined so that a library needing something it does not link fails here, rather than in the
# first program to dlopen() it. -lm because the calibrations include <math.h>: at -O2 the compiler
# inlines what they use, and a build at another level may call into libm instead.
$(LIB_SHARED): $(PIC_OBJ) $(LIB_EXPORTS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(LIB_SONAME) -Wl,--version-script=$(LIB_EXPORTS) -Wl,--no-undefined \
	  $(PIC_OBJ) -lm -o $@

# Grouped (&:), so one recipe makes both links rather than running once per link.
$(LIB_SHARED_LINKS) &: $(LIB_SHARED)
	ln -sf libframedsl.so.$(SO_VERSION) $(BUILD)/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(BUILD)/libframedsl.so

# No -Isrc, same as SYSTEM_FLAGS below: the example is a user of the library, not part of it. It
# links the archive rather than the objects for the same reason -- that is the line someone copying
# this file out of the repository will write.
$(EXAMPLE_UART_DEVICE): examples/uart_device.c $(LIB_ARCHIVE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Iinclude examples/uart_device.c -L$(BUILD) -l:libframedsl.a -o $@

# Built like the example: a user of the library, against the public header and the archive. -O2 comes
# from CFLAGS, as it would for anybody building their own program.
$(BENCH): bench/bench.c $(LIB_ARCHIVE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -Iinclude bench/bench.c -L$(BUILD) -l:libframedsl.a -o $@

# Built like the benchmark, against the archive by name (`-l:`) so that no shared library sitting beside
# it in build/ can be linked instead. The compiler and flags are compiled in, so the results say what
# built them.
$(BASELINE): bench/baseline.c $(LIB_ARCHIVE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Iinclude '-DBASELINE_CC="$(CC)"' '-DBASELINE_CFLAGS="$(CFLAGS)"' bench/baseline.c \
	  -L$(BUILD) -l:libframedsl.a -o $@

# The library is compiled into each fuzz binary from source rather than linked from the archive: the
# sanitizers and libFuzzer's coverage instrumentation have to reach inside it to see anything.
$(BUILD)/fuzz/%: fuzz/fuzz_%.c $(LIB_SRC) $(wildcard src/*.h) include/framedsl.h
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_FLAGS) -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
	  $(SANITIZE_NO_PIE) -D_DEFAULT_SOURCE -Iinclude -Isrc $< $(LIB_SRC) -o $@

$(BUILD)/replay/%: fuzz/fuzz_%.c fuzz/replay.c $(LIB_SRC) $(wildcard src/*.h) include/framedsl.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -g -O1 $(SANITIZE) -D_DEFAULT_SOURCE -Iinclude -Isrc $< fuzz/replay.c $(LIB_SRC) -o $@

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

test: test-unit test-system test-editors test-fuzz-replay test-changelog test-completion test-python \
	test-output-cap

# The Python bindings' tests (#98), through python3's own unittest: no pip, no network, nothing to install.
# FRAMEDSL_LIBRARY points them at this build's library rather than whatever the system has, or a copy
# `make python` left in the package. Skipped, and said so, without python3.
test-python: $(LIB_SHARED_LINKS)
	@if ! command -v $(PYTHON) >/dev/null 2>&1; then \
		echo "no $(PYTHON) -- skipping the Python bindings' tests"; \
	else \
		FRAMEDSL_LIBRARY="$(abspath $(BUILD)/$(LIB_SONAME))" PYTHONPATH="$(abspath python)" \
		  $(PYTHON) -m unittest discover -s python/tests -t python -v; \
	fi

test-unit: $(UNIT_RUNNER)
	@TESTSUITE_OUTPUT_DIR=$(BUILD)/tests/unit ./$(UNIT_RUNNER)

test-system: $(SYSTEM_RUNNER)
	@TESTSUITE_OUTPUT_DIR=$(BUILD)/tests/system ./$(SYSTEM_RUNNER)

# The Emacs, Vim and Neovim halves of the editor support. Each is skipped, loudly, when that editor
# is not installed: this project builds and tests with a C compiler, make and tar, and a highlighter
# is not a reason to add a fourth thing somebody has to install to run the suite. What the skips give
# up is covered without any editor by test/unit/9_editor_support.c, which compares the word lists
# against the compiler's.
test-editors: $(TARGET)
	@if command -v $(EMACS) >/dev/null 2>&1; then \
		$(EMACS) $(EMACS_TEST_FLAGS); \
	else \
		echo "test-editors: no $(EMACS) on PATH, skipping editors/emacs/frame-dsl-mode.el tests"; \
	fi
	@if command -v $(VIM) >/dev/null 2>&1; then \
		$(call VIM_TEST_CMD,$(VIM)); \
	else \
		echo "test-editors: no $(VIM) on PATH, skipping editors/vim tests"; \
	fi
	@if command -v $(NVIM) >/dev/null 2>&1; then \
		$(call VIM_TEST_CMD,$(NVIM)); \
	else \
		echo "test-editors: no $(NVIM) on PATH, skipping editors/vim tests under Neovim"; \
	fi

# The Bash and zsh completions (#85), driven without a terminal against the tool just built. The zsh half
# skips, loudly, without zsh. Then the install: both scripts land where their shells look, and
# uninstall takes them away again.
test-completion: $(TARGET)
	@completions/test_completion.sh $(TARGET)
	@staging=$$(mktemp -d) && trap 'rm -rf "$$staging"' EXIT && \
	$(MAKE) -s install DESTDIR=$$staging PREFIX=/usr >/dev/null && \
	test -f $$staging/usr/share/bash-completion/completions/frame-dsl && \
	test -f $$staging/usr/share/zsh/site-functions/_frame-dsl && \
	$(MAKE) -s uninstall DESTDIR=$$staging PREFIX=/usr >/dev/null && \
	test -z "$$(find $$staging -type f)" && \
	echo "  ok   make install puts both completions where their shells look; make uninstall removes them" \
	|| { echo "  FAIL make install / make uninstall of the completions" >&2; exit 1; }

# The changelog is written as one fragment per entry under changelog.d/ and assembled when a version is
# cut (#90); see changelog.d/README.md. These check the scripts that do that, and the conflict-marker
# guard CI runs, in throwaway directories -- nothing here touches the real CHANGELOG.md.
test-changelog:
	@scripts/test_changelog.sh
	@scripts/test_conflict_markers.sh

# The cap every sanitized run's output goes through (#114), made to pass something well-behaved and to
# stop a runaway.
test-output-cap:
	@scripts/test_cap_output.sh

# What the next release's section would read like, from changelog.d/ and whatever is still under
# [Unreleased]. Changes nothing.
changelog-preview:
	@scripts/changelog_assemble.sh $(or $(VERSION),vNEXT) --preview

# Folds changelog.d/ into CHANGELOG.md as VERSION's section and removes the fragments it used: the first
# task of the release tag issue (.forgejo/issue_template/tag.md).
changelog:
	@test -n "$(VERSION)" || { echo "usage: make changelog VERSION=vX.Y.Z" >&2; exit 1; }
	scripts/changelog_assemble.sh $(VERSION)

# Fails if any tracked file still holds a merge conflict marker. CI runs it on every push.
conflicts:
	@scripts/check_conflict_markers.sh

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
format: format-tool $(TARGET)
	$(CLANG_FORMAT) -i $(FORMAT_SRC)
	$(TARGET) $(FDSL_FORMAT_SRC) --format

# Non-mutating: exits non-zero if formatting `format` would still change something, without
# changing it. This is what CI runs, at the top of the job, before the build.
format-check: format-tool $(TARGET)
	@fail=0; \
	for f in $(FORMAT_SRC); do \
		$(CLANG_FORMAT) --dry-run --Werror "$$f" || fail=1; \
	done; \
	$(TARGET) $(FDSL_FORMAT_SRC) --format --check || fail=1; \
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
