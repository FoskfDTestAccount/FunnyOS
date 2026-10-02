# M5 真实应用：任务一选型、任务二兼容性摸底

日期：2026-10-01。接续 W1–W6，**本次只做选型、参考基线、诊断与问题清单**。
没有实现句柄复制、重定向或其他新 DOS 服务；没有提交、推送或发布。

## 1. 结论与下一步

首个目标选为 **Microsoft MS-DOS 2.0 目录下发布的 MORE.COM**，使用原始
4,364 字节二进制，不修补、不重编译、不用自编程序替代。

- 任务一完成：固定来源与哈希，筛选候选，定义工作流；在 DOSBox 0.74-3
  建立六组**非交互短输入**的精确字节基线。
- 任务二完成：同一二进制经真实 FunnyCOM → DOS.IMG → FAT → PSP → VM
  链路，在 BIOS/UEFI × 有/无诊断构建中各运行两个命令，共 **8 次**。
- **应用不兼容，M5 不结项**。首个阻塞点是 `INT 21h/AH=45h` 复制句柄。
  MORE 没有检查这次失败的 CF，把失败后的 AX 当成输入句柄，之后反复读失败，
  最终达到宿主运行预算。不是编译失败、QEMU 外部超时或 CPU 异常。
- 各次运行结束后，独立执行 `echo aftermore1/2` 的输出完整，Shell 恢复。
  这是正常预算中止后的恢复证据，不是任意 guest 故障或强制中止的全面证明。

下一轮应先设计 **DOS 句柄表与共享打开对象**，再实现复制/关闭/重定向；
不能只补一个让 AH=45h 返回成功的分支，也不应通过修改 MORE 来绕过问题。

## 2. 选型记录

以下是本地参考材料的静态筛选，不声称候选全部做过运行对照。

| 候选 | 本地二进制 | 初步依据与选择 |
|---|---|---|
| **MORE** | v2.0/bin/MORE.COM，4,364 字节 | 选中。小型真实过滤/分页工具；版本查询、标准句柄复制、读入、字符输出和分页输入形成明确工作流；没有本次代码段中的磁盘扇区或图形模式依赖 |
| EDLIN | v2.0/bin/EDLIN.COM，4,389 字节 | 后续候选。启动代码已出现 AH=37h，配套 EDLIN.ASM 有 FCB_OPEN/FCB_CREATE 等文件操作，会把首轮范围扩大到 FCB 与编辑器语义 |
| DEBUG | v2.0/bin/DEBUG.COM，11,764 字节 | 后续候选。启动出现 AH=51h/52h、后续有 AH=26h；需更复杂 PSP、DOS 内部结构及调试行为，不适合第一条有界兼容闭环 |
| 磁盘工具 | CHKDSK、FORMAT、DISKCOPY 等 | 不选本轮目标。其磁盘工作流不是当前私有 FAT 文件接口验收的最小范围 |

来源仓库：`microsoft/MS-DOS`。固定 Git 提交：
`2d04cacc5322951f187bb17e017c12920ac8ebe2`。

MORE.COM SHA-256：

```text
56830889fef22d56ef5938df0394bf30a8416cc06225b8dc35adc74bb13416b7
```

机器可读选型清单：[`../../dos/apps/msdos2-more.json`](../../dos/apps/msdos2-more.json)。
工具核对工作区文件与固定提交的 Git 对象一致，保留该提交的 LICENSE 到产物目录；
参考仓库没有被修改。参考材料与应用二进制仍不加入 FunnyOS 主仓库。

### 源码不是二进制的等价替身

配套 `v2.0/source/MORE.ASM` 的 `IBMVER=TRUE` 分支含 BIOS 显示查询；但本次
二进制的可执行代码 `0100h..01D9h` 不含这个查询，且 MAXROW 数据为 24。
分页继续分支也有差异。因此仅把源码用于理解设计，**依赖与调用位置以固定
二进制的反汇编为准**，不能把源码配置的预期强加给发布二进制。

二进制代码中确认的 DOS 入口：30h、09h、45h、3Eh、3Fh、02h、0Ch；
退出使用 INT20h。线性反汇编在 `01DAh` 之后进入数据和缓冲区，不能把那里
“反汇编出来的指令”当作 CPU 需求。

## 3. 参考基线与最终验收工作流

参考环境：**DOSBox 0.74-3**，宿主目录挂载到 F:，不是启动 MS-DOS 2.0
系统盘。两边使用同一 SHA-256 的 MORE.COM。

```text
MORE < INPUT.TXT > OUTPUT.TXT
```

独立预期：初始化输出 CRLF，再输出输入字节，遇 1Ah 或文件 EOF 停止；
不规范化 CR/LF、Tab、Backspace，不从应用实际输出反推期望。

| 输入案例 | 输入字节数 | 精确匹配的输出字节数 | 本次覆盖 |
|---|---:|---:|---|
| empty | 0 | 2 | 空输入，仍有启动 CRLF |
| short | 25 | 27 | 两行文本、文件 EOF |
| controls | 19 | 21 | Tab、Backspace、CRLF 保留 |
| ctrlz | 30 | 14 | 1Ah 后文本不输出 |
| wrap | 83 | 85 | 81 字符行，短输入不触发分页 |
| near-page | 198 | 200 | 22 行，接近但尚未到分页等待 |

六组输出全部精确匹配。DOSBox 进程正常退出不等于已捕获 guest 的退出码；
本次没有把宿主进程返回码当成 MORE 的 DOS 退出状态。

### 后续“跑通 MORE”的验收标准

1. 两行文件与空文件经 DOS stdin 绑定读入，输出与本次基线一致。
2. 同一个二进制完成 Ctrl-Z 与文件 EOF 两条路径。
3. stdout 重定向后，02h/09h 等输出进入目标文件，而不是仍直接写 guest 屏幕。
4. 另外建立至少一页以上输入的**参考交互基线**，实际看到分页提示，输入后
   继续、最终退出；这条本次没有完成，也不能用六个短输入代替。
5. BIOS/UEFI 上均通过，返回 Shell 后仍能输入命令，多次运行资源不残留。

当前尚未达到上述 FunnyOS 应用验收标准。

## 4. FunnyOS 实测证据

MORE 和短输入嵌入**独立探测构建**的 DOS.IMG；mtools 的 mcopy 独立提取
两份资源并逐字节确认。默认构建镜像未增加第三方应用。

每个固件与构建组合运行：

```text
dos more.com
echo aftermore1
dos more.com < input.txt
echo aftermore2
```

两个命令都在约 5 秒的内部运行预算之后返回 runner code 1；这个 1 是 VM
失败后的包装返回值，**不是 MORE 正常调用 DOS 终止服务得到的退出码**。
有诊断构建的串口输出会影响运行时间，所以另用无诊断构建复核停止原因。

| 固件 | 构建 | 两次停止原因 | 命令尾证据 | 退出后 Shell |
|---|---|---|---|---|
| BIOS | DOS_TRACE=1 | RUN LIMIT | 空尾 / `< input.txt` 原样在 PSP 中 | 两次 echo 均实际执行 |
| UEFI | DOS_TRACE=1 | RUN LIMIT | 空尾 / `< input.txt` 原样在 PSP 中 | 两次 echo 均实际执行 |
| BIOS | 默认、无诊断 | RUN LIMIT | 仅静态代码依据，不宣称有 trace | 两次 echo 均实际执行 |
| UEFI | 默认、无诊断 | RUN LIMIT | 同上 | 两次 echo 均实际执行 |

有诊断构建观察到：PSP=1000h、入口 CS:IP=1000:0100、SP=FFFE、
FLAGS=F202、环境段=0F00；版本调用返回 AX=1E03，通过 MORE 的 DOS>=2 检查。

### 第一个失败点与后续传播

下表偏移为 guest 段内地址；`caller` 记录的是 INT 后的返回 IP。

| 调用 | INT 所在偏移 | 输入 | 实际返回/后果 |
|---|---|---|---|
| 版本 30h | 0102h | AH=30h | AX=1E03h，通过版本门槛 |
| 启动输出 09h | 0119h | DS:DX=1000:0207 | 启动 CRLF，尚未处理输入 |
| **复制句柄 45h** | **011Fh** | **BX=0000h** | **AX=4500h、CF=1；首个缺失服务** |
| 关闭 3Eh | 0125h | BX=0000h；BP 已为4500h | AX=0006h、CF=1；当前关闭路径只认识 FAT 的文件句柄 |
| 再复制 45h | 012Ch | BX=0002h | AX=4500h、CF=1；没有将 stderr 复制到刚释放的 stdin 槽位 |
| 读取 3Fh | 0139h | BX=4500h、CX=1000h、DS:DX=1000:020A | AX=0006h、CF=1，缓冲区仍是 6 个 00 |

原始关键片段：

```text
[DOS-TRACE] #3 AH=45 caller=1000:0121 ax=4524 bx=0000 ...
[DOS-TRACE] #3 after ax=4500 ... cf=1
[DOS-TRACE] #4 AH=3e caller=1000:0127 ... bp=4500 ...
[DOS-TRACE] #4 after ax=0006 ... cf=1
[DOS-TRACE] #6 AH=3f caller=1000:013b ax=3f00 bx=4500 cx=1000 dx=020a bp=4500 ...
[DOS-TRACE] buffer-before 00 00 00 00 00 00
[DOS-TRACE] #6 after ax=0006 ... cf=1
```

MORE 在 `0121h` 执行 `MOV BP,AX`，没有检查复制失败的 CF；读返回后只检查
AX 是否为零，错误码 6 被当成“读到 6 字节”，于是通过 02h 输出六个 NUL，
再回到读循环。后续调用的保存 FLAGS 含 CF，证明失败标志确实传回 guest；
不能把这次失败归因为旧的 flags 丢失缺陷。未观察到 CPU fault。

未实现调用返回 AL=0/CF=1、保留 AH 是当前已有策略；本次**没有改成 AX=1**
或其他返回值来掩盖缺失的服务。

屏幕相关：初始只输出 CRLF，后续读取失败后输出 NUL；不是预期文件文本。
工具保存了运行后 Shell 的截图，但**本次没有对 MORE 画面做逐像素断言**。

## 5. 兼容性问题清单

“实测阻塞”与“静态确认但尚未执行到”必须分开。

| 编号 | 问题 | 证据等级 | 下一轮所需工作 |
|---|---|---|---|
| G1 | AH=45h 未实现 | 四次诊断运行直接观察，四次普通运行复核失败 | DOS 句柄复制；别名共享文件位置与底层对象生命周期 |
| G2 | 标准句柄不能按普通 DOS 句柄关闭/重绑定 | 3Eh/BX=0 直接返回错误6；FAT get() 仅接受>=5 | 统一 DOS 句柄表；close(0) 后 duplicate(2) 必须能复用空槽0 |
| G3 | Shell 无 DOS stdin/stdout 重定向 | `< input.txt` 原样留在 PSP；cmd_dos 仅拼接参数 | 提取 `<`/`>` 并在装载前绑定端点，保留其余正常参数 |
| G4 | 字符输出绕过 stdout 句柄 | 静态：02h/09h 直接走 bios10_tty；40h 对1/2特殊直写 | 让需遵循重定向的输出经过 DOS 输出端点 |
| G5 | 分页输入 AH=0Ch 未实现 | 二进制01BCh含调用；当前分发无0Ch，**本次未走到** | flush+AL 子功能输入，确认 console/BIOS 队列及扩展键状态的语义 |
| G6 | 交互程序仍受固定时间/步数预算 | 本次八次都被 RUN LIMIT 收回 | 测试与交互模式分离；保留宿主中止和资源归还机制 |

本轮没有发现需要先做 MZ/EXE、DOS EXEC、TSR、FCB、图形或硬盘持久化才能
进入 MORE 的短输入工作流。此结论仅针对已检查的目标，不推广到其他应用。

## 6. 工具、产物与复现

新增文件：

- `dos/apps/msdos2-more.json`：固定应用、来源、二进制/源码哈希与依赖。
- `tools/probe-dos-app.py`：验证 Git 对象、参考基线、独立资源提取、两种构建、
  QMP 驱动 Shell、trace 判定、恢复检查和结构化结果。
- `tools/tests/test_probe_dos_app.py`：18 个判定器测试；坏哈希、错误CF、错误调用
  位置、错误句柄、缺少内存观察、只有键盘回显、外部超时和 panic 都会拒绝。
- `user/vm/vm.c` 的 `VM_DOS_TRACE`：只读、前16次服务日志与调用计数；记录
  寄存器、guest INT 帧、输入缓冲区、PSP 命令尾和最终状态，不改变服务结果。
- `Makefile` 的 `DOS_TRACE=1`：仅 opt-in。两种构建必须用不同 BUILD_DIR，
  编译选项变化不是对象依赖，不能在同一对象目录切换后假装已重建。

结构化执行结果快照：[`M5-app-probe-results.json`](M5-app-probe-results.json)。
完整产物在 `/var/tmp/funyos-build/m5-app-probe/`：

```text
reference/{empty,short,controls,ctrlz,wrap,near-page}/
  MORE.COM, LICENSE, INPUT.TXT, OUTPUT.TXT, dosbox.log
qemu/{trace,plain}-{bios,uefi}/
  serial.log, run-{1,2}.log, qemu.log, command.json, shell-after.ppm
trace-build/、plain-build/：独立内核、用户态、镜像与 ISO
MORE.disasm、results-all.json
```

在 WSL 仓库根目录执行：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools/tests -v

# 本机此前解包的 DOSBox；没有全局安装，也不自动下载依赖。
DOSBOX=/var/tmp/funyos-dosbox-runtime/root/usr/bin/dosbox \
LD_LIBRARY_PATH=/var/tmp/funyos-dosbox-runtime/root/usr/lib/x86_64-linux-gnu \
PYTHONDONTWRITEBYTECODE=1 python3 tools/probe-dos-app.py --phase all

# 可分开跑，但只有 all 的报告同时包含参考与 FunnyOS 证据。
python3 tools/probe-dos-app.py --phase qemu --firmware bios uefi
# reference 阶段同样需要上面的 DOSBOX/LD_LIBRARY_PATH。
```

工具返回0只表示**调查断言完成**，结构化结果明确标为
`application_compatible=false`，不是宣称 MORE 兼容。依赖缺失、编译失败、
QMP/sendkey 失败、超时或诊断不符都会返回非零；不会用它们替代已知缺口。

本轮验证：

- 18 个 Python 调查判定器测试通过。
- 六个 DOSBox 短输入基线精确匹配；八次 QEMU 调查均确认预期缺口和 Shell 恢复。
- 两份独立构建的镜像资源由 mcopy 提取确认；默认用户态二进制不含诊断字符串。
- 普通构建完整 **`make check` 返回0**，全部宿主套件、原有 QEMU 回归与
  W5/W6 双固件资源/像素检查通过。日志：`/var/tmp/funyos-m5-app-regression.log`。
- `git diff --check` 通过。

应用的预期失败不加入普通 `make check`，避免把调查工具的绿灯误读成应用已验收。

## 7. 本次没有证明的事

- MORE 的完整交互分页、长文件、任意输入及 Ctrl-C 行为。
- 原版 MS-DOS 2.0 系统环境下的结果；当前外部参考是 DOSBox。
- FunnyOS 与 DOSBox 同输入的最终输出等价：FunnyOS 连输入端点绑定都还缺，
  当前建立的是参考合同与失败原因，而不是一个通过的差分兼容性结论。
- guest 正常退出码、标准设备全部语义、强制中止或故障时完整资源清理。
- 独立交叉审查、物理真机验证、任意现成 DOS 应用兼容性。
