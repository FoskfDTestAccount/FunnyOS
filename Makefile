# =====================================================================
#  FunnyOS build system
#
#  Targets:
#    make            Build the kernel and a bootable ISO
#    make run        Run interactively in QEMU (with display)
#    make test       Run headless and assert (BIOS path)
#    make test-uefi  Run headless and assert (UEFI path)
#    make test-all   Run both boot paths
#    make export     Copy the ISO into the project directory
#    make clean      Remove build artifacts
#    make distclean  clean + remove the download cache
#
#  ---------------------------------------------------------------------
#  About the build directory
#
#  Build artifacts go to the WSL-native path /var/tmp/funyos-build rather
#  than into the project directory. This is not tidiness, it is the
#  conclusion of hitting a real problem:
#
#    A file created by WSL on /mnt/c (a 9p mount) becomes corrupted if
#    WSL is terminated before the data actually reaches disk. The result
#    is a file that Win32 APIs (dir, PowerShell) list with a correct
#    size, but that WSL and MSYS stat() report as ENOENT -- and that
#    del / Remove-Item cannot remove either. Two such phantom files were
#    produced here in practice (funyos.elf / funyos.iso).
#
#  Separately, compiling many small files over 9p is noticeably slower
#  than on a native filesystem.
#
#  Sources stay in the project directory (visible from Windows, suitable
#  for version control); only build output lives inside WSL. Use
#  `make export` when you need the ISO outside.
# =====================================================================

PROJECT := FunnyOS

# Override with: make BUILD_DIR=/somewhere/else
BUILD_DIR ?= /var/tmp/funyos-build

OBJ_DIR  := $(BUILD_DIR)/obj
ISO_ROOT := $(BUILD_DIR)/iso_root
KERNEL   := $(BUILD_DIR)/funyos.elf
ISO      := $(BUILD_DIR)/funyos.iso

LIMINE_DIR := boot/limine/limine-binary
LIMINE     := $(LIMINE_DIR)/limine

# ISO 9660 volume identifier.
#
# This is load-bearing, not cosmetic: boot/limine.conf locates the kernel
# with fslabel(VOLID), so the label baked into the image and the label in
# the config must match exactly. The build verifies this below, because a
# mismatch produces a kernel that builds fine and then cannot be booted.
VOLID := FUNNYOS

TOOLS_DIR := tools
RUN_TEST  := $(TOOLS_DIR)/run-qemu-test.sh
RUN_FAULT_TEST := $(TOOLS_DIR)/run-fault-test.sh
RUN_INPUT_TEST := $(TOOLS_DIR)/run-input-test.sh
RUN_USER_TEST  := $(TOOLS_DIR)/run-user-test.sh
RUN_SCREEN_TEST := $(TOOLS_DIR)/run-screen-test.sh
CHECK_VECTOR_REGS := $(TOOLS_DIR)/check-no-vector-regs.sh

# ---------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------
CC   := gcc
LD   := ld
NASM := nasm

INCLUDES := -Ikernel/include -Ilibk/include -Iboot/limine

# Notes on the less obvious flags:
#   -ffreestanding        No hosted standard library; this is bare metal.
#   -fno-stack-protector  Stack protection needs the host libc's
#                         __stack_chk_fail, which does not exist here.
#   -fno-pic -fno-pie     The kernel is linked at a fixed high address and
#                         does not use position-independent code.
#   -mno-red-zone         Disable the x86-64 128-byte red zone. That
#                         feature assumes a valid stack below rsp, which
#                         interrupt handlers will clobber. The kernel must
#                         turn it off at compile time.
#   -mcmodel=kernel       Code model for the upper half of the address space.
#   -mgeneral-regs-only   Restrict the compiler to general-purpose
#                         registers, forbidding SSE/MMX codegen.
#
#                         THIS ONE IS AN INVARIANT, NOT A PREFERENCE.
#                         An interrupt does not save vector register
#                         state -- the CPU saves that only when software
#                         asks it to. So the kernel never touching a
#                         vector register is precisely what makes
#                         interrupt entry cheap, and what lets a program
#                         keep its floating point registers across one.
#
#                         Remove this flag and every interrupt starts
#                         silently corrupting whatever the interrupted
#                         program had loaded. See
#                         kernel/include/funnyos/arch/x86_64/fpu.h.
#
#                         User programs are built without it, so they may
#                         use floating point; that asymmetry is
#                         deliberate and is the whole design.
CFLAGS := -std=c17 -g -O2 \
          -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -Wall -Wextra \
          $(INCLUDES)

NASMFLAGS := -f elf64 -g -F dwarf

# -z max-page-size forces 4 KiB section alignment, which is also what
# CONSTANT(MAXPAGESIZE) resolves to inside linker.ld.
LDFLAGS := -T linker.ld -nostdlib -z max-page-size=0x1000

# ---------------------------------------------------------------------
# Source discovery
# ---------------------------------------------------------------------
C_SOURCES   := $(shell find kernel libk -name '*.c'   | sort)
ASM_SOURCES := $(shell find kernel      -name '*.asm' | sort)

# Object paths keep the source extension. Without it, foo.c and foo.asm in
# the same directory both map to foo.o and silently clobber each other --
# which shows up as a linker error naming a symbol twice, pointing nowhere
# near the actual cause.
C_OBJS   := $(patsubst %.c,  $(OBJ_DIR)/%.c.o,$(C_SOURCES))
ASM_OBJS := $(patsubst %.asm,$(OBJ_DIR)/%.asm.o,$(ASM_SOURCES))

# ---------------------------------------------------------------------
# User programs
#
# Built separately from the kernel and then embedded in it as a flat
# binary. Three of the flags below are load-bearing:
#
#   (no -mgeneral-regs-only)
#       User programs MAY use floating point and vector registers, and
#       the kernel enables SSE for them. The kernel itself must not, and
#       does not -- see the note above CFLAGS. This asymmetry is the whole
#       design: an interrupt does not save vector state, so the kernel
#       never touching it is what makes interrupt entry cheap, while a
#       process's state is saved whenever control changes hands.
#
#   -mcmodel=small
#       A user program lives in the lower half and is addressed with the
#       small code model, unlike the kernel's -mcmodel=kernel.
#
#   -fno-builtin
#       Stops the compiler turning a loop into a call to a libc function
#       that does not exist on this side of the boundary.
#
#   -Idos/include
#       The 8086 interpreter is compiled into this image as well, and its
#       headers live under dos/include. Without this the interpreter does
#       not compile here at all: every one of its files includes
#       <vm86/...>, and nothing else on the user side puts that directory
#       on the search path.
#
# libk is compiled a second time for user space. It is freestanding
# already, so the only thing that changes is the code model.
# ---------------------------------------------------------------------

USER_CFLAGS := -std=c17 -g -O2 \
               -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
               -mno-red-zone -mcmodel=small \
               -fno-builtin -Wall -Wextra \
               -Iuser -Idos/include -Ikernel/include -Ilibk/include

# The user program is linked into a single loadable segment, so the linker
# warns that it has RWX permissions. That warning is about ELF program
# headers, and the ELF here is an intermediate: it is converted to a flat
# binary two lines later, and a flat binary has no program headers at all.
# The kernel also never enables the NX bit, so segment permissions would
# not be enforced even if they were expressed.
USER_LDFLAGS := -T user/link.ld -nostdlib -z max-page-size=0x1000 \
                --no-warn-rwx-segments

USER_OBJ_DIR := $(BUILD_DIR)/userobj

# The 8086 interpreter is part of the user image, because that is where it
# runs: a Ring 3 process, per DESIGN.md's decision D4. Only the core comes
# in -- dos/tests is the host-side test driver and has no business in a
# program that is supposed to execute guest code.
#
# intr/ and bios/ are globbed rather than listed, the same way dos/Makefile
# globs its suites. Several people add files there at once, and a list is
# the thing each of them would have to edit -- which is exactly the
# shared-file edit the split exists to avoid. An empty glob is empty, so
# this costs nothing before those directories exist.
USER_C_SOURCES   := $(shell find user -name '*.c' | sort) libk/printf.c libk/string.c \
                    $(wildcard dos/cpu/*.c) $(wildcard dos/mem/*.c) \
                    $(wildcard dos/intr/*.c) $(wildcard dos/bios/*.c) \
                    $(wildcard dos/dos/*.c)
USER_ASM_SOURCES := $(shell find user -name '*.asm' | sort)

# --- The BIOS corpus, for the interpreter to run in Ring 3 ------------
#
# The same samples the host suite runs, assembled by the same nasm
# invocation dos/Makefile uses and embedded the same way the init program
# is. Not copied into a C file by hand: the acceptance sentence is about
# one of these programs printing on a real screen, and a second copy of
# the bytes, kept in step by editing, is a copy that would eventually not
# be.
#
# Three of the seven, not all of them. The others are about services
# answering registers, which the host suite checks in a millisecond; they
# would buy nothing here and cost a user image that is embedded inside the
# kernel. `direct` is here because the pair is the point: it writes the
# same text with no interrupt at all, so if the two screens differ on the
# real machine, the display is two pieces of state and this is where that
# shows. `timer` is here because it is the only sample that makes the clock
# do anything, and the clock is a device this machine really has -- the
# calibrated LAPIC timer -- rather than one the host is pretending about.
VM_CORPUS_SRC  := dos/corpus/bios/hello.asm dos/corpus/bios/direct.asm \
                  dos/corpus/bios/timer.asm
VM_CORPUS_BIN  := $(patsubst dos/corpus/bios/%.asm,$(BUILD_DIR)/vmcorpus/%.bin,$(VM_CORPUS_SRC))
VM_CORPUS_C    := $(patsubst dos/corpus/bios/%.asm,$(BUILD_DIR)/generated/vm_corpus_%.c,$(VM_CORPUS_SRC))
VM_CORPUS_OBJS := $(patsubst %.c,%.c.o,$(VM_CORPUS_C))

# --- The DOS corpus, for the same interpreter -------------------------
#
# A separate rule rather than a wider pattern, because the two corpora name
# their symbols differently: dos/corpus/bios/hello.asm becomes
# vm_corpus_hello, and a shared `%` would try to make dos/corpus/dos/psp.asm
# into vm_corpus_dos/psp, which is not an identifier.
#
# These samples are loaded with a Program Segment Prefix rather than by the
# M3/M4 convention, which is why they are not in the list above: the two
# entry conventions both exist and a sample belongs to exactly one of them.
VM_CORPUS_DOS_SRC  := dos/corpus/dos/psp.asm
VM_CORPUS_DOS_BIN  := $(patsubst dos/corpus/dos/%.asm,$(BUILD_DIR)/vmcorpus/dos_%.bin,$(VM_CORPUS_DOS_SRC))
VM_CORPUS_DOS_C    := $(patsubst dos/corpus/dos/%.asm,$(BUILD_DIR)/generated/vm_corpus_dos_%.c,$(VM_CORPUS_DOS_SRC))
VM_CORPUS_DOS_OBJS := $(patsubst %.c,%.c.o,$(VM_CORPUS_DOS_C))

$(BUILD_DIR)/vmcorpus/dos_%.bin: dos/corpus/dos/%.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin $< -o $@

$(BUILD_DIR)/generated/vm_corpus_dos_%.c: $(BUILD_DIR)/vmcorpus/dos_%.bin $(TOOLS_DIR)/bin2c.py
	@mkdir -p $(@D)
	@echo "  EMBED   $@"
	@python3 $(TOOLS_DIR)/bin2c.py $< vm_corpus_dos_$* $@

$(BUILD_DIR)/generated/vm_corpus_dos_%.c.o: $(BUILD_DIR)/generated/vm_corpus_dos_%.c
	@mkdir -p $(@D)
	@echo "  CCu     $<"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

# Kept rather than deleted after use, for the reason dos/Makefile gives for
# the same two lines: make removes intermediates at the end of a run, and a
# deleted .c whose .o survives comes back newer than it on the next build,
# so the corpus would be re-assembled and re-embedded on every build.
.SECONDARY: $(VM_CORPUS_BIN) $(VM_CORPUS_C) $(VM_CORPUS_DOS_BIN) $(VM_CORPUS_DOS_C)

USER_C_OBJS   := $(patsubst %.c,  $(USER_OBJ_DIR)/%.c.o,$(USER_C_SOURCES))
USER_ASM_OBJS := $(patsubst %.asm,$(USER_OBJ_DIR)/%.asm.o,$(USER_ASM_SOURCES))
USER_OBJS     := $(USER_C_OBJS) $(USER_ASM_OBJS) $(VM_CORPUS_OBJS) \
                 $(VM_CORPUS_DOS_OBJS)

USER_ELF := $(BUILD_DIR)/funnycom.elf
USER_BIN := $(BUILD_DIR)/funnycom.bin

INIT_BLOB_C   := $(BUILD_DIR)/generated/funnycom_blob.c
INIT_BLOB_OBJ := $(BUILD_DIR)/generated/funnycom_blob.c.o

OBJS := $(C_OBJS) $(ASM_OBJS) $(INIT_BLOB_OBJ)

# ---------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------
.PHONY: all user run test test-uefi test-all test-fault test-input test-user test-vm test-screen check export clean distclean help

all: $(ISO)

# Just the user program, for checking it builds without the kernel.
user: $(USER_BIN)

$(KERNEL): $(OBJS) linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@bash $(CHECK_VECTOR_REGS) $@

$(OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) $(NASMFLAGS) $< -o $@

# ---------------------------------------------------------------------
# User program
# ---------------------------------------------------------------------

$(USER_OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@echo "  CCu     $<"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	@echo "  NASMu   $<"
	@$(NASM) $(NASMFLAGS) $< -o $@

# The corpus: assembled flat, then turned into a C array. `-f bin` rather
# than the elf64 the rest of the user side uses, because a guest program
# is a flat binary by definition -- see the entry convention in
# dos/corpus/bios/replay.h.
$(BUILD_DIR)/vmcorpus/%.bin: dos/corpus/bios/%.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin $< -o $@

$(BUILD_DIR)/generated/vm_corpus_%.c: $(BUILD_DIR)/vmcorpus/%.bin $(TOOLS_DIR)/bin2c.py
	@mkdir -p $(@D)
	@echo "  EMBED   $@"
	@python3 $(TOOLS_DIR)/bin2c.py $< vm_corpus_$* $@

$(BUILD_DIR)/generated/vm_corpus_%.c.o: $(BUILD_DIR)/generated/vm_corpus_%.c
	@mkdir -p $(@D)
	@echo "  CCu     $<"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_ELF): $(USER_OBJS) user/link.ld
	@echo "  LDu     $@"
	@$(LD) $(USER_LDFLAGS) -o $@ $(USER_OBJS)

# Two objcopy passes, and the first is the one that matters.
#
# .bss is NOBITS: it occupies no space in the file. Converting to a flat
# binary would therefore omit it entirely and report a length that stops
# at the end of .data. Marking .bss loadable turns it into real zero bytes
# that count.
#
# That length is the image size -- how many bytes the kernel copies. It is
# not how much memory the program gets: anything the program declares in
# the NOLOAD section user/link.ld puts after .bss is mapped and zeroed by
# the kernel rather than carried through here, and is counted from
# _image_end instead. See the rule below.
$(USER_BIN): $(USER_ELF)
	@echo "  OBJCOPY $@"
	@objcopy --set-section-flags .bss=alloc,load,contents $< $@.tmp
	@objcopy -O binary $@.tmp $@
	@rm -f $@.tmp
	@echo "  Image   $(USER_BIN): $$(stat -c %s $@) bytes"

# Memory the program asks for, read out of the ELF rather than written
# down here.
#
# The linker is the only thing that knows where the program's memory ends
# -- the end of .bss, plus whatever the NOLOAD section holds -- and a
# constant written down instead would drift away from it silently. Add an
# array to the program, forget the constant, and the new array lands
# outside the mapping and faults on its first write, with the error
# naming the array rather than the constant.
#
# The check below is the point of doing this in four lines rather than
# one. `_image_end - 0x400000` is what the kernel is told, and the flat
# image's length is what it copies; the tail between them is the region
# the kernel maps and zeroes. If the linker ever stops placing _image_end
# where the image ends, the symptom is a global variable that is
# inexplicably wrong, which is a long way from this line.
$(INIT_BLOB_C): $(USER_BIN) $(USER_ELF) $(TOOLS_DIR)/bin2c.py
	@mkdir -p $(@D)
	@echo "  BIN2C   $@"
	@image_size=$$(stat -c %s $(USER_BIN)); \
	image_end=$$(nm $(USER_ELF) | awk '$$NF == "_image_end" { print $$1 }'); \
	if [ -z "$$image_end" ]; then \
	    echo "  ERROR: $(USER_ELF) defines no _image_end."; \
	    echo "         user/link.ld is supposed to; without it the kernel"; \
	    echo "         cannot be told how much memory the program asked for."; \
	    exit 1; \
	fi; \
	memory_size=$$(( 0x$$image_end - 0x400000 )); \
	if [ "$$memory_size" -lt "$$image_size" ]; then \
	    echo "  ERROR: _image_end says $$memory_size bytes of memory, but the"; \
	    echo "         image is $$image_size bytes and goes into it."; \
	    exit 1; \
	fi; \
	echo "  Memory  $(USER_BIN): $$memory_size bytes (image $$image_size, tail $$(( memory_size - image_size )))"; \
	python3 $(TOOLS_DIR)/bin2c.py $< funnyos_init_image $@ $$memory_size

$(INIT_BLOB_OBJ): $(INIT_BLOB_C)
	@mkdir -p $(@D)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Build the bootable ISO. BIOS and UEFI both come in through El Torito
# entries, so a single image serves both firmware types.
#
# Two deliberate omissions, both learned from a VMware boot failure:
#
#   No `limine bios-install`. That step exists to make an image bootable as
#   a hard disk (a USB stick), and it rewrites the partition table to do it.
#   CD boot does not need it, and the MBR it left behind -- with a 32 KiB
#   "bootable" partition holding Limine's stage 2 -- was one more structure
#   for firmware to misread.
#
#   No `--protective-msdos-label`. Dropping it leaves the image with a GPT
#   and no DOS partition table at all, which is one less thing a BIOS can
#   mistake the optical disc for.
#
# -efi-boot-part stays: UEFI needs it to associate the boot device with a
# readable volume. Without it Limine reports "Could not meaningfully match
# the boot device handle with a volume" and cannot find the kernel.
#
# If you want a USB-bootable image instead, the sequence is: add
# --protective-msdos-label back, then run `limine bios-install` on the
# result. That produces an isohybrid image you can dd onto a stick.
$(ISO): $(KERNEL)
	@echo "  Preparing ISO tree"
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot
	@cp $(KERNEL)                          $(ISO_ROOT)/boot/funyos.elf
	@cp boot/limine.conf                   $(ISO_ROOT)/boot/
	@cp $(LIMINE_DIR)/limine-bios.sys      $(ISO_ROOT)/boot/
	@cp $(LIMINE_DIR)/limine-bios-cd.bin   $(ISO_ROOT)/boot/
	@cp $(LIMINE_DIR)/limine-uefi-cd.bin   $(ISO_ROOT)/boot/
	@echo "  XORRISO $@"
	@xorriso -as mkisofs -R -r -J \
	    -V $(VOLID) \
	    -b boot/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image \
	    $(ISO_ROOT) -o $@ >/dev/null 2>&1
	@label=$$(xorriso -indev $@ -pvd_info 2>&1 | sed -n 's/^Volume [Ii]d *: *//p' | head -1 | tr -d "'"); \
	if [ "$$label" != "$(VOLID)" ]; then \
	    echo "  ERROR: ISO volume id is '$$label', expected '$(VOLID)'."; \
	    echo "         boot/limine.conf resolves the kernel via fslabel($(VOLID));"; \
	    echo "         with a mismatched label the image will not boot."; \
	    exit 1; \
	fi; \
	echo "  Volume id verified: $$label"
	@echo ""
	@echo "  Built: $@"

run: $(ISO)
	@echo "  Starting QEMU (with display)"
	@qemu-system-x86_64 \
	    -m 512 -cdrom $(ISO) -boot d \
	    -serial stdio -vga std

test: $(ISO)
	@bash $(RUN_TEST) $(ISO) bios

test-uefi: $(ISO)
	@bash $(RUN_TEST) $(ISO) uefi

# Exercise both boot paths. BIOS and UEFI go through completely different
# firmware stacks, so passing on one says nothing about the other.
test-all: $(ISO)
	@bash $(RUN_TEST) $(ISO) bios
	@bash $(RUN_TEST) $(ISO) uefi

# Boot an image whose kernel command line asks for deliberate fault
# injection, and assert the exception handler produces a real diagnostic.
# This is the M1 acceptance test: a fault must be reported, not turned into
# a triple fault and a silent reboot.
test-fault: $(ISO)
	@bash $(RUN_FAULT_TEST) bios

# Type on the emulated keyboard through the QEMU monitor and assert on
# what comes back. This is the only test that covers the interrupt path
# from the 8042 to the line discipline; the decoder's own self-test proves
# the translation tables but cannot prove an interrupt ever arrives.
test-input: $(ISO)
	@bash $(RUN_INPUT_TEST) $(ISO) bios

# Load and run the user program, in both of the modes the kernel can start
# it in: normally, and deliberately faulting. The second is the one that
# checks the isolation claim -- a program going wrong must stop being a
# program, not a machine.
test-user: $(ISO)
	@bash $(RUN_USER_TEST) $(ISO) bios

# Boot with `vm=1` instead of the shell, and run the 8086 interpreter in
# Ring 3. This is M3's acceptance test: guest instructions executed by the
# operating system, with the terminal state compared against the manual
# rather than printed for a person to look at.
#
# FUNYOS_BUILD_DIR is passed through because the script rebuilds an image
# from the ISO tree, and `make test-vm BUILD_DIR=/somewhere/else` has to
# keep the two of them looking at the same place.
test-vm: $(ISO)
	@FUNYOS_BUILD_DIR=$(BUILD_DIR) \
	    bash $(TOOLS_DIR)/run-vm-test.sh $(ISO) bios

# Boot the machine with a display, leave a DOS program's page on the
# screen, and read the pixels back out of a screendump. This is the only
# test that can see the framebuffer: everything else reads serial, and
# every claim a serial log can make about a screen is consistent with a
# framebuffer nobody wrote to.
test-screen: $(ISO)
	@FUNYOS_BUILD_DIR=$(BUILD_DIR) \
	    bash $(RUN_SCREEN_TEST) $(ISO)

# Everything. Use this before committing.
check: $(ISO)
	@bash $(RUN_TEST) $(ISO) bios
	@bash $(RUN_TEST) $(ISO) uefi
	@bash $(RUN_FAULT_TEST) bios
	@bash $(RUN_INPUT_TEST) $(ISO) bios
	@bash $(RUN_USER_TEST) $(ISO) bios
	@FUNYOS_BUILD_DIR=$(BUILD_DIR) \
	    bash $(TOOLS_DIR)/run-vm-test.sh $(ISO) bios
	@FUNYOS_BUILD_DIR=$(BUILD_DIR) \
	    bash $(RUN_SCREEN_TEST) $(ISO)

# Copy the ISO into the project directory so other emulators on Windows
# can open it. Output goes to dist/ rather than the project root because
# the root holds two phantom files (funyos.iso / funnyos.elf) left behind
# by a lost 9p write, which cannot be overwritten or deleted.
export: $(ISO)
	@mkdir -p dist
	@cp $(ISO) dist/funyos.iso
	@sync
	@echo "  Exported to dist/funyos.iso"
	@echo "  Note: WSL wrote this to /mnt/c; do not terminate WSL until the write has settled."

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  Removed build artifacts ($(BUILD_DIR))"

distclean: clean
	@rm -rf .cache
	@echo "  Removed download cache"

help:
	@echo "FunnyOS build targets:"
	@echo "  make            Build kernel and ISO (output in $(BUILD_DIR))"
	@echo "  make run        Run interactively in QEMU"
	@echo "  make test       Run headless and assert (BIOS path)"
	@echo "  make test-uefi  Run headless and assert (UEFI path)"
	@echo "  make test-all   Run both boot paths"
	@echo "  make test-fault Boot with fault injection and check the diagnostic"
	@echo "  make test-input Type on the emulated keyboard and check the echo"
	@echo "  make test-user  Load and run the user program in its modes"
	@echo "  make test-vm    Run the 8086 interpreter in Ring 3 and assert"
	@echo "  make test-screen Read the framebuffer back and judge the pixels"
	@echo "  make check      Run every test above"
	@echo "  make export     Copy the ISO into the project directory"
	@echo "  make clean      Remove build artifacts"
	@echo "  make distclean  Remove build artifacts and download cache"
