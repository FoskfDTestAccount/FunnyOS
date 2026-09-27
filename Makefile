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
#                         registers, forbidding SSE/MMX codegen. The kernel
#                         does not save FPU state, so any floating point or
#                         vector instruction emitted here would silently
#                         corrupt data on the next context switch.
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
OBJS     := $(C_OBJS) $(ASM_OBJS)

# ---------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------
.PHONY: all run test test-uefi test-all test-fault test-input check export clean distclean help

all: $(ISO)

$(KERNEL): $(OBJS) linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) $(NASMFLAGS) $< -o $@

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

# Everything. Use this before committing.
check: $(ISO)
	@bash $(RUN_TEST) $(ISO) bios
	@bash $(RUN_TEST) $(ISO) uefi
	@bash $(RUN_FAULT_TEST) bios
	@bash $(RUN_INPUT_TEST) $(ISO) bios

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
	@echo "  make check      Run every test above"
	@echo "  make export     Copy the ISO into the project directory"
	@echo "  make clean      Remove build artifacts"
	@echo "  make distclean  Remove build artifacts and download cache"
