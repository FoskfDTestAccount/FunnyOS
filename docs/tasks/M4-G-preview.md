# M4-G 开发预览 ISO（2026-10-01）

这是一份供查看效果的**开发快照**，不是正式发行，也不代表整个 M4-G 包已经结项。
仓库保留先前 M5 MORE 调查改动；当前镜像的 DOS_TRACE 关闭。

**方向更新（2026-10-01）：** 用户已反馈鼠标恢复正常；下一步优先实现用户可创建/关闭的
多终端会话，不推进窗口式 GUI。此 ISO 仍只有固定测试页，**没有** `[+]` 新终端入口、
多个独立 FunnyCOM 或关闭会话能力。实施与验收顺序见
[`M4-G-terminal-sessions.md`](M4-G-terminal-sessions.md)，不可把本预览标成多终端完成版本。

## 新终端体验入口

普通 `funyos.iso` 已加入用户新建/关闭终端及独立 Shell，会话功能说明见
[`M4-G-terminal-completion.md`](M4-G-terminal-completion.md)。
本文件描述的 `funyos-m4g-preview.iso` 仍是旧 `selftest=desktop` 验收夹具，
不要把固定 Log/Program 演示与现在的普通启动终端混为一谈。

## 文件

- 普通启动：`C:\FunnyOS\dist\funyos.iso`（WSL：`/mnt/c/funnyos/dist/funyos.iso`）。
- 桌面预览：`C:\FunnyOS\dist\funyos-m4g-preview.iso`，设置 `selftest=desktop`。
- 字节数与 SHA-256：`/mnt/c/funnyos/dist/m4g-preview-build.json`。

不要使用仓库根目录遗留的 ISO，它们不是这次导出的产物。

## 预览操作

1. 以光驱方式挂载桌面预览 ISO；BIOS、UEFI 都可。
2. 启动后有 Log 与 Program 两个标签，可用 Alt+1/Alt+2，或鼠标左键点击切换。
3. 鼠标首次移动后显示指针；鼠标不能靠切换标签去向隐藏程序输入。
4. 在当前预览程序页按 Enter，启动子程序，出现第三个标签，可用 Alt+3。
5. 在子程序页按 Enter，子程序退出；切回父程序页再按 Enter，预览结束。
6. 普通启动 ISO 进入 Shell，默认只显示日志页，因此起初不显示标签栏。

这是**多标签文本控制台与鼠标指针**，不是窗口式 GUI：蓝色区域是 80×25 文本页，
没有可拖动窗口、图标、任务栏或 GUI 控件。

这是验收程序，不是多任务终端：可重入/阻塞式父子进程仍是当前运行模型。
切换显示页面不创建 Shell 会话，也不是新进程调度器。

## VirtualBox 必须开启 I/O APIC

FunnyOS 当前将传统 8259 PIC 屏蔽，键盘 IRQ 1 与鼠标 IRQ 12 都通过 I/O APIC。
因此 **PS/2 Mouse 与 I/O APIC 必须同时启用**；只换成 PS/2 鼠标还不够。

在虚拟机完全关机后，VirtualBox 设置：

- 系统 → 主板 → 启用 I/O APIC。
- 系统 → 主板 → 指点设备 → PS/2 鼠标（不要选 USB Tablet）。

也可在 Windows PowerShell 执行（这是用户操作说明，本轮没有修改 `ReForgeOS`）：

```powershell
& "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" modifyvm "ReForgeOS" --ioapic on --mouse ps2
```

2026-10-01 在独立临时 VirtualBox VM 中使用同一 ISO 做了开/关对比：
关闭时无鼠标/键盘事件；开启时实际收到 `dx=7 dy=-3`，且 Alt 切页和点击切页正常。
此前内核会把缺失 I/O APIC 的 MMIO 读值 `0xffffffff` 误报成版本 255、240 条线；
本轮修复后报告 `I/O APIC: NOT AVAILABLE`，键盘和鼠标也不再虚报可用。
这是配置要求，不是已实现传统 PIC 后备支持。

## 交互预览的键盘报错修复

旧预览把首个非 Enter 字节一律误报为 `G raw switch leaked: FAIL`。
`e0` 只是扩展扫描码前缀，不能单凭它断定 Alt 快捷键泄漏。
新预览记录完整原始输入，普通扩展键与释放事件不再终止演示；
队列错误或实际 Alt+数字泄漏仍报告失败。
QEMU 自动验收仍严格要求三个 Enter 字节，不因交互容错而放宽快捷键隔离的断言。

## 本轮测试

编译全量重建并导出期间，宿主桌面套件在 ASan/UBSan 下通过。
新桌面 QEMU 套件在 BIOS/UEFI 下均通过；覆盖实际鼠标位移和点击、
原始键盘快捷键隔离、嵌套页面以及 presenter 内存改写后仍显示内核快照。
每张 guest 验收图检查 256,000 个页面像素，另外检查标签和指针。

2026-10-01 本轮完整 `make check` 已成功退出（exit 0），包含宿主 DOS 套件、
BIOS/UEFI 启动、故障、输入、用户进程、VM、屏幕与 W5/W6 资源回归。
日志：`/var/tmp/funyos-m4g-vbox-check.log`。
桌面宿主 ASan/UBSan 套件通过 772,278 条检查，I/O APIC 空设备/零值/有效版本
探测测试也通过。

VirtualBox 自动测试（Windows VirtualBox + PowerShell + WSL Python/Pillow）：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 /mnt/c/funnyos/tools/run-vbox-desktop-test.py
```

脚本只创建、启动、删除自己的临时 VM，不改变用户已有 VM。
使用公开 COM 相对鼠标接口注入真实 PS/2 事件，验证：
无 I/O APIC 的诊断、鼠标位移与 Y 方向、指针像素、页面恢复、点击标签、
左右 Alt+数字隔离、隐藏页键盘隔离、普通扩展扫描码、嵌套子页与退出。
两张截图各检查 256,000 个 guest 像素，并检查标签与覆盖指针。
本轮已通过，日志 `/var/tmp/funyos-m4g-vbox-acceptance.log`；
截图和串口在 `/mnt/c/funnyos/.cache/m4g-vbox/27819603/`。
这验证 VirtualBox BIOS 路径，不声称完成 VirtualBox UEFI 验收。

用户提供的 VMware 截图已有实际鼠标移动与按键事件，属于人工运行证据，
但旧预览随后因为收到 `e0` 退出。没有进行 VMware 完整像素/标签生命周期自动验收，
也没有验证本轮新 ISO 在 VMware 的完整交互，因此不将其记为完整 G1-G4 验收。
