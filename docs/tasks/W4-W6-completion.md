# W4–W6 完成记录（2026-10-01）

接续 `W4-handover.md` 的未提交工作区。交接文档保留为历史记录；本次
**未丢弃原有改动、未自动提交、未推送、未发布**。

## 1. 交付范围

| 工作包 | 交付 | 验收 |
|---|---|---|
| W4 DOS 核心收尾 | 十二个核心功能码、INT20h、返回码/服务 flags 通路；构建头依赖和默认目标修复；依据与计划回填 | int21 套件 73 断言；原有三页真机屏幕回归；17 个定向注入 |
| W5 FAT 镜像与文件 | 纯字节数组 FAT12/FAT16 挂载；8.3 根目录/现有子目录；16 个文件句柄；3Ch–42h、4Eh/4Fh | fat 256 断言、files 69 断言；从镜像加载的 FILES.COM；BIOS/UEFI 各一页像素检查；mtools、DOSBox 对照 |
| W6 键盘与 DOS 输入 | 独占原始 set-1 字节流与非阻塞查询；INT09/INT16 接线；01h/06h/07h/08h/0Ah/0Bh；默认及 guest INT23 | input 45 断言；普通/Shift/方向/功能键双字段；行编辑；BIOS/UEFI 各一页像素检查；释放后非阻塞译码读键 |

W4–W6 范围内已完成；**不把这句话偷换成“任何现成 DOS 应用都兼容”**。
M5 的“第一个真实现成应用”兼容性选型仍没有一份独立完成证明，
因此没有把整个里程碑或发行状态改成已发布。

## 2. 最终边界

### 文件：资源跨内核，FAT 不跨

内核 ramfs 内嵌确定性的 `DOS.IMG`（360 KiB FAT12）；SYS_OPEN/READ
把镜像资源交给 Ring 3。VM 挂载自己的可写副本，DOS 文件调用在 VM
内部处理。没有内核 FAT parser，没有每次读扇区都映射整个镜像。

默认镜像含 `HELLO.TXT`、`EMPTY.TXT`、跨簇的 `BIG.BIN`，以及
`FILES.COM`、`KEYS.COM`、`INT21.COM`、`PSP.COM`。Shell 新增：

```text
dos files.com
dos keys.com
```

第二条等待 `a`、`Shift+B`、`Up`、`F1`、`c`、`d`、`Backspace`、`e`、
`Enter`；这是验收样本的特定输入，不是通用键盘演示。
`DOS <8.3.COM> [args]` 是通用加载入口，命令尾交给既有 PSP 构造器。
测试专用 `vm=files` / `vm=keys` 会把最终页留到下一个键，以供截图。

写入在同次运行的关闭/重开后可见；新运行重挂载，不跨运行持久化。
这有意收窄原计划“不写 FAT”：真正实现并验证 /3Ch、/40h、/41h，
但不顺手扩大成块设备与宿主文件写回工程。

### 键盘：原始流是进程资源

内核独占流 API 为 SYS_KBD_ACQUIRE/POLL/RELEASE；没有键立即返回，
溢出明确返回错误。IRQ 把原始字节给 VM，不给 Shell 追加一个译码副本。
VM 仅在 BIOS latch 空且无 pending IRQ 时取一个字节，完整保留前缀、
修饰键、make/break；实际走 INT09，不能拿翻译后字符反查扫描码。

释放或进程销毁清队列/修饰键并交还控制台。SYS_POLLKEY 的非阻塞
译码形式在截图 hold 的释放路径实测；Shell 回归连续启动两次
FILES.COM 和一次 KEYS.COM，然后 `echo afterdos` 仍得到完整一行。

### 公共陷阱层的真正修复

阻塞服务在现有 stub trap 内重试，**不弹 guest INT 帧，不重复 INT**。
一个 last-SP 不能描述嵌套键盘/定时器 IRQ，所以 CPU 记录 SS:SP 帧栈，
IRET 恢复外层帧身份；far-call 链不能假装是 INT 帧。最多追踪 64 层，
更深不猜测 flags 地址。计数器按机器重置。

宿主测试专门验证：等待时只有一个 live frame（SP-6）、完成后原 SP、
嵌套 Ctrl-C handler 实际运行、无拒绝 flags 写回。一个丢弃外层帧身份
的注入确实会让 `no dropped flags` 失败。

## 3. 已执行的验证与复现

构建产物始终放在 `/var/tmp/funyos-build`，没有写入仓库根目录的旧幽灵 ISO。

```bash
cd /mnt/c/FunnyOS
make -B -j4
make -C dos -B -j4 test
make check
python3 tools/inject-int21.py
python3 tools/inject-resources.py
```

- 全量重建通过，链接后反汇编确认内核不碰向量寄存器。
- **27 个宿主套件通过**；其中 25 个断言式套件共 4,405 条检查，
  另两套分别执行 7 个 BIOS 样本、17 个指令语料用例。
- `make check` 现在包含宿主套件、原有七组 QEMU 回归，再加 W5/W6
  资源验收（files/keys × BIOS/UEFI）。Shell 输入回归含 DOS 的启动和归还。
- W4 基线 **17/17** 和 W5/W6 后的 **17/17** 均触发各自预期诊断；
  新资源注入 **21/21**，只把运行时诊断算命中，编译错误/超时不算。
- 四个相关 suite（fat/files/input/int21）在 **ASan+UBSan** 下共
  443 条断言通过，未报告 sanitizer 错误。
- 四次 W5/W6 QEMU 截图，每页 **32,000** 条扫描线核对。检查 guest
  内容、真实 framebuffer、cursor、区块外颜色；不把串口 PASS 代替像素。
- mtools 独立列出镜像目录并读出 `HELLO.TXT` 的 19 字节 CRLF 文本。
- 同一份 FILES.COM 在 **DOSBox 0.74-3** 执行，35 字节输出完全匹配，
  文件读回、DTA 大小/名字、FindNext 耗尽和删除的样本自检一致。

### 单独跑资源验收

```bash
make test-dos-resources
# 只看一个固件路径：
python3 tools/run-dos-resources-test.py bios
python3 tools/run-dos-resources-test.py uefi
```

图片、串口与 QEMU 诊断留在
`/var/tmp/funyos-build/dos-resources-test/{files,keys}-{bios,uefi}/`。
测试走 QMP 标记等待：W6 标记只有在 guest 完成空队列检查并实际画出提示后才发出，避免慢 TCG 的初始化与按键竞态。sendkey 拒绝和 QEMU 提前退出会失败；超时不被当成成功。

### 可选的外部对照（不要求普通 make check 安装 DOSBox）

```bash
DOSBOX=/absolute/path/to/dosbox python3 tools/compare-dosbox.py
```

本次把发行版 DOSBox 与运行依赖解包在 `/var/tmp/funyos-dosbox-runtime`，
**没有系统安装**；复现本机这次对照可执行：

```bash
DOSBOX=/var/tmp/funyos-dosbox-runtime/root/usr/bin/dosbox \
LD_LIBRARY_PATH=/var/tmp/funyos-dosbox-runtime/root/usr/lib/x86_64-linux-gnu \
python3 tools/compare-dosbox.py
```

对照不比较键盘时序、内部内存状态，也不声称从 DOSBox 取回了退出码。
DOSBox 挂的是宿主目录，故这不是独立 FAT 实现的逐位对照；镜像的
独立可读性是 mtools 的检查，文件行为和公开寄存器/DTA 是 `.COM` 的检查。

### Sanitizer 复现

```bash
make -C dos -B -j4 BUILD=/var/tmp/funyos-build/sanitized \
  CFLAGS='-std=c17 -g -O1 -Wall -Wextra -Werror -fno-builtin -Iinclude -I../libk/include -fsanitize=address,undefined -fno-omit-frame-pointer' \
  test-fat test-files test-input test-int21
```

## 4. 主要文件

- `dos/dos/fat.c` / `dos/include/vm86/fat.h`：FAT 字节数组后端。
- `dos/dos/int21.c` / `dos/include/vm86/int21.h`：核心、文件、输入分发。
- `kernel/console/kbd.c`、`kernel/proc/syscall.c`、`kernel/proc/process.c`：
  raw 所有权、非阻塞系统调用、退出/故障释放。
- `user/vm/vm.c`、`user/funnycom/main.c`：挂载、从盘装载 COM、驱动输入、Shell 入口。
- `dos/intr/deliver.c`、`dos/intr/trap.c`、`dos/cpu/ops_ctl.c`：嵌套中断帧身份。
- `dos/corpus/dos/{int21,files,keys}.asm`：执行语料；W5/W6 新语料显式指定
  `cpu 8086`，避免 NASM 为远条件跳转生成 386 的 0F8x。
- `dos/tests/test_{fat,files,input,int21}.c`：独立寄存器/内存/完整程序验收。
- `tools/make-fat-image.py`、`tools/run-dos-resources-test.py`、
  `tools/compare-dosbox.py`、`tools/inject-{int21,resources}.py`：生成、
  QEMU/像素、外部对照和定向缺陷。
- README、DESIGN、M5-README、DOS 依据文件已同步当前范围；交接文档标为历史。

## 5. 明确没有证明或没有实现的

- 真实物理 PC（当前硬件链路验收运行在 QEMU）。
- 任意现成 DOS 应用兼容；MZ/EXE、EXEC、TSR、FCB I/O 和内存分配是后续。
- FAT32、MBR 分区选择、宿主路径映射、跨运行持久化、目录创建/扩容，
  当前目录/长文件名/完整设备与共享锁语义。
- 完整 DOS cooked/可重定向 stdin、F1–F5 模板行编辑、Ctrl-Z 文件 EOF。
- 通用交互程序不限时运行：现有 VM 驱动仍有 5 秒/步数的防挂死预算。
- 完整变异表的覆盖率：这里只报告 **17+21 个定向注入**，没有把全表
  未跑过的 M3/M4 缺陷或所有输入边缘情况算作通过。
- 独立交叉审查：对照、注入和 sanitizer 不能替代一个没有共享误解的审查者。
