# M6 集成验收与证据归档

**记录日期：2026-10-02**

这份文档描述 M6 的可重复验收入口和证据边界。它是集成/发布材料，
不修改 DOS 核心实现，也不把自编的 `MZTEST.EXE`、`EXEC.COM` 当作真实
MS-DOS 应用兼容性证明。

## 1. 普通 M6 ISO 验收

所有构建产物放在 WSL 原生文件系统，避免在 `/mnt/c` 上编译：

```bash
BUILD=/var/tmp/funyos-m6/funyos-build

make -j4 BUILD_DIR="$BUILD" all
PYTHONDONTWRITEBYTECODE=1 python3 tools/run-terminal-test.py \
  --build-dir "$BUILD" --log-dir "$BUILD/terminal-test" bios uefi

python3 tools/run-m6-acceptance.py \
  --build-dir "$BUILD" \
  --archive /var/tmp/funyos-m6/m6-acceptance \
  --full-check-log /var/tmp/funyos-m6/full-check-exec.log \
  --vbox-log /var/tmp/funyos-m6/vbox-m6-exec.log
```

`run-terminal-test.py` 的成功条件包括：

- BIOS 和 UEFI 都执行 `MZTEST.EXE`，出现 `M6 MZ loader PASS`；
- BIOS 和 UEFI 都执行 `EXEC.COM`，出现子程序和父程序 PASS，并观察到返回码 0；
- 多标签、DOS 等待/恢复、鼠标入口、会话隔离和资源复用断言全部通过；
- 串口日志中不出现 `PANIC`、`CPU EXCEPTION`、`RUN LIMIT` 等失败标记。

`run-m6-acceptance.py` 会把 ISO、BIOS/UEFI 串口日志、QEMU 诊断日志以及
传入的全量回归/VirtualBox 日志复制到归档目录，并生成：

- `manifest.json`：来源、文件大小、SHA-256 和声明边界；
- `SHA256SUMS`：归档文件哈希；
- `README.txt`：归档用途和限制。

## 2. MS-DOS 4.0 EDLIN/DEBUG 外部应用验收

上游源码和编译输出位于被 `.gitignore` 忽略的参考树。为了避免把外部
二进制误加入普通发布 ISO，先建立一个明确的 staging 目录：

```bash
STAGE=/var/tmp/funyos-m6/msdos4/apps
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp /var/tmp/funyos-m6/msdos4/SRC/CMD/EDLIN/EDLIN.COM "$STAGE/"
cp /var/tmp/funyos-m6/msdos4/SRC/CMD/DEBUG/DEBUG.COM "$STAGE/"
sha256sum "$STAGE/EDLIN.COM" "$STAGE/DEBUG.COM"

APP_BUILD=/var/tmp/funyos-m6/msdos4/apps-build
make m6-app-iso \
  M6_APPS_DIR="$STAGE" BUILD_DIR="$APP_BUILD"
```

`m6-app-iso` 是显式 opt-in 目标；普通 `make all` 不会包含这两个外部
程序。必须为它使用新的 `BUILD_DIR`，因为 FAT 镜像是生成文件，Make 不会
把“改变了命令行变量”当作已有目标的内容变化。

### 2.1 有界 smoke 命令

下面命令只验证程序能被装载、进入交互提示并能从最短退出路径返回；它
**不等于**完整 EDLIN/DEBUG 兼容性：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 tools/run-terminal-test.py \
  --build-dir "$APP_BUILD" \
  --log-dir "$APP_BUILD/terminal-test" \
  --app-suite bios uefi
```

`--app-suite` 会在 MZ/EXEC 核心夹具之后执行：

```text
dos edlin.com
q
dos debug.com
q
```

脚本会等待 EDLIN 的 `End of input file`、DEBUG 的 `-` 提示以及 DOS 返回码。
如果某个应用在提示或退出路径失败，脚本应保持失败；不要用“Shell 仍然
可用”替代应用通过。

### 2.2 真实工作流命令

交互式检查时，建议逐步保留串口日志，并分别验证文件内容：

```text
dos edlin.com M6EDIT.TXT
```

在 EDLIN 提示符下执行一个最小编辑/列表/退出流程：

```text
I
M6 EDLIN acceptance line
<空行结束插入>
1,1L
E
Q
```

随后回到 Shell：

```text
dir
 type M6EDIT.TXT
```

应独立记录：启动、插入、列表、保存、重新读取和退出各阶段；不能仅凭
屏幕上出现过一行文字就判定保存成功。

DEBUG 的最小命令流建议从只读操作开始：

```text
dos debug.com
R
D 100 10F
Q
```

这组命令只覆盖进入、寄存器查看、内存查看和退出。汇编/写内存、加载
文件、写回文件和断点等命令属于更高一层的 DEBUG 兼容性，不应在没有
单独日志和结果断言时标记为通过。

## 3. 当前证据与边界

截至 2026-10-02，最终开发 ISO 的记录为：

- ISO：`/var/tmp/funyos-m6/funyos-build/funyos.iso`；
- 大小：`4,759,552` bytes；
- SHA-256：
  `3322c6d2435a29a9a9cbb66f64a7523ad30432de1c26979b3aa1c99f163a18d3`；
- `dist/funyos.iso` 与 `dist/funyos-m6-preview.iso` 与构建 ISO 哈希一致；
- 全量日志：`/var/tmp/funyos-m6/full-check-exec.log`；宿主 DOS 套件合计
  4,415 条断言，0 失败；
- BIOS/UEFI 原生终端回归均通过；VirtualBox 终端回归日志为
  `/var/tmp/funyos-m6/vbox-m6-exec.log`。

这些结果证明 M6 核心服务、MZ/EXEC 自编 guest 夹具和多标签终端集成路径
通过了当前自动化回归。它们**不证明**：

1. MS-DOS 4.0 EDLIN 的全部编辑、保存、错误路径已经兼容；
2. MS-DOS 4.0 DEBUG 的全部命令、文件和内存调试路径已经兼容；
3. 所有 DOS 应用都能运行；
4. 自编夹具覆盖了真实应用遇到的所有 `INT 21h` ABI 边界。

归档脚本明确把 EDLIN/DEBUG 标成外部输入，把两者的 full compatibility
标成 `false`，避免发布材料出现过度声明。
