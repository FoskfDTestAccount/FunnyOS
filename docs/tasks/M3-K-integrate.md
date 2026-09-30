# 任务 K:让解释器在 FunnyOS 里跑起来

**文件:** `user/vm/`(新建)、`Makefile`(一处:`USER_C_SOURCES`)、`kernel/main.c`(一处:启动参数)
**依赖:** 无。这是 M3 真正欠着的那件事。

> 开始之前先读 `docs/tasks/README.md`。规则、构建方式、完成标准都在那里。
>
> **先同步你的 worktree**:你的分支切出之后 main 上已经合入了 A–E(整个 `dos/` 都在里面)。用 `git -C /c/FunnyOS log --oneline -5` 看主检出状态,把 main 合进来再开始。
>
> **`dos/cpu/ops_*.c` 一个字都不要改。** 任务 F 正在逐行验证那五个文件。你只加文件。
>
> **Makefile 只做追加,不要重排任何现有行。** 你至少要动两处:`USER_C_SOURCES`(收进解译器核心)和 `USER_CFLAGS`(加 `-Idos/include`,否则 `dos/` 里每个文件都找不到 `<vm86/...>` —— **这一处在第一版任务书里漏了,是执行者编译时才发现的**)。任务 G 同时在改同一个文件的 blob 生成那一段,重排会让两边合并时打架。

---

## 一、为什么要做这件事

**这是 M3 唯一一条还没兑现的承诺。**

现在的事实是:`dos/` 是一套**在宿主进程里测试通过的独立库**。主 Makefile 里没有它(顶层的两处 "dos" 出现在一句关于 MS-DOS 保护性 MBR 标志的注释里,和它无关),内核和用户态也没有任何一处引用 `vm86/`。

所以"解释器能执行二进制"目前**只在宿主的测试进程里成立**。FunnyOS 本身还执行不了一条 guest 指令。

DESIGN 的 D4 决定了这件事该长什么样:

> **决策**:8086 虚拟机的解释器运行在 Ring 3,作为一个普通用户态进程。

任务是把这个决策落到能跑。

---

## 二、你要做什么

### 2.1 关键事实:通配已经把门开着

顶层 Makefile 里:

```make
USER_C_SOURCES := $(shell find user -name '*.c' | sort) libk/printf.c libk/string.c
```

**`user/` 下的 `.c` 是自动收进用户态镜像的。** 所以你在 `user/vm/` 下加的文件不需要新的链接规则 —— 这正是这个任务能和任务 G 并行、而不互相踩的原因。

但 `dos/` 不在 `user/` 下,所以**需要改两处**:一处把解释器核心收进来,一处把它的头文件目录放进搜索路径。

```make
USER_C_SOURCES := $(shell find user -name '*.c' | sort) libk/printf.c libk/string.c \
                  $(wildcard dos/cpu/*.c) $(wildcard dos/mem/*.c)
```

**只要 `dos/cpu/*.c` 和 `dos/mem/*.c`。** `dos/tests/` 不进用户态(那是宿主测试的驱动)。

### 2.2 怎么被启动:用现成的"一个参数选模式"机制

内核现在这样启动用户程序(`kernel/main.c` 结尾):

```c
uint64_t arg = 0;
if (strstr(cmdline, "selftest=userfault")) arg = 1;
else if (strstr(cmdline, "selftest=userexit")) arg = 2;
else if (strstr(cmdline, "selftest=fputest"))  arg = 3;
int code = process_run(program, arg);
```

`process.h` 里对 `arg` 的解释是:"一个值放在寄存器里就是启动一个程序的人和程序之间的全部接口 —— 足够让一个程序以不止一种模式被启动"。

**照这个用。** 加一个 `vm=1` 的开关(用 `selftest=` 之外的词,因为 `selftest=` 是 M2 故障注入测试的语义,别把它搅浑),值可以取 4 或其它约定值。
**为什么不做成第二个程序:** 那需要第二个 blob、第二套链接规则,而且 `g_current` 是单个全局变量、`process_run()` 不可重入(见 `kernel/proc/process.c`)—— **"Shell 启动 VM"这条路今天走不通**,那是 M4 之后的事。M3 只需要证明操作系统能执行 guest 指令,不需要证明 Shell 能启动它。这一步是有意跳过的,报告里要写明。

### 2.3 驱动

`user/vm/` 下写驱动,做这几件事:

1. `vm86_ops_build()`,**并且检查返回值**。它是把五张操作码表合并成一张的地方,重复认领会让它返回 false。**再装一个冲突报告器**(`vm86_set_conflict_reporter`)把冲突打到输出上 —— 不装的话冲突是静默的,而失败信息会指向别处。`dos/tests/harness.c` 里有一个现成的写法可以照抄。
2. 一块 guest 内存,用 `vm86_mem_attach()` 接上。
3. 把一段 guest 二进制装进那块内存。
4. 用 `vm86_step()` 驱动它跑,直到停机 / fault / 指令条数上限。
5. 把终态打出来:**八个通用寄存器、四个段寄存器、`IP`、`FLAGS`(逐位展开成名字)**。

### 2.4 guest 内存的大小:先用小的,并写清楚为什么

`vm86_mem_attach()` 要一块**调用方拥有**的连续区域,`mem.h` 的注释说得很清楚:"真正的模拟器把它映射进自己的地址空间,好让 guest 的写落在一个内核能保护的地方"。这一侧的设计是对的,不用改。

**但现在只能用一块不大的静态数组**,因为:

- `process_create()` 从 `PROCESS_CODE_BASE` 起**精确映射镜像长度**那么多页;
- `.bss` 被 objcopy 强制变成镜像里真实的零字节,所以静态数组**会进镜像**;
- 镜像经 `tools/bin2c.py` 变成 C 源码编进内核,每字节 6 个字符。

所以 `static uint8_t guest_ram[16*1024*1024]` 会生成约 **96 MB** 的 C 源码。**不要这么做。**

**这一版用一个够用的尺寸**(64 KiB 到 1 MiB 之间,你定,写清楚理由)。**任务 G 正在同时给加载器加"内存大小"这个概念**(即 ELF 的 `p_memsz`),等它落地,这里改成一块 16 MB 的零区只是改一个数 —— 所以**把尺寸定义成一个具名常量**并写一句注释说明它为什么暂时是这个值、以及谁来把它改大。

### 2.5 guest 二进制从哪来

**嵌在用户程序里的字节数组**,不要在这版去做文件加载 —— `SYS_OPEN`/`SYS_READ` 是有的,但那是 M5 的 `.COM` 加载器要解决的,不是这一版。写一个**手工汇编的小程序**当输入,形状照 `docs/tasks/M3-I-corpus.md` 第二节的约定(`org 0x100`,入口 `CS:IP = 0:0x100`,`SP = 0xFFFE`,`FLAGS = 0x0002`)。哪怕只有十来条指令,也按那个约定来 —— 任务 I 正在造一批同约定的样本,到时候直接换进来。

---

## 三、坑

### 1. 解释器要在**用户态**的编译环境里活下来

它现在只用宿主测试环境编过。两边的差别是有意的(`USER_CFLAGS`: `-mcmodel=small`、`-fno-builtin`、不含 `-mgeneral-regs-only`、`-Ilibk/include`)。

已经确认的一点是好的:`dos/cpu/cpu.c` 用的是 `<libk/string.h>` 和 `memset`,**不是宿主 libc** —— 它本来就是按"编进这棵树"写的。但用户态那份 libk 只编了 `printf.c` 和 `string.c` 两个文件,如果 `dos/` 用到了这两个之外的东西,链接会失败。**先编一次看结果**,不要猜。

### 2. 不要用 `malloc`

用户态没有堆。"用户态指针在读之前就被检查"那套机制(见 DESIGN 第六节)保护的是程序传给内核的指针,不提供分配。guest 内存和所有缓冲都必须在静态存储里。

### 3. `vm86_ops_build()` 的返回值别丢

这是最容易被忽略的一处:`vm86_ops_build()` 失败意味着某个操作码被两组同时认领,而**单步跑下去可能一开始看不出问题**——直到踩到那个操作码。必须一进来就检查,失败就直接报出来。

### 4. `-mcmodel=small` 与平坦镜像的地址假设

用户程序链接在 `0x400000`(`user/link.ld`),`_start` 被强制在偏移 0。解释器里如果有任何对地址大小的假设(比如把指针截成 32 位),到这边才会暴露。**如果编不过或者跑飞,先怀疑这一类**,而不是先怀疑模拟器。

### 5. 指令条数上限必须有

一个写错的 guest 程序(或者解释器自己的 bug)会让 `vm86_step()` 一直转。**必须有上限,而且上限触发的输出要和"fault"明确区分开** —— 否则死循环会伪装成一次异常,而你会去查那个不存在的异常。

### 6. QEMU 里的验证要是自动化的

`make test` 那套是用串口输出做断言的。**不要只靠肉眼看屏幕** —— 目视验证过的东西在回归里就等于没验证过。把你的输出做成可断言的形式(比如一行 `VM: PASS ...`),接进现有的测试路径。

---

## 四、完成标准

1. `make` 全绿(kernel + user 两份都在编),`-Werror` 下零警告
2. `make test` 与 `make test-all` 全绿 —— **M2 那三条 `selftest=` 路径不能被弄坏**
3. **在 QEMU 里,用 `vm=1` 启动,guest 二进制真的被执行,终态被打印并且被断言。** 这是本任务的全部意义,要有自动化证据,不要"我看到了"。
4. 至少两个 guest 输入:一个正常跑完的,一个**故意触发非法指令**(比如 `FF /7`)的,后者要报出正确的异常向量而不是被当成别的指令跑过去。
5. `dos/cpu/ops_*.c` **零改动**(用 `git diff --stat` 证明)。
6. 报告里写:
   - guest 内存的尺寸、你选它的理由、以及谁在什么时候把它改大
   - 解释器在用户态编译时**实际遇到了什么**(哪怕答案是"什么都没遇到",也要说你是怎么确认的)
   - "Shell 启动 VM"这条路为什么这版不做,以及它卡在哪个具体的机制上

---

## 五、参考

- `kernel/main.c` 结尾那一段 —— 启动参数与 `process_run()`
- `kernel/include/funnyos/process.h` —— `arg` 的语义、`PROCESS_CODE_BASE`、进程模型的边界
- `kernel/proc/process.c` —— `build_address_space()`、`g_current` 与 `process_run()` 不可重入
- `dos/include/vm86/mem.h` —— 开头那段关于"谁提供内存"的注释;`vm86_mem_attach()` 的契约
- `dos/tests/harness.c` —— 怎么建表、怎么装冲突报告器、断言怎么写
- `user/libu/libu.h` 与 `user/funnycom/main.c` —— 用户态的输出与系统调用怎么用
- `docs/tasks/M3-I-corpus.md` 第二节 —— guest 镜像的加载与进入约定(照它做)
- `docs/DESIGN.md` 的 D4、D5
