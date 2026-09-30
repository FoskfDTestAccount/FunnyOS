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
    mem/mem.c

# libk provides memset and memcpy. Linking it here rather than the host's
# means the tests exercise the same implementation the kernel does.
LIBK_C := ../libk/string.c

HARNESS_C := tests/harness.c

# Every tests/test_*.c is a suite, discovered rather than listed. Adding
# one requires no edit to this file, which matters when several people are
# adding them at once.
SUITES := $(patsubst tests/test_%.c,%,$(wildcard tests/test_*.c))

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
