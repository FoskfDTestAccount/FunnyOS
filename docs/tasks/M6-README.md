# M6：MZ EXE 与 DOS 兼容性扩展

**开始日期：2026-10-02**

M6 的目标是把 M5 的 `.COM`/基础 `INT 21h` 环境扩展到能承载 MZ/EXE、
句柄共享、内存块和真实 DOS 工具的边界。本文只记录已经落地或已经有
证据的内容；计划、夹具和上游应用不混写成“兼容性已完成”。

## 状态摘要

- **M6 核心集成：已通过当前回归。**
- **MZ/EXEC guest 夹具：BIOS/UEFI 通过。**
- **全量宿主/QEMU/VirtualBox 证据：已归档入口。**
- **MS-DOS 4.0 EDLIN/DEBUG：有独立 staging 和有界 smoke 入口；完整应用
  兼容性未结项。**
- **M7：进入规划阶段，尚未开始实现。**

## 已完成的核心范围

1. MZ 头部校验、文件长度边界、镜像装载和 relocation；
2. EXE 初始 `CS:IP`、`SS:SP`、`DS/ES` 与 PSP 连接；
3. JFT/SFT 风格共享句柄、标准句柄、复制/强制复制和 `<`/`>`/`>>` 重定向；
4. `AH=48h/49h/4Ah` MCB 分配、释放、调整和链完整性；
5. FCB 解析、打开/创建/关闭、顺序/随机/块读写、查找、删除和 DTA；
6. `AH=56h` 重命名、`AH=57h` 时间戳、`AH=31h` TSR 内存保留路径；
7. 阻塞式 `AH=4Bh` EXEC：子 PSP、句柄/环境继承、返回码和普通子块回收；
8. 多标签终端中的 DOS 会话隔离、等待/恢复和资源复用。

## 固定 guest 夹具与真实应用的边界

`MZTEST.EXE`、`EXEC.COM` 和 `EXECHILD.COM` 是仓库内的自编验收夹具。它们
覆盖 FunnyOS 实现的 MZ 入口、relocation、父子 EXEC 和返回码契约，但不能
证明所有真实 DOS 程序都兼容。

MS-DOS 4.0 的 EDLIN/DEBUG 二进制在被忽略的 `reference-msdos/` 构建目录中
维护。它们不进入普通 ISO；使用 `M6_APPS_DIR` 才会被显式放进一个单独的
临时 FAT/ISO。当前验收分为三层：

| 层次 | 入口 | 结论含义 |
|---|---|---|
| MZ/EXEC 核心夹具 | `run-terminal-test.py` 默认路径 | 核心装载/父子返回码契约 |
| EDLIN/DEBUG 有界 smoke | `run-terminal-test.py --app-suite` | 能否进入提示和走最短退出路径 |
| EDLIN/DEBUG 工作流 | 手动或后续专用脚本 | 编辑/保存、寄存器/内存/文件命令的真实应用兼容性 |

因此当前不能把 EDLIN/DEBUG 标记为“完整兼容”。具体命令和预期边界见
[`M6-acceptance.md`](M6-acceptance.md)。

## 可重复构建和验收

普通核心 ISO：

```bash
BUILD=/var/tmp/funyos-m6/funyos-build
make -j4 BUILD_DIR="$BUILD" all
PYTHONDONTWRITEBYTECODE=1 python3 tools/run-terminal-test.py \
  --build-dir "$BUILD" --log-dir "$BUILD/terminal-test" bios uefi
```

归档 ISO、日志和哈希：

```bash
python3 tools/run-m6-acceptance.py \
  --build-dir /var/tmp/funyos-m6/funyos-build \
  --archive /var/tmp/funyos-m6/m6-acceptance \
  --full-check-log /var/tmp/funyos-m6/full-check-exec.log \
  --vbox-log /var/tmp/funyos-m6/vbox-m6-exec.log
```

外部应用 staging：

```bash
STAGE=/var/tmp/funyos-m6/msdos4/apps
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp /var/tmp/funyos-m6/msdos4/SRC/CMD/EDLIN/EDLIN.COM "$STAGE/"
cp /var/tmp/funyos-m6/msdos4/SRC/CMD/DEBUG/DEBUG.COM "$STAGE/"
APP_BUILD=/var/tmp/funyos-m6/msdos4/apps-build
make m6-app-iso M6_APPS_DIR="$STAGE" BUILD_DIR="$APP_BUILD"
PYTHONDONTWRITEBYTECODE=1 python3 tools/run-terminal-test.py \
  --app-suite --build-dir "$APP_BUILD" \
  --log-dir "$APP_BUILD/terminal-test" bios uefi
```

`m6-app-iso` 是 opt-in 目标，普通 `make all` 不带 EDLIN/DEBUG。外部程序
的大小和 SHA-256 应随 staging 日志一并保留。

## 当前发布证据（2026-10-02）

- 构建 ISO：`/var/tmp/funyos-m6/funyos-build/funyos.iso`；
- 大小：`4,759,552` bytes；
- SHA-256：`3322c6d2435a29a9a9cbb66f64a7523ad30432de1c26979b3aa1c99f163a18d3`；
- `dist/funyos.iso` 与 `dist/funyos-m6-preview.iso` 哈希相同；
- 全量回归：`/var/tmp/funyos-m6/full-check-exec.log`，宿主套件合计
  4,415 条断言，0 失败；
- VirtualBox：`/var/tmp/funyos-m6/vbox-m6-exec.log`；
- 归档工具生成的 `manifest.json` 会再次记录这些路径和哈希，避免手工
  抄录成为唯一证据。

## 后续 M7 规划

M7 不是立即做窗口式 GUI，而是先为真实 DOS 图形程序建立图形和扩展内存
基础：

1. VGA Mode 13h 的模式切换、平面/线性显存语义和像素验收；
2. VESA 基础模式与模式信息查询；
3. 8087/x87 指令覆盖与真实程序驱动测试；
4. EMS/XMS 的最小可用内存扩展接口；
5. 选择一个有明确输入/显示边界的真实 DOS 游戏作为最终应用验收。

GUI 和抢占式后台多任务仍是后续范围，不应在 M7 第一阶段与图形设备模拟
混为一个目标。
