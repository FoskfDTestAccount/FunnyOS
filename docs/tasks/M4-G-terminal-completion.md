# 用户可创建的多标签终端 —— 第一版实现与验收

## 交付与边界

本轮实现用户澄清的目标：**先做多个独立命令行终端，不做图形桌面。**
普通启动路径创建 `Terminal 1`；每个标签有一份独立 Ring 3 FunnyCOM 进程、
私有地址空间、用户/内核栈、行输入调用和输出缓冲。它们不是一份 Shell 的页面副本。

这是**协作式、前台会话调度**，不是通用抢占式多任务：

- 等待 stdin 的 Shell 可挂起，其他会话可执行；半行输入继续留在该 Shell 的栈和地址空间。
- 切到别的会话会暂停原会话的执行上下文；切回来恢复，不承诺后台持续计算。
- SYS 调用入口及阻塞输入等待是切换安全点；完全不调用内核的任意原生 Ring 3
  死循环仍不能被这版会话切换机制抢占。
- DOS 解释器定期进行宿主调用，能在 guest 忙或等待输入时切会话。
  提供显式停止 DOS 的宿主快捷键；不把取消模拟器说成通用进程强杀。
- 第一版是有限屏幕行缓冲与滚动；没有无限 scrollback、命令历史编辑或会话工作目录。
- DOS 使用各 Shell 进程的独立 VM 全局状态与私有镜像副本；不新增共享磁盘持久化语义。

## 用户操作

使用普通镜像：`C:\FunnyOS\dist\funyos.iso`，WSL `/mnt/c/funnyos/dist/funyos.iso`。
另存同一镜像：`/mnt/c/funnyos/dist/funyos-terminal-preview.iso`。
不要用旧的 `funyos-m4g-preview.iso` 体验这项功能；旧文件仍是 `selftest=desktop` 夹具。

| 操作 | 入口 |
|---|---|
| 新建终端 | 标签栏 `[+]` / `Ctrl+Shift+T` / `TAB NEW` |
| 切换终端 | 点击标签 / `Alt+1..8`（按当前显示顺序） |
| 关闭终端 | 标签上的 `x` / `Ctrl+Shift+W` / `TAB CLOSE` / 当前 Shell 的 `EXIT` |
| 内核日志 | 标签栏 `[Log]` / `F12`；再按 `Alt+数字` 返回终端 |
| 终端数量与自身身份 | `TAB` |
| 资源/中断栈诊断 | `TAB STATS` |
| 停止当前 DOS 运行 | `Ctrl+Shift+K` |
| 浮点和嵌套进程检查 | `CHECK` |

- 最多八个用户会话。关闭非当前页会选择该页再关闭，并选择仍存活的会话。
- 忙于 DOS 或嵌套子程序的会话拒绝直接关闭；提示显示在状态栏，不破坏待输入行。
  先让程序正常退出，或使用停止 DOS 快捷键，再关闭。
- `EXIT` 只结束所在 Shell，会话资源随之回收。最后一页退出/关闭后创建空白替代会话；
  若分配失败，保留状态提示与 `[+]` 重试入口。
- 槽位名字可能复用（例如 Terminal 1），快捷键始终按当前可见标签的顺序，
  不把名字中的槽位数字当快捷键编号。
- 仍要求 I/O APIC 和 PS/2 Mouse。没有 USB Tablet 或传统 PIC 输入后备支持。

## 实现位置

- `kernel/console/terminal.c` / `kernel/include/funnyos/terminal.h`：
  每会话的字符页、光标、翻译键队列、原始扫描码队列、guest 快照、鼠标事件、
  标签布局和状态提示；输出从权威 RAM 重绘，绝不读取 framebuffer。
- `kernel/proc/process.c`：每会话独立 runner 栈及可恢复上下文；等待和切换进入中立
  scheduler 栈。保存/还原当前进程、PML4、TSS `rsp0`、x87/SSE；释放资源只在中立栈上。
  现有阻塞父子 `process_run()` 仍保留，子进程继承会话并可在嵌套调用中挂起/恢复。
- `kernel/console/console.c` / `kbd.c` / `screen.c` / `kernel/proc/syscall.c`：
  用户标准输入、输出、清屏、guest 页面和原始键盘路由到进程所属终端；
  `kprintf()` 继续写独立日志/串口；旧 selftest 路径继续走原显示页模型。
- `SYS_TERMINAL` 与 libu：新建/关闭、数量/身份及诊断/取消接口。
- `user/funnycom/main.c`：`TAB` / `CHECK`；普通 Shell 每页各运行一份。
- `user/vm/vm.c`：交互 DOS 不再因人等待/切页超过五秒而被测试预算误杀；
  自检、W5/W6 资源与 MORE 探测路径仍保留原有预算。停止快捷键恢复该页 Shell。

键盘焦点变化时，为旧 raw 持有者补齐已送出的按键释放，防止 stuck key；
Ctrl/Shift/Alt 的 make 在普通 guest chord 可判定后再送出，因此宿主动作不会先污染 guest。
原始队列溢出仍明确报错，而不是静默复用旧会话的输入。

## 已完成验收

### 宿主（ASan/UBSan）

```bash
bash /mnt/c/funnyos/tools/test-desktop-host.sh
```

现有桌面套件加会话内容/清屏隔离、左右 Alt 与新建快捷键、原始扩展键、
guest 恢复、会话上限及槽位复用、软换行边界退格：773,079 条检查通过。
最终宿主日志 `/var/tmp/funyos-terminal-final-host.log`（退出 0）；全量回归中的较早宿主运行
为 772,951 条，之后新增了 128 个换行退格像素断言，内核/ISO 没有再改变。
实际 I/O APIC MMIO 探测的无设备/零/有效版本测试也通过。

### QEMU 普通启动：BIOS 与 UEFI

```bash
make test-terminals
```

`tools/run-terminal-test.py` 使用普通 ISO，不要求 selftest；两条固件路径已通过：

- A 的半行命令经过 B 的新建/输入/清屏/退格后完整恢复。
- 每个字符页截图检查 659,456 个可见文本像素，检查输出存在与其他会话内容不存在。
  光标的最后两行单独排除于 glyph 解码，不声称该 oracle 独立验证字体设计或所有 cursor 像素。
- A 中 DOS 等待时 B 可运行；切走超过五秒再回来仍能通过真实键盘验收。
- A 中 DOS 的状态不会被 B 中运行另一份 DOS 程序覆盖。
- 忙页关闭拒绝、停止 DOS、回到原 Shell、左右快捷键隔离。
- 嵌套子进程执行期间切到其他会话，回来正确恢复；浮点结果正确。
- 鼠标新建与关闭、键盘新建与关闭、`EXIT`、八页上限、复用和最后一页替代。
- 反复关闭/新建后 PMM used pages 与 heap in-use 精确回到相同数值；
  Ring 3 中断有实际样本，off-stack 计数为零。

最终日志 `/var/tmp/funyos-terminal-final-check.log`；截图在
`/var/tmp/funyos-terminal-final/terminal-test/bios/` 和
`/var/tmp/funyos-terminal-final/terminal-test/uefi/`。

### VirtualBox BIOS 普通启动

```bash
PYTHONDONTWRITEBYTECODE=1 python3 /mnt/c/funnyos/tools/run-vbox-desktop-test.py \
  --terminals --iso /mnt/c/funnyos/.cache/terminal-vbox/final.iso
```

只创建/删除脚本自己的临时 VM，不修改用户的 ReForgeOS。
使用公开 COM 鼠标接口注入真实 PS/2 相对事件；验证鼠标新建/关闭、半行命令恢复、
A 中 DOS 等待时 B 命令执行、DOS 返回、槽位复用与 EXIT 后其他 Shell 继续工作。
七张文本截图通过像素与会话隔离检查，开/关 I/O APIC 的诊断也通过。
最终 ISO 已复测，退出 0。日志 `/var/tmp/funyos-terminal-final-vbox.log`；实测图片与串口
`/mnt/c/funnyos/.cache/m4g-vbox/1b97f2a5/`。
不据此声称 VirtualBox UEFI 或 VMware 新终端验收；后者仅有用户此前的鼠标反馈。

### 全量回归

`make check` 已接入宿主桌面、G1–G4 BIOS/UEFI 与新终端 BIOS/UEFI 套件，
并保留原 DOS 宿主、启动、故障、输入、进程、VM、屏幕和 W5/W6 资源回归。
2026-10-02 最终隔离目录从零构建成功、无编译警告；全量回归退出 0。
构建日志 `/var/tmp/funyos-terminal-final-build.log`；全量日志
`/var/tmp/funyos-terminal-final-check.log`。可复现命令：

```bash
make -j4 BUILD_DIR=/var/tmp/funyos-terminal-final all
FUNYOS_BUILD_DIR=/var/tmp/funyos-terminal-final \
  make BUILD_DIR=/var/tmp/funyos-terminal-final check
```

已导出普通启动 ISO 与同内容的 terminal-preview 别名，均为 4,739,072 字节；
SHA-256 `1a2829ec4ce7d26bc128526a52e313638ec20c2bb700dc18fafca185ce627db1`。
构建说明 `/mnt/c/funnyos/dist/terminal-preview-build.json`；实测截图
`/mnt/c/funnyos/dist/terminal-preview.png`。

## 尚未覆盖

- 人为注入各种 PMM/heap 失败阶段尚未做穷举；八会话上限、资源回收与重新分配已经验证。
- 真实鼠标连续高频压力、缩放/动态分辨率与极小 framebuffer 没有完整验收。
- 任意原生 CPU 密集进程的抢占、SMP、后台持续执行、通用进程强杀均不在本版支持范围。
