# =====================================================================
#  FunnyOS -- the 8086 subsystem
#
#  These targets build and run on the HOST, not on FunnyOS. The
#  interpreter knows nothing about the operating system it lives in: give
#  it memory and some bytes and it executes them. So its tests need no VM,
#  no boot and no QEMU, and run in milliseconds.
#
#  See dos/tests/harness.h for the reasoning and for how to add a test.
#
#  Targets:
#    make test           build and run every suite
#    make test-alu       one suite
#    make list           which suites exist
#    make clean
# =====================================================================

CC    ?= gcc

# Keyed on the directory the sources live in, so that a session working
# in its own git worktree compiles into its own output directory instead
# of overwriting the binaries of a session working in another one.
#
# This matters because several people build at once here. A shared output
# path means make can find a binary that is newer than the sources in
# front of it, decide there is nothing to do, and run a test suite built
# from somebody else's tree -- a result that means nothing and looks
# exactly like a passing one. The alternative, rebuilding everything on
# every invocation, is what a shared path degrades to anyway once the
# timestamps stop lining up.
#
# So: main checkout here gets .../funyos-build/FunnyOS/dos-tests, and a
# worktree at .claude/worktrees/m3-c-str-task-188236 gets
# .../funyos-build/m3-c-str-task-188236/dos-tests.
BUILD ?= /var/tmp/funyos-build/$(notdir $(patsubst %/dos,%,$(CURDIR)))/dos-tests

# -Werror is on here and not in the kernel's build, deliberately. The
# kernel has history; this directory is new and has none, so there is
# nothing to grandfather in and no reason to let the first warning
# through. A subsystem written by several people at once lives or dies on
# this: a warning nobody owns is a warning nobody reads.
#
# -Wno-unused-parameter is not here on purpose either. A handler that does
# not use its opcode must say so with (void)opcode, and the warning is
# what reminds it to.
#
# -fno-builtin keeps the host compiler from noticing that libk's strstr
# has a different return type from the one the C library declares. The
# interpreter is freestanding, so on its real targets there is no libc
# declaration to disagree with; only this host build has the problem, and
# the honest fix is to tell the compiler not to assume, not to change the
# header that the kernel also uses.
CFLAGS := -std=c17 -g -O2 -Wall -Wextra -Werror -fno-builtin \
          -Iinclude -I../libk/include

# The interpreter itself. Everything here is freestanding: no system
# header is included from dos/cpu or dos/mem, and that is checked by the
# fact that this list also compiles for the user-space build.
#
# The debugger is in it for the same reason. It takes its output as a
# callback rather than calling printf, so it is as freestanding as the
# interpreter it watches and links into the same two builds.
CORE_C := \
    cpu/cpu.c      \
    cpu/decode.c   \
    cpu/step.c     \
    cpu/flags.c    \
    cpu/ops_alu.c  \
    cpu/ops_mov.c  \
    cpu/ops_str.c  \
    cpu/ops_ctl.c  \
    cpu/ops_186.c  \
    dbg/trace.c    \
    mem/mem.c      \
    $(wildcard intr/*.c) \
    $(wildcard bios/*.c)

# libk provides memset and memcpy. Linking it here rather than the host's
# means the tests exercise the same implementation the kernel does.
LIBK_C := ../libk/string.c

HARNESS_C := tests/harness.c

# Every tests/test_*.c is a suite, discovered rather than listed. Adding
# one requires no edit to this file, which matters when several people are
# adding them at once.
SUITES := $(patsubst tests/test_%.c,%,$(wildcard tests/test_*.c))

# --- The corpus suite -------------------------------------------------
#
# Everything else in tests/ is written in C and hands the interpreter a
# handful of bytes. The corpus suite runs whole programs, and a program
# is easier to read -- and to check against a manual -- as assembly than
# as a list of hex bytes. So dos/corpus holds nasm sources, and this is
# what turns them into something a C test can run.
#
# The build assembles each one and then converts it with tools/bin2c.py,
# the same way the top-level Makefile embeds the init program. The point
# of converting rather than reading the .bin at run time is that the
# suite then has no files to find and no path to be told: a test that
# depends on its working directory is a test that fails for a reason
# having nothing to do with the emulator. (python3 comes from the
# top-level build, which already needs it for the same script.)
#
# None of this is linked into the other suites, so a checkout that never
# runs the corpus pays one unused pair of rules.
NASM ?= nasm

CORPUS_SRC   := $(wildcard corpus/*.asm)
CORPUS_BIN   := $(patsubst corpus/%.asm,$(BUILD)/corpus/%.bin,$(CORPUS_SRC))
CORPUS_EMBED := $(patsubst corpus/%.asm,$(BUILD)/corpus/%.c,$(CORPUS_SRC))

# Kept rather than deleted after use.
#
# A file that exists only to satisfy another rule is an intermediate to
# make, and make removes intermediates at the end of a run. Here that
# would mean the assembled binaries -- the exact bytes the suite runs --
# vanish whenever the build finishes, so a failing sample could not be
# disassembled without rebuilding it, and every run would re-assemble the
# whole corpus because the deleted .bin would come back newer than the .c
# built from it.
.SECONDARY: $(CORPUS_BIN)

# nasm is the one tool here that the rest of dos/ does not need, so its
# absence is worth a sentence rather than a shell's "command not found" --
# and worth saying before the first sample rather than after the last.
# The top-level build already requires nasm for the kernel, so this is a
# check for people building this directory on its own.
$(BUILD)/corpus/%.bin: corpus/%.asm
	@mkdir -p $(@D)
	@command -v $(NASM) >/dev/null 2>&1 || { \
	    echo "  !!    $(NASM) is not installed."; \
	    echo "        dos/corpus builds its samples from the .asm files"; \
	    echo "        in corpus/, so the corpus suite cannot be run"; \
	    echo "        without it."; \
	    echo "        On Debian and Ubuntu:  apt-get install nasm"; \
	    exit 1; \
	}
	@echo "  NASM    $<"
	@$(NASM) -f bin $< -o $@

$(BUILD)/corpus/%.c: $(BUILD)/corpus/%.bin ../tools/bin2c.py
	@echo "  EMBED   $@"
	@python3 ../tools/bin2c.py $< corpus_$(basename $(@F)) $@

# The replayer and its header, added to the one suite that uses them.
#
# replay.h is an order-only prerequisite so that editing it rebuilds the
# suite without being handed to the compiler: everything in $^ ends up on
# the command line, and a .h file there is not a thing to compile.
$(BUILD)/test_corpus: $(CORPUS_EMBED) corpus/replay.c | corpus/replay.h

# --- A second corpus: whole programs that talk to the firmware --------
#
# Same machinery, a different directory, and both halves of that matter.
# corpus/bios/ is separate so that the plain corpus suite does not pick
# these up: those samples run on a bare processor, and these need services
# registered and a display behind them, so a suite that ran one without
# the other would fail for a reason having nothing to do with the sample.
#
# The pattern rules above already reach into subdirectories -- make's `%`
# matches across a slash -- so the only new thing here is which suite gets
# the result. Declared once, here, rather than added by whoever writes the
# samples: the same reasoning as the globs in CORE_C, which is that a rule
# five people have to edit is a file five people are editing at once.
BIOS_CORPUS_SRC   := $(wildcard corpus/bios/*.asm)
BIOS_CORPUS_BIN   := $(patsubst corpus/%.asm,$(BUILD)/corpus/%.bin,$(BIOS_CORPUS_SRC))
BIOS_CORPUS_EMBED := $(patsubst corpus/%.asm,$(BUILD)/corpus/%.c,$(BIOS_CORPUS_SRC))

.SECONDARY: $(BIOS_CORPUS_BIN)

# Guarded, because the rule is written before the samples are. An
# unguarded prerequisite that neither exists nor has a rule makes make
# stop parsing the whole file, which would break every other suite for the
# sake of a directory that is still empty.
ifneq ($(strip $(BIOS_CORPUS_SRC)),)
$(BUILD)/test_bios_programs: $(BIOS_CORPUS_EMBED) \
                            $(wildcard corpus/bios/replay.c) \
                            | $(wildcard corpus/bios/replay.h)
endif

.PHONY: all test list clean $(addprefix test-,$(SUITES))

all: test

$(BUILD)/test_%: tests/test_%.c $(HARNESS_C) $(CORE_C) $(LIBK_C)
	@mkdir -p $(@D)
	@echo "  CCh     $@"
	@$(CC) $(CFLAGS) -o $@ $^

# Run every suite, and report at the end rather than at the first failure:
# one run should tell you everything that is wrong.
test: $(addprefix $(BUILD)/test_,$(SUITES))
	@echo
	@fail=0; \
	for suite in $(SUITES); do \
	    $(BUILD)/test_$$suite || fail=1; \
	done; \
	echo; \
	if [ $$fail -eq 0 ]; then \
	    echo "====> 8086 tests PASSED"; \
	else \
	    echo "====> 8086 tests FAILED"; \
	    exit 1; \
	fi

# A static pattern rule rather than a plain `test-%:` one.
#
# The plain form looks right and silently does nothing: make skips the
# implicit rule search for a phony target, and every suite name is phony
# here, so `make test-alu` printed "Nothing to be done for 'test-alu'"
# and exited 0. That is the worst shape a broken command can take --
# it is documented in the README as the way to run one suite, and it
# reports success while running nothing.
#
# The static form is expanded at parse time, so it is a real rule and
# .PHONY has nothing to suppress.
$(addprefix test-,$(SUITES)): test-%: $(BUILD)/test_%
	@$<

list:
	@echo "Suites: $(SUITES)"

clean:
	@rm -rf $(BUILD)
	@echo "  Removed $(BUILD)"
