# M4 任务 A 报告:中断核心与投递

**执行者:** A
**范围:** 运行循环、三条 M3 欠账、`step.c` 的 `insn_ip`、`FE /7` 接线(后被协调方接走)、`dos/tests/test_intr.c` 全套
**状态:** `cd dos && make test` 全绿、零警告(16 个套件);11 个变异体全部被预期用例抓到

---

## 一、交付了什么

| 文件 | 内容 | 性质 |
|---|---|---|
| `dos/intr/run.c` | `vm86_run()` —— 运行循环 | 新建 |
| `dos/tests/test_intr.c` | 19 个用例、82 条断言 | 新建 |
| `dos/cpu/step.c` | `cpu->insn_ip = cpu->ip;` | 白名单改动 |
| `dos/cpu/ops_str.c` | `vm86_str_repeat()` 迭代间检查 + IP 回卷 | 白名单改动 |
| `dos/cpu/ops_ctl.c` | STI 影子;`do_interrupt()` 收敛到 `vm86_interrupt()` | 白名单改动 |
| `dos/cpu/ops_mov.c` | `MOV SS`(`8E /2`)影子 | 白名单改动 |
| `dos/cpu/ops_alu.c` | `FE /7` → `vm86_host_trap()`(协调方所为);`POP SS`(`17`)影子(我加的) | 白名单改动 |

`git diff --stat main...HEAD` 只列出上面七个文件。**没有碰**任何头文件、`Makefile`、`cpu.h`、别人的模块。

`dos/intr/{trap,ivt,deliver,firmware}.c` 由协调方实现,本报告不重复描述它们。四个文件里唯一有事的一处见第六节。

---

## 二、运行循环:顺序,以及**一处有理由的偏离**

### 2.1 顺序与理由

```c
for (;;) {
    1. 影子为零 → 投递(若有可投递的);投递不消耗预算,continue
       影子非零 → 跳过投递
    2. halted → VM86_STOP_HALT
    3. 预算用完 → VM86_STOP_STEPS
    4. 影子非零 → 递减一次
    5. vm86_step();预算 +1;按结果分派
}
```

任务书第三节的四条要求逐条落实:

- **投递不消耗预算。** `continue` 在预算递增之前,中断不是指令。
- **`HLT` 之后继续循环。** `case VM86_HALT:` 是 `break`,不是 `return`。下一个边界上第 1 步才决定"被唤醒"还是"真的停下"——`sti; hlt` 的唤醒路径全靠这个。用例 `sti; hlt is woken rather than stopped` 用 HLT 之后那条 `inc ax` 当证据:被唤醒才跑得到它。
- **取编号最小的挂起向量。** 用 `vm86_next_pending()`(协调方在 `host.h` 中公开的),没有在 `run.c` 里另写一份扫描。
- **`steps == 0` 合法。** 第 1 步在预算检查之前,所以 `vm86_run(cpu, 0)` 仍然投递。

### 2.2 偏离:**影子在第 4 步递减,不在第 1 步**

任务书给的顺序是「影子 → 投递 → halted → 预算 → step」,其中影子那一步写作「非零 → 递减、跳过投递」。**我把它挪到了预算检查之后**——因为字面顺序有一个真实的洞:

> 影子的含义是"**下一条指令**不被识别",它属于**即将执行的那条指令**。若在第 1 步递减、而第 3 步随后因预算返回,那么影子被**消耗掉了却没有指令执行**。宿主下一次调用 `vm86_run` 时,影子已经是 0,投递会落在它本该保护的那条指令**之前**。

对 `sti; hlt` 的具体后果:本应在 HLT **之后**投递的中断,现在在 HLT **之前**投递 → 处理程序 IRET 回到那条 HLT → HLT 再等一个"已经处理过"的中断 → **空闲循环停住而不是睡下**。这正是影子存在的理由,只是换了个入口进来。

修法:递减与"即将执行的指令"绑在一起(第 4 步),预算切断在第 3 步,影子就不会被吃掉。用例 `a slice does not spend the grace it cannot use` 用 `vm86_run(cpu, 1)` 把切片正好切在 STI 之后,断言 `intr_shadow == 1` 且第二个切片仍能唤醒;把递减放回第 1 步(M6)该用例立刻报红。

**这条待协调方确认**(我已去信)。它不改变任何对外可见的接口,只改 `run.c` 内部的执行顺序。

---

## 三、三条 M3 欠账

### 3.1 `REP` 可中断 —— **检查放在迭代之间,不在第一次迭代之前**

任务书第四节写的是「每一轮的**开始**检查一次」,但同一节还写着「回卷之后……重放的是**剩下的**次数」。**这两条不能同时成立**:若在第 1 次迭代**之前**检查,第一个元素根本没搬,CX 原封不动,重放的就不是"剩下的"而是"全部",后面那条要求的用例(搬到一半被打断、剩下的字节被搬完)**证明不了任何东西**。

我把它放在 **body 跑完、CX 递减之后**。三条理由:

1. **真机就是每迭代之间识别**,第一个元素一定搬完;
2. **运行循环只在投递不了的时候才走进这条指令**,所以放在第一次迭代之前的那个检查,问的是运行循环在该边界上**刚刚回答过的同一个问题**——它几乎永远是"否";
3. 只有放在后面,中断发生时 SI/CX/DI 才和指令开头不同,用例才能钉住"**不回退**"这条性质。

**协调方已采纳**(任务书第四节按"迭代之间"重写)。

回卷到 `cpu->insn_ip`,即**第一个前缀之前**。`step.c` 在消费前缀的循环之前记下它。

### 3.2 与 3.3 `STI` 与写 `SS` —— 同一个 `intr_shadow`

- `ops_ctl.c` 的 `case 0xFB`:`vm86_flag_set(cpu, VM86_IF, true); cpu->intr_shadow = 1;` —— **IF 立即置位**,被推迟的是**识别**。原来那段 "M4 OWES THE DELAY" 注释已改写成现在的事实,并写明"推迟的是识别不是标志"以及写错它的代价。
- `ops_mov.c` 的 `op_mov_sreg()`:`8E /2` 之后 `if (mr.reg == VM86_SS) cpu->intr_shadow = 1;`
- `ops_alu.c` 的 `op_alu_block()`:`17`(POP SS)之后同样一行。

### 3.4 顺带发现:任务书把 `POP SS` 的**位置**写错了

任务书第五节写「`POP SS` 走的是 `8F` 的路径」。**不是。** 核对操作码表:

- `8F /0` 是 `POP r/m16`,mod=11 时 r/m 寻址 `AX/CX/DX/BX/SP/BP/SI/DI`——**段寄存器不在其中**,所以 `8F` 永远写不到 SS;
- `POP SS` 是 **`17`**,落在 `ops_alu.c` 的 `op_alu_block()`(段寄存器的 push/pop 形式在算术块里,不在栈指令里)。

所以入口确实是**两个**(`8E /2` 和 `17`),只是第二个不在任务书写的地方。**不修的话,"写 SS 抑制一条"只做了一半**,而症状是"大多数程序没事,少数在设置栈的时候被中断打进来看见半个栈"。协调方已确认并改了任务书;这是白名单之外我多动的一处,已在第五节列明。

### 3.5 一处**查到过、但决定不做**的:8086 errata 把抑制范围放得更宽

`ops_mov.c` 原来的注释里有这么一句:「By the errata that inhibition is wider than intended -- it applies to every MOV and POP to a segment register, not to SS alone.」

**这句是对的,我联网查证了**(仓库里没有 8086 手册,这是这一节唯一一条外部事实,标注为**已查**):Intel 的修复比问题更宽,原始 8086 上**任何** `MOV segreg` / `POP segreg` 之后都有这条抑制,后来的实现才收窄回 SS。

**本机按 SS 实现,不按 errata。** 理由是冻结接口: `cpu.h` 写的是 "Set to 1 by STI and by a load of SS",`host.h` 的 `vm86_interruptible()` 写的是 "the shadow of STI or of a load of SS"。**接口是唯一来源**;而且真正被程序依赖的只有 SS 那一种(它让 `mov ss, ax` / `mov sp, ...` 成为原子)。这条差异已写进 `ops_mov.c` 的注释,写明"这是有意的简化",而不是留一个读起来完整却没人查的理由。

---

## 四、每一条期望值是从哪来的

任务书 M4-README 第一节的要求。分成三类:**冻在接口里**(能追到 `host.h`/`firmware.h`/`cpu.h` 的原文)、**PC 约定**(IBM 定下的软件接口,仓库里没有文档)、**我们选的**。

| 期望 | 来源 | 类别 |
|---|---|---|
| 陷阱是 `FE 38 <v> CF`、`F000:(v*4)`、1 KiB | `host.h` 顶部注释 + M4-1 | 冻在接口里 |
| 陷阱读 ModRM 之后的那个字节作为**服务号** | `host.h`: "which service is being asked for" | 冻在接口里 |
| 服务号 == 向量号(注册表按向量索引) | `host.h` 的注册表 + M4-A §一 | 冻在接口里 |
| 陷阱**不弹帧**、返回后 IP 停在 `CF` | `host.h`: "leaves the instruction pointer on the stub's IRET" | 冻在接口里 |
| 未注册的向量 = 到桩上什么都不做、**不是异常** | `host.h` 注册表段 + M4-README §八 | 冻在接口里 |
| 同一向量投两次 = 一次 | `cpu.h` + `host.h` 的 `vm86_raise()` | 冻在接口里 |
| **取编号最小的**挂起向量 | `host.h` 的 `vm86_next_pending()` 注释 | 冻在接口里 |
| 影子由 STI / 写 SS 置 1、由**运行循环**递减 | `cpu.h` 的 `intr_shadow` | 冻在接口里 |
| 异常的两种去向(桩 → 停;被改写 → 投递) | M4-5 + `host.h` 的 `vm86_vector_is_stub()` | 冻在接口里 |
| `HLT` 是"等",`cli; hlt` 是终结 | M4-6 | 冻在接口里 |
| `steps == 0` 合法且仍然投递 | `host.h` 的 `vm86_run()` 注释 | 冻在接口里 |
| `0040:0010` 设备字、`0040:0013` 内存大小 | `firmware.h` 的 `VM86_BDA_*` | 冻在接口里 |
| `INT 11h`/`12h` 的答案是**读 BIOS 数据区**,不是自带常量 | `firmware.h` 开头"addresses live here once" + M4-9 | 冻在接口里 |
| 向量号即优先级 | 8259 的线序 = 向量号;PC 约定 | **PC 约定**(未查,见下) |
| 640 KB 常规内存 | IBM PC/XT 的常规内存边界;本机 1 MiB 中 640K 以下算 conventional | **PC 约定** |
| `REP` 的检查放在迭代之间 | 见 3.1;真机行为 + 运行循环结构 | **推导**(已与协调方确认) |
| 影子在第 4 步递减 | 见 2.2;从"影子属于下一条指令"推出 | **我们选的** |

**关于"未查"的那两条。** 向量号=优先级的说法来自 8259 的线序,与 IBM 的向量分配一致,但仓库里既没有 8259 也没有 IBM PC 技术参考,我**没有**联网核对它;而 640 KB 我没有核对任何文档,它只是"1981 年那台机器的常规内存是 640K"这一通行说法。两条都写在这里,而不是写进代码注释当成事实——按 `mark-inferred-vs-checked-claims`,带理由的错误比没理由的错更难发现。

**唯一一条我联网查过的外部事实**是 3.5 那段 errata(已标注)。

---

## 五、白名单里那几处改动具体怎么落的

| 文件 | 改动 | 怎么落的 |
|---|---|---|
| `step.c` | `cpu->insn_ip = cpu->ip;` | 加在**消费前缀的循环之前**。写在循环之后是不行的:到那时地址已经被走过去了 |
| `ops_str.c` | 循环里加中断检查 | 在 `body()` 与 `cx--` **之后**、`conditional` 的提前退出检查之后。回卷 `cpu->ip = cpu->insn_ip;` 后 `return VM86_CONTINUE`。`+ #include <vm86/host.h>` |
| `ops_ctl.c` | STI 影子 | `case 0xFB` 里在 `vm86_flag_set(IF, true)` **之后**加 `cpu->intr_shadow = 1;`;该 case 的注释整段重写;"M4 OWES THE DELAY" 与文件头那句"does NOT model the delay"两处都已改成现在的事实 |
| `ops_ctl.c` | `do_interrupt()` 收敛 | 删掉静态函数,三处调用点(`CC`、`CD`、`CE`)改成 `vm86_interrupt(cpu, ...)`;原处的长注释替换为一段指向 `dos/intr/` 的说明。`+ #include <vm86/host.h>`。**没有留下第二份实现** |
| `ops_mov.c` | `MOV SS` 影子 | `op_mov_sreg()` 里 `vm86_set_seg()` 之后 `if (mr.reg == VM86_SS) cpu->intr_shadow = 1;`。文件头那段"the interrupt shadow itself is left undone"已整段重写 |
| `ops_alu.c` | `FE /7` 路由 | 协调方所做,以 main 为准。**注意顺序**:`mr.reg == 7` 的判断在 `mr.reg > 1` 之前 |
| `ops_alu.c` | `POP SS` 影子 | `op_alu_block()` 的 `form == 7` 分支里,`vm86_set_seg(SS, ...)` 之后 `if (segment == VM86_SS) cpu->intr_shadow = 1;`。见 3.4 |

三处新增了 `#include <vm86/host.h>`(`ops_alu.c`/`ops_ctl.c`/`ops_str.c`)。`host.h` 自己 `#include <vm86/ops.h>`,所以 `vm86_interrupt`、`vm86_interruptible`、`vm86_host_trap`、`vm86_pop16` 都在作用域里。**没有改任何头文件。**

---

## 六、`REP` 用例证明了什么

### 6.1 `rep is interrupted between iterations`

程序:`sti; rep movsb; hlt`,CX=8,源 `0x800`、目标 `0x900`。向量 8 上装一个 **guest 处理程序**(不是宿主服务),它把"被中断那一刻的 SI 和 CX"写进 `0x700`,然后 IRET。

断言四件事:

1. **8 个字节全都搬到了** —— 证明 REP 被重放而且**搬完了剩下的**,不是搬一个字节就结束;
2. 处理程序**只跑了一次** —— 证明循环确实在中途让了出来,而不是一路跑到底;
3. 处理程序看到的 **SI == 源+1** —— 证明**第一个元素已经搬完**,也证明 SI 没有被回退;
4. 处理程序看到的 **CX == 7** —— 证明 CX 没有被回退,重放的是**剩下的 7 个**。

第 3、4 条合起来才是"回卷之后 CX/SI/DI 不回退"这条要求的证明。**只有把检查放在第一次迭代之前,这两条都测不出来**(SI 会是源、CX 会是 8),而那正是任务书原来那个位置。

### 6.2 回卷那一条**怎么证明能失败**

把 `cpu->ip = cpu->insn_ip;` 改成 `cpu->ip = (uint16_t)(cpu->insn_ip + 2u);`(即回卷到 `F3` 之后、`A4` 之前)——**变异体 M1**:

- 处理程序仍然只跑一次,SI/CX 仍然对;
- 但 IRET 回来之后执行的是 **`A4`(`movsb`)而不是 `F3 A4`(`rep movsb`)**:搬 **1 个字节**`CX` 再也不参与;
- 于是目标缓冲区里**只有 2 个字节是正确的**(中断前的一个 + 重放的一个),剩下 6 个还是 0;
- `every byte was copied` 报红。

这正是任务书说的"**静默地**把 `rep movsb` 变成 `movsb`,机器看起来还在正常工作"——它不会崩、不会报错,只是搬少了。**这条用例存在的全部意义就是让它不静默。**

### 6.3 `INS`/`OUTS` 那条路径

`rep insb`(`F3 6C`)走的是**同一个** `vm86_str_repeat()`,只是 body 不同。所以"改一处两条都改到了"是要的,但也意味着**只用 `MOVS` 测不出"共享没断"**:如果哪天 `ops_186.c` 改成自己写一份循环,`rep movsb` 的用例照样全绿。用例 `rep ins is interrupted between iterations` 就是钉这一点的——它断言 DI 前进了一个元素、CX 剩 3、4 个字节全部落盘。

**变异体 M2**(把检查复制到 `body()` 之前)被 `rep is interrupted between iterations` 抓到:`SI` 会是源地址而不是源+1。**这条也说明了检查位置不是风格问题。**

---

## 七、测试与变异

### 7.1 19 个用例

| 组 | 用例 |
|---|---|
| 陷阱 | 往返(IP 停在 `CF`、SP 完全复原)、桩按向量选服务、未注册向量无害 |
| 挂起 | 同向量两次算一次、两个向量取最小的先投 |
| 影子 | STI / MOV SS / POP SS 各一条,用"被保护的那条指令跑没跑"当判据 |
| HLT | `sti; hlt` 被唤醒、`cli; hlt` 是终结 |
| 预算 | `steps == 0` 仍然投递、切片不消耗用不掉的影子 |
| 异常 | 未挂钩 → `VM86_STOP_FAULT`;已挂钩 → 投递给 guest |
| 串操作 | `rep movsb` 与 `rep insb` 各一条 |
| 固件 | `INT 11h`/`12h` 与数据区一致、`vm86_vector_is_stub()` 的三种答案 |
| 阻塞读 | 服务重试、键在两次尝试之间送到 |

### 7.2 变异测试:11 个变异体,全部被抓

每一个都是**故意改错**、跑套件、确认**预期的那条**用例报红,然后还原。两个假阴性来源都避开了:`-Werror` 下编译失败的变异体(脚本把"没编出来"单独判为失败),以及 `make` 同秒不重建(每次都改源文件,时间戳必然变化)。

| 变异 | 抓到它的用例 |
|---|---|
| M1 REP 回卷到 `F3` 之后 | `rep is interrupted between iterations` |
| M2 REP 在 body **之前**检查 | `rep is interrupted between iterations` |
| M3 STI 不置影子 | `sti delays recognition by one instruction` |
| M4 `MOV SS` 不置影子 | `mov ss delays recognition by one instruction` |
| M5 `POP SS` 不置影子 | `pop ss delays recognition by one instruction` |
| M6 影子每边界递减两次 | `sti delays recognition by one instruction` |
| M7 取**编号最大**的挂起向量 | `the lowest pending vector is delivered first` |
| M8 `HLT` 直接返回运行循环 | `sti; hlt is woken rather than stopped` |
| M9 预算检查挪到投递之前 | `a budget of zero still delivers` |
| M10 所有异常无条件投递 | `an unhooked fault stops the machine` |
| M11 `FE /7` 仍然报向量 6 | `a trap call comes back` |

M3/M4/M5 三处分得很干净:去掉哪一个,只有它自己那条用例报红。这是"两个入口"拆在 `ops_mov.c` 和 `ops_alu.c` 两个文件里之后仍然测全的证据。

### 7.3 `vm86_service_retry`:一次我错了、对方对了的记录

我最初认定 `vm86_service_retry()` 应当**弹掉 INT 压的帧并回卷到 `INT` 本身**(帧内返回地址 − 2),并写了 `dos/intr/retry.c`。协调方发现 `trap.c` 里已有一份(重复符号),并给出了一个**我没想到的反例**:

> 那个 **`− 2` 只在被中断的是一条两字节的 `INT imm8` 时成立**。TSR 链式调用走的是 `pushf` + `call far`(5 字节),帧里的返回地址是 `call` **之后**的地址,减 2 会落在 `call` 指令**中间**。

链式远调用正是 D6 点名要测的那条路。所以正确修法是**不弹帧**、把帧留给桩那条 `IRET` 在最终成功时弹一次——**无论桩是怎么被到达的**;缺的那一半是 **IF**:`INT` 清了 IF,而运行循环的投递要求 IF=1,于是重试期间中断永远送不进来,**阻塞读死锁**。修法是在 `trap.c` 里 `vm86_flag_set(cpu, VM86_IF, true)`。

**我验证过这条修法**:在它落地之前,我用 `vm86_run` 跑了我自己写的阻塞读用例——同一份用例,在 `trap.c` 是老版本时 `the keyboard interrupt was delivered between attempts` 报红(键永远送不进来),在协调方改完之后全绿。`retry.c` 已删除,`host.h` 的注释里也补上了 IF 那一段。

**留给我自己的教训:** 我当时的推理"只有 `pop + 回卷` 自洽"是从"`INT n` 只有一种编码"出发的,**没有考虑栈上的帧不是 `INT` 压的那种情况**——而 `host.h` 原文里恰好写着那种情况,我读过却把它当成了一句免责声明。

---

## 八、测试覆盖到了什么

- 19 个用例,82 条断言,`make test-intr` 全绿
- `make test` 16 个套件全绿、零警告(`-Werror`)
- 除 `test_intr.c` 之外**一条都没红**,包括 `test_verify.c` 的 `sti takes effect immediately`(它断言 IF、不是投递,按预期继续绿)与 `inc and dec a byte register`(`FE /7` 不破坏 `/0`-`/1`)

---

## 九、**没覆盖到的部分**

按重要性排:

1. **`vm86_run` 的重入与嵌套中断。** 用例里没有任何一条在服务或处理程序的**内部**再调用 `vm86_run`。M4-E 的验收程序会在宿主循环里反复调用,但嵌套(例如处理程序想单步宿主)完全没有测,也没有设计。
2. **`TF`(单步)完全没有碰。** `vm86_interrupt` 会清 TF,但没有任何用例、也没有任何代码在 IRET/每指令之后重新置 TF。M? 的调试器要用它之前,这块是空的。
3. **链式 TSR 那条路没有端到端用例。** `vm86_service_retry` 的语义正是为了它才那样设计的,我的用例只走了 `INT 16h` 这一条路(`pushf` + `call far` 的链式帧一次都没构造过)。这是**已知的最大缺口**——也正是 7.3 那条反例所在的地方。
4. **STI 之外的重叠影子。** 没有用例构造"STI 的影子还没吃完又写了一次 SS"(影子被再次置 1)。按接口它应当重新计 1,但没测。
5. **8086 errata 的宽抑制**(3.5):按 SS 实现,**多段寄存器卸载后的那一条没有被抑制**。有意不做,但也没测"确实没做"。
6. **给某个向量既装了 guest 处理程序、又注册了宿主服务**时谁赢——没测。按实现是 guest 赢(陷阱到不了)。这可能是对的,但没有用例钉住它。
7. **`vm86_run` 的预算与挂起中断同时到达的边界**只测了 `steps == 0` 一档;`steps` 很小(1、2)且中断密集的组合没测。
8. **`REP` 带非段前缀**(如 `F3 F3 A4`、`F2 F3 A4`)的回卷没测——回卷到 `insn_ip` 对它们应当同样成立,但没有用例。
9. **性能。** 没有测过每秒退休多少条指令;`pending_lowest` 换成 `vm86_next_pending()` 之后没有做任何基准。
10. **`0F` / `60`-`6F` 与陷阱的交互**没测:它们是 186 的编码,与 `FE /7` 无关,但"未认领 → 向量 6"这条在 M4 下是否仍然成立(会不会被投递给 guest)只有 `test_verify.c` 的老用例覆盖,不在我这里。

---

## 十、仍然悬着的一处

**2.2 的影子递减位置**是我对任务书顺序的唯一一处偏离,理由与证据都在那一节。协调方若坚持字面顺序,把 `run.c` 里第 4 步那三行挪回第 1 步即可——但 `a slice does not spend the grace it cannot use` 会立刻报红,而那是**真机行为**。

