# =====================================================================
#  FunnyOS 构建系统
#
#  目标：
#    make            构建内核与可引导 ISO
#    make run        在有显示的 QEMU 中交互运行
#    make test       无头运行，捕获串口输出并断言
#    make export     把 ISO 复制到项目目录，便于在 Windows 侧使用
#    make clean      清除构建产物
#    make distclean  clean + 清除下载缓存
#
#  ---------------------------------------------------------------------
#  关于构建目录的重要说明
#
#  构建产物默认放在 WSL 原生文件系统 /var/tmp/funyos-build 下，
#  而不是项目目录里。这不是洁癖，是踩坑后的结论：
#
#    在 /mnt/c（9p 协议挂载）上由 WSL 创建的文件，如果 WSL 在数据
#    真正落盘之前被终止，会留下一种损坏状态——Win32 API（dir、
#    PowerShell）能列出文件、长度也正确，但 WSL 与 MSYS 的 stat()
#    一律返回 ENOENT，且 Remove-Item 也删不掉。实测已经产生过两个
#    这样的幽灵文件（funyos.elf / funyos.iso）。
#
#  除此之外，9p 上编译大量小文件也明显慢于原生文件系统。
#
#  源码仍然留在项目目录里（Windows 侧可见、可版本控制），
#  只有构建产物在 WSL 内部。需要 ISO 时用 make export。
# =====================================================================

PROJECT := FunnyOS

# 允许通过环境变量 BUILD_DIR=... 覆盖
BUILD_DIR ?= /var/tmp/funyos-build

OBJ_DIR  := $(BUILD_DIR)/obj
ISO_ROOT := $(BUILD_DIR)/iso_root
KERNEL   := $(BUILD_DIR)/funyos.elf
ISO      := $(BUILD_DIR)/funyos.iso

LIMINE_DIR := boot/limine/limine-binary
LIMINE     := $(LIMINE_DIR)/limine

TOOLS_DIR := tools
RUN_TEST  := $(TOOLS_DIR)/run-qemu-test.sh

# ---------------------------------------------------------------------
# 工具链
# ---------------------------------------------------------------------
CC   := gcc
LD   := ld
NASM := nasm

INCLUDES := -Ikernel/include -Ilibk/include -Iboot/limine

# 关键标志说明：
#   -ffreestanding        无宿主标准库，这是裸机代码
#   -fno-stack-protector  栈保护依赖宿主 libc 的 __stack_chk_fail
#   -fno-pic -fno-pie     内核不使用位置无关代码（链接到固定高地址）
#   -mno-red-zone         关掉 x86-64 的 128 字节 red zone。该特性假设
#                         存在有效栈，中断处理会破坏这个假设，
#                         内核必须在编译期禁用。
#   -mcmodel=kernel       代码模型落在地址空间上半区
#   -mgeneral-regs-only   只用通用寄存器，禁止生成 SSE/MMX 指令。
#                         内核不保存 FPU 状态，任何浮点/向量指令都会
#                         在后续上下文切换时静默损坏数据。
CFLAGS := -std=c17 -g -O2 \
          -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -Wall -Wextra \
          $(INCLUDES)

NASMFLAGS := -f elf64 -g -F dwarf

# -z max-page-size 让链接器按 4 KiB 对齐段，
# 这也是 linker.ld 里 CONSTANT(MAXPAGESIZE) 的取值来源。
LDFLAGS := -T linker.ld -nostdlib -z max-page-size=0x1000

# ---------------------------------------------------------------------
# 源文件枚举
# ---------------------------------------------------------------------
C_SOURCES   := $(shell find kernel libk -name '*.c'   | sort)
ASM_SOURCES := $(shell find kernel      -name '*.asm' | sort)

C_OBJS   := $(patsubst %.c,  $(OBJ_DIR)/%.o,$(C_SOURCES))
ASM_OBJS := $(patsubst %.asm,$(OBJ_DIR)/%.o,$(ASM_SOURCES))
OBJS     := $(C_OBJS) $(ASM_OBJS)

# ---------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------
.PHONY: all run test test-uefi test-all export clean distclean help

all: $(ISO)

$(KERNEL): $(OBJS) linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) $(NASMFLAGS) $< -o $@

# 构造可引导 ISO（BIOS + UEFI 双路）
$(ISO): $(KERNEL)
	@echo "  准备 ISO 目录结构"
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot
	@cp $(KERNEL)                          $(ISO_ROOT)/boot/funyos.elf
	@cp boot/limine.conf                   $(ISO_ROOT)/boot/
	@cp $(LIMINE_DIR)/limine-bios.sys      $(ISO_ROOT)/boot/
	@cp $(LIMINE_DIR)/limine-bios-cd.bin   $(ISO_ROOT)/boot/
	@cp $(LIMINE_DIR)/limine-uefi-cd.bin   $(ISO_ROOT)/boot/
	@echo "  XORRISO $@"
	@xorriso -as mkisofs -R -r -J \
	    -b boot/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(ISO_ROOT) -o $@ 2>/dev/null
	@echo "  写入 BIOS 引导阶段"
	@$(LIMINE) bios-install $@ >/dev/null
	@echo ""
	@echo "  构建完成：$@"

run: $(ISO)
	@echo "  启动 QEMU（有显示模式）"
	@qemu-system-x86_64 \
	    -m 512 -cdrom $(ISO) -boot d \
	    -serial stdio -vga std

test: $(ISO)
	@bash $(RUN_TEST) $(ISO) bios

test-uefi: $(ISO)
	@bash $(RUN_TEST) $(ISO) uefi

# 两条引导路径都跑。BIOS 与 UEFI 走的是完全不同的固件栈，
# 只在其中一条上通过不代表另一条可用。
test-all: $(ISO)
	@bash $(RUN_TEST) $(ISO) bios
	@bash $(RUN_TEST) $(ISO) uefi

# 把 ISO 复制到项目目录，方便在 Windows 侧用其它模拟器打开。
# 输出到 dist/ 而不是项目根目录——根目录下有两个因 9p 写入丢失
# 而产生的幽灵文件（funyos.iso / funyos.elf），无法被覆盖或删除。
export: $(ISO)
	@mkdir -p dist
	@cp $(ISO) dist/funyos.iso
	@sync
	@echo "  已导出到 dist/funyos.iso"
	@echo "  注意：该文件由 WSL 写入 /mnt/c，请在确认写入完成前不要终止 WSL。"

clean:
	@rm -rf $(BUILD_DIR)
	@echo "  已清除构建产物（$(BUILD_DIR)）"

distclean: clean
	@rm -rf .cache
	@echo "  已清除下载缓存"

help:
	@echo "FunnyOS 构建目标："
	@echo "  make            构建内核与 ISO（输出到 $(BUILD_DIR)）"
	@echo "  make run        在 QEMU 中交互运行"
	@echo "  make test       无头运行并断言（BIOS 路径）"
	@echo "  make test-uefi  无头运行并断言（UEFI 路径）"
	@echo "  make test-all   两条引导路径都测"
	@echo "  make export     把 ISO 复制到项目目录"
	@echo "  make clean      清除构建产物"
	@echo "  make distclean  清除构建产物与下载缓存"
