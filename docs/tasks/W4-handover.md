# W4 交接文档:`INT 21h` 核心

> **历史交接记录**：下文描述交接当时的状态。W4–W6 已于 2026-10-01 继续完成，最终实现、验证和边界见 [W4-W6-completion.md](W4-W6-completion.md)。原文中的“没做完”不是当前状态。

**写这份文档时的工作区状态**:W4 功能上完成,`make check` 全绿,**一行都没提交**。W5、W6 没开始。

这份文档是给接手的人的,不是给历史看的。它记三件事:**改了什么、怎么自己验一遍、以及踩过的坑**。看懂了就可以直接往下做,不需要原始对话。

> 里程碑本身的计划在 [`M5-README.md`](M5-README.md)。这份文档里的「做完之后」那一节**还没回填到那里**,接手的人收尾时要一起补。

---

## 一、三十秒版本

W4 做的是 DOS 的函数分发器:一个 `.COM` 能执行 `INT 21h` 拿到答案。做了 12 个功能码——版本、字符串和字符输出、两种终止、读写中断向量、设置和读取 DTA、驱动器选择。验收是一个打了 9 行答案的 `.COM`,既在宿主套件里逐字节比对,也被截屏后在真机屏幕上逐格比对。

**做这件事的过程里挖出一个比它本身严重得多的构建系统缺陷**,记在第五节。那个缺陷会让内核或用户态程序**一半的目标文件用旧结构体、一半用新结构体**,编译不报错,运行时静默出错。已经修好并验证。

另有一件没做完:**最后一次注入验证少跑了一条**(第六节)。

---

## 二、先自己验一遍

**所有命令都在 WSL 里跑,不要在 Git Bash 里跑**(Git Bash 调 `wsl.exe` 要 `MSYS_NO_PATHCONV=1`,否则 `$` 被吞、`/mnt` 被改写)。构建产物在 `/var/tmp/funyos-build`,**不要放到 `/mnt/c`**——9p 会留下删不掉的幽灵文件。

```bash
cd /mnt/c/FunnyOS && make check
```

七条路径全绿,最后一条是屏幕测试,52 条断言。约五分钟(含七次 QEMU 启动)。

```bash
cd /mnt/c/FunnyOS/dos && make -B test
```

二十一个宿主套件。**`-B` 不是可选的**:这个仓库有「同秒不重建」的历史,加 `-B` 才是真的重新编译。`int21` 那一行应该是 `73 check(s), 0 failure(s)`。

单独看 W4:

```bash
cd /mnt/c/FunnyOS/dos && make test-int21
```

```bash
cd /mnt/c/FunnyOS && python3 tools/inject-int21.py
```

注入验证,**约二十分钟**(每条注入都全量重建)。这一条会逐条告诉你每次注入红的时候说的是不是对的。

只想看屏幕上长什么样:

```bash
cd /mnt/c/FunnyOS && FUNYOS_BUILD_DIR=/var/tmp/funyos-build bash tools/run-screen-test.sh /var/tmp/funyos-build/funyos.iso
```

---

## 三、工作区里有什么

### 新文件

| 文件 | 是什么 |
|---|---|
| `dos/include/vm86/int21.h` | `INT 21h` 的接口:功能码、错误码、驱动器取值、`struct int21_state`、要注册的两个服务 |
| `dos/dos/int21.c` | 分发器本体,12 个功能码 |
| `dos/corpus/dos/int21.asm` | W4 的验收程序:483 字节,打 9 行答案,以 `INT 21h AH=4Ch` 退出,返回码 7 |
| `dos/tests/test_int21.c` | 20 个用例、73 条断言,含一条真机回放机器上的整页比对 |
| `tools/inject-int21.py` | 注入驱动:从 `verify-mutations.py` 读缺陷表,逐条核对「红的时候说的是不是对的」 |

### 改动的文件(按用途分组)

**终止程序的通路**(这是 W4 唯一动到公共接口的地方):

- `dos/include/vm86/ops.h` — `enum vm86_result` 加 `VM86_EXIT`
- `dos/include/vm86/cpu.h` — `struct vm86_cpu` 加 `exited` / `exit_code`
- `dos/include/vm86/host.h` — `enum vm86_stop` 加 `VM86_STOP_EXIT`;新增 `vm86_service_exit()`、`vm86_flags_written_back()`、`vm86_flags_declined()`
- `dos/intr/trap.c` — 服务结束后返回 `VM86_EXIT`;两处诊断计数器
- `dos/intr/run.c` — `VM86_STOP_EXIT`
- `dos/corpus/bios/replay.h` / `.c` — `VM86_BIOS_EXITED`、`vm86_bios_exit_code()`,注册两个 DOS 服务,装载后修正 DTA
- `dos/dbg/trace.c`、`dos/tests/test_bios_programs.c` — 被 `-Wswitch` 逼着补的 `case`
- `dos/include/vm86/dos.h` — DOS 的中断向量号(20h–24h);**修了一处把 AL/AH 写反的注释**

**控制台输出**:

- `dos/bios/bios10.h` / `.c` — 新出口 `bios10_tty()`,把原本 static 的 teletype 暴露出来给 DOS 层共用

**接线**:

- `user/vm/vm.c` — 注册 `INT 21h` 和 `INT 20h`;`machine_build_dos()` 多一个 `path` 参数;`drive()` 认 `VM86_STOP_EXIT`;新增 `vm_int21_hold()`
- `user/vm/vm.h`、`user/vm/corpus.h`、`user/funnycom/main.c`、`kernel/main.c` — `ARG_VM_INT21` / `vm=int21`
- `Makefile` — 语料加进 `VM_CORPUS_DOS_SRC`;构建系统修复(第五节)
- `dos/Makefile` — 测试套件的头文件依赖(第五节)
- `tools/run-screen-test.sh` — 第三次截屏及其断言

**依据和验证**:

- `docs/dos-refs-dos.md` — 新增第九节:`INT 21h` 的功能表、返回约定、错误码、未实现功能码的行为、终止那一节
- `tools/verify-mutations.py` — 新增 `M5_MUTATIONS`(17 条)和 `dos` 层
- `dos/tests/test_mov.c` — 新增 `mov leaves the flags alone`

### 依赖顺序

`dos-refs-dos.md` 第九节**先写的**,代码后写。这不是洁癖:没有依据文件时,装载器和测试会一起错——W3 已经吃过一次(PSP 的 FCB 空位本该是空格,装载器和测试都以为是零)。

---

## 四、W4 的设计决定

每一条都写在代码里、也写进了依据文件。这里挑出接手的人最需要知道的四条。

### 1. 服务住在 VM 进程里,只有真实资源才走系统调用

M5-README 第一节裁定的。`AH=02h`(打印一个字符)完全本地:它写的是 guest 自己的显存,而 guest 显存就在 VM 进程的地址空间里,内核根本不需要知道。`AH=3Dh`(打开文件)需要,那是 W5 的事。

推论:**分发器那部分大而会涨**(几百个功能码),**跨边界那部分小而稳定**。

### 2. 控制台输出共用 BIOS 的 teletype

`bios10_tty()`。真机上 DOS 的 `AH=02h`/`09h` 就是调 BIOS 的 teletype 例程,共用一份实现是还原事实,不是走捷径。两份实现意味着 CR/LF/BS/TAB 有第二个地方可以错。

### 3. 终止:一条新的跨层通路

`vm86_service_exit()` 设一个标记 → `vm86_host_trap()` 返回 `VM86_EXIT` → `vm86_step()` 往上传 → 运行循环返回 `VM86_STOP_EXIT`。

**故意加进 `enum vm86_result` 而不是改服务签名**:`-Wswitch` 会把每一个消费方逼出来。它确实逼出来了两个(`trace.c` 和 `test_bios_programs.c`),那两个都是旧代码里用 `switch` 的地方。**但 `if`/`else` 链它管不了**,`user/vm/vm.c` 里的 `drive()` 就是我手动改的。

**没做的两件事**记在依据文件里:不走 `INT 22h`(没有父进程,那个地址指向一段没意义的代码),不释放内存(没有分配器,`AH=48h` 在 M6)。

### 4. 未实现的功能码

**`AL = 0` 并置 CF,AH 不动**。这是查过的:FreeDOS 现在这么做;它曾经返回 `AX = 1`,那一版**把 4DOS 弄坏了**(程序看到 CF 置位会读 AX,而 `AX=1` 和一个字节的 0 对调用方不是一回事)。来源在依据文件第九节。

**成功一律清 CF**,包括不文档化 CF 的功能。这是**本机决定,不是事实**,依据文件和代码里都这么标了。

---

## 五、最重要的发现:构建系统静默半重建

### 症状

Ring 3 里跑 W4 的验收程序,屏幕上打出来的是:

```
select A:   = 000F CF=0
function 55 = 5500 CF=0
```

宿主套件里同样的程序、同样的字节打的是 `CF=1`。**同一个解释器源码,两台机器,两个答案。**

### 真正的原因

顶层 `Makefile` 的内核和用户态规则,每个 `.c` 编译成独立的 `.o`,**并且没有任何头文件依赖**:

```make
$(USER_OBJ_DIR)/%.c.o: %.c
	@$(CC) $(USER_CFLAGS) -c $< -o $@
```

W4 往 `struct vm86_cpu` 里插了两个字段(`exited` / `exit_code`,插在 `halted` 后面)。于是:

- `dos/dos/int21.c` 是**新文件**,用新头文件编译;
- `dos/intr/deliver.c` **没被编辑过**,`make` 认为它是最新的,**用旧头文件编译的 `.o` 被留着**;
- 两边对 `intr_frame_sp` 在结构体里的偏移**不一致**。

后果:中断帧的栈指针记录**永远对不上**,每一条服务的标志写回都被静默跳过。实测计数器是 `404 declined, 0 written back`。

**宿主套件为什么是绿的**:`dos/Makefile` 把全部源码**放在一条 `cc` 命令里**编译,所以永远自洽。**半重建的二进制什么都不报**——它只是行为微妙地错了。

### 修了什么

1. **顶层 `Makefile`**:`CFLAGS` 和 `USER_CFLAGS` 加 `-MMD -MP`,末尾 `-include $(DEPS)`。
2. **同一处加了 `.DEFAULT_GOAL := all`。** 这一条是必须的,不是保险:`-include` 会把 `.d` 里的规则插到 Makefile 自己的 `all:` 之前,make 拿第一个见到的目标当默认目标,于是 `make` 变成「构建 `acpi.c.o`,报告 up to date,退出 0」。**这个坑我踩了**,所以写在这里。
3. **`dos/Makefile`**:`HEADERS := $(shell find . ../libk/include -name '*.h')` 作为每个套件的普通依赖。原来写的是 `| corpus/bios/replay.h`,**order-only 前提根本不会触发重建**——注释说的和做的是相反的,三个里程碑都这样。注释已经改正。

### 怎么确认修好了

```bash
cd /mnt/c/FunnyOS && touch dos/include/vm86/cpu.h && sleep 1 && make 2>&1 | grep -cE '^  CC |^  CCu'
```

应该是二十几个(不是 0)。随后再 `make` 一次应该报 `Nothing to be done for 'all'`。

**`sleep 1` 不能省**——同秒的时间戳 make 判不出来,这正是会让人以为「修了也没用」的地方。

### 顺带加的哨兵

`vm86_flags_written_back()` / `vm86_flags_declined()`(`dos/include/vm86/host.h`)。

理由:**从 guest 那一侧这两件事长得一模一样**——服务用进位标志回答「不行」,和服务设的进位标志被丢掉,程序看到的都是 CF 的某个值,它分不出来。所以机器得自己说。

- `user/vm/vm.c` 只在 `declined != 0` 时打一行警告(所以**没有输出就是全对**);
- `dos/tests/test_int21.c` 在验收用例里直接断言「有写回,且零丢弃」,一毫秒。

`declined` 非零**不一定是错**——TSR 用远调用链进 stub 时栈上只有两个字,那时拒绝写入正是对的。断言里写清楚了这一点。

---

## 六、没做完的

### W5 和 W6:完全没动

- **W5** FAT 镜像挂载,以及 `3Dh`–`42h`(打开/关闭/读写/定位)、`4Eh`/`4Fh`(查找)。
  **这些功能码是故意留给 W5 的**:它们背后需要文件系统,而 W5 反正要建一套;先写了的那一套就是测试会照着写的那一套。
- **W6** 键盘来源(set-1 扫描码 + 非阻塞读键)。W4 的输入类功能码(`01h`/`06h`/`07h`/`08h`/`0Ah`/`0Bh`)一起留在这里,理由写在 `int21.h` 头部——**加一条没有调用方、也测不了的接口,就是「套件绿在一个从没被测过的实现上」的另一种写法**。

### W4 自己的收尾

1. **最后一次注入验证少跑了一条。** 完整跑过的是 16/16 全中;之后往 `M5_MUTATIONS` 加了第 17 条(`the trap never writes a service's flags back`),**没来得及重跑**。接手的人第一件事应该是跑一遍 `python3 tools/inject-int21.py`。
2. **`README.md` 和 `docs/tasks/M5-README.md` 的「做完之后」都没写。** W1–W3 都有那一节,记的是「计划里没写、动手时才定下来的」东西。W4 的这一节现在散在这份文档里,收尾时要搬过去。
3. **`docs/dos-refs-dos.md` 第八节**仍然写着「W4 开始写」,要按实际覆盖范围更新。

---

## 七、踩过的坑(按踩到的顺序)

1. **构建产物绝不能放 `/mnt/c`。** 9p 挂载上 WSL 写坏的文件,Win32 看得到、WSL 和 MSYS `stat()` 报 ENOENT、还删不掉。仓库根目录那两个 `funyos.elf` / `funyos.iso` 就是这么来的。
2. **`make` 同秒不重建。** 改完文件立刻 `make`,时间戳分不出先后。用 `sleep 1` 或 `make -B`。**这一条直接导致了第五节那个 bug 被发现之前我一度以为「改了也没用」。**
3. **NASM 的宏必须先定义后使用。** 宏定义在调用点之后,NASM **不报错**,把那一行当成一个标签,**什么都不生成**。我因此白追了一轮:诊断代码根本没跑,`[g_cf]` 里是上一个用例的陈旧值,而看起来像是「进位标志丢了」。**以后写 .asm 里的诊断代码,先 `ndisasm` 看一眼它真的生成了。**
4. **宿主套件和 Ring 3 用的是同一份源码,但不是同一个二进制。** 宿主自洽不代表真机对。`tools/run-screen-test.sh` 是唯一看得到真机的地方,**W4 里这一条是被它救的**。
5. **`tools/verify-mutations.py` 的路径相对 `dos/`**,不是相对仓库根(`dos/int21.c`,不是 `dos/dos/int21.c`)。`layer_of()` 里已经加了 `dos` 层。
6. **`tools/inject-int21.py` 会 `importlib` 加载 `verify-mutations.py`**,所以会在 `tools/` 下留一个 `__pycache__/`(已经删过一次了,留意别提交进去)。
7. **从 Git Bash 调 `wsl.exe` 要 `MSYS_NO_PATHCONV=1`**;`git push` 只能在 Git Bash 里做(WSL 里没有 git 凭据)。本地 `main` 现在领先 `origin/main` **2 个提交**(`38b6ee8` W2、`346f83a` W3),都没推。
8. **`-Wswitch` 能逼出 `switch` 的消费方,逼不出 `if`/`else` 链。** 加枚举值时两处都要手动翻。
9. **`make` 的默认目标会被 `-include` 进来的规则偷走。** 见第五节第 2 条。

---

## 八、建议的下一步

1. **先跑 `python3 tools/inject-int21.py`**(约二十分钟),确认 17/17。
2. **把 W4 提交**,建议拆成三个:`W4: a .COM can ask DOS for things` / 构建系统修复(单独一个,它值得被单独看见)/ 文档回填。
3. **回填 `M5-README.md` 的 W4「做完之后」**,把这份文档的第四、五节搬过去。
4. **然后才动 W5。**

W5 有一个**必须先定的架构问题**,`docs/DESIGN.md` 没有回答,`M5-README.md` 第二节列了两条路并倾向 (a)。**W4 落地之后的实际情况会改变这个判断**,因为 W4 确立了「服务住在 VM 进程里」:磁盘镜像是**资源**,所以它应该像别的一样跨边界,而 FAT 解析应该留在 VM 进程里——那样它才是一个能拿字节数组直接喂的纯函数,能在这个仓库最擅长的地方(宿主套件)被验。这只是接手时的判断,**没有实现过,也没有验过**,不要当成结论。
