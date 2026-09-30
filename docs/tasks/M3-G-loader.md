# 任务 G:让用户程序拥有一块比镜像更大的内存

**文件:** `kernel/proc/process.c`、`kernel/include/funnyos/process.h`、`user/link.ld`、`Makefile`、`tools/bin2c.py`、`kernel/main.c`
**测试:** 现有全部内核测试必须保持通过

> 开始之前先读 `docs/tasks/README.md`。规则、构建方式、完成标准都在那里。
>
> **并且先同步你的 worktree**:你的分支切出之后 main 上已经合入了 M3 的 A–E,`dos/` 整个目录都是新的。用 `git -C /c/FunnyOS log --oneline -5` 看主检出的当前状态,把 main 合进你的分支再开始。

---

## 一、为什么要做这件事

M3 的下一步是**把 8086 解释器作为 Ring 3 进程接进 FunnyOS**(DESIGN 的 D4)。那个进程需要一块 guest 物理内存,按 D5 是 1 MB 常规内存 + 扩展池,合计 16 MB。

现在它拿不到。原因是三个事实叠在一起:

1. `process_create()` 从 `PROCESS_CODE_BASE` 开始**精确映射 `size` 个字节**,`size` 就是平坦镜像的长度。
2. `user/link.ld` 的注释写明,`.bss` 被 `objcopy --set-section-flags .bss=alloc,load,contents` 强制变成镜像里真实的零字节,所以**未初始化数据也占镜像长度**。
3. 那个镜像经过 `tools/bin2c.py` 变成一份 C 源码编进内核。bin2c 每字节输出 6 个字符(`0x12, `),所以 **16 MB 的静态数组会生成约 96 MB 的 C 源码**。

第三条是致命的。所以"给 VM 进程声明一个 `static uint8_t guest_ram[16*1024*1024]`"不是一个可选项,哪怕它看起来最省事。

---

## 二、你要做什么

给加载器加上"**内存大小**"这个概念,与"镜像大小"分开。这是 ELF 早就有的东西(`p_filesz` 与 `p_memsz`),也是每个真实加载器都有的一对值。

### 2.1 新签名

```c
struct process *process_create(const char *name, const void *image,
                               size_t image_size, size_t memory_size);
```

契约:

- `memory_size >= image_size`,否则返回 `NULL`(参数错误,不是内存不足)。
- 从 `PROCESS_CODE_BASE` 起映射 **`memory_size`** 个字节。
- 前 `image_size` 个字节从 `image` 填进去,**其余全部清零**。
- 映射的页全部是 `VMM_USER_RW`,和现在一样。

`struct process` 里现在的 `image_size` 字段记录的是"这个进程占了多少页",**改成记录 `memory_size`** —— 因为 `process_destroy()` 靠它调 `free_leaf_frames()`。名字也要跟着改(比如 `memory_size`),否则下一个人会以为它还是镜像长度。**这是这次改动里最容易出错的一处**:改漏了不会崩,只会漏释放页帧,而且只在 VM 进程上漏,测试看不出来。

### 2.2 `memory_size` 从哪来

从 ELF 里读出来,因为链接器知道 `.bss` 到哪里结束,而平坦镜像不知道。

建议做法(你可以换更好的):

1. `user/link.ld` 在 `.bss` 之后放一个符号,例如 `_image_end = .;`。
2. Makefile 在生成 blob 之前,用 `nm`(或 `objdump`)从 `$(USER_ELF)` 取出 `_image_end` 的值,减去 `0x400000` 得到 `memory_size`。
3. 把它作为第三个参数传给 `bin2c.py`,让它**多发一个符号** `<symbol>_mem_size`。
4. `kernel/main.c` 把两个值都传给 `process_create()`。

`bin2c.py` 现在接受三个参数(`<input> <symbol> <output>`),给它加参数时**保持旧的三参数调用仍然能用**,或者把 Makefile 里的调用一起改掉 —— 但不要留下一个"文档说三参数、实际要四参数"的状态。

**为什么用链接器符号而不是手写常量:** 手写的常量会和 `.bss` 的实际大小悄悄脱钩。有人给 VM 进程加一个全局数组,`memory_size` 不变,于是新数组落在映射之外,第一次写就 fault —— 而错误信息会指向那个新数组,不指向这里。

### 2.3 关于 `.bss` 那个 objcopy 技巧

**保留它。** 它现在的作用变成了"让平坦镜像等于需要初始化的部分",而 `memory_size - image_size` 的那一截由加载器清零。两者是互补的,不是重复的。

但 `user/link.ld` 里那段注释现在**说的是旧的理由**("A NOBITS .bss would ... fall outside the size the kernel is told, and fault on first touch")。它仍然是真的,但不再完整 —— 现在还有第二个机制。把它更新成同时说明这两件事,否则下一个人读到它会以为 `.bss` 的处理只有一种。

---

## 三、坑

### 1. 三种"大小"不要混

改完之后同时存在四个数,写代码时把它们标清楚:

| 名字 | 含义 |
|---|---|
| 平坦镜像文件长度 | `objcopy -O binary` 的输出字节数,等于 `image_size` |
| `image_size` | 从 blob 拷进去的字节数 |
| `memory_size` | 映射的页覆盖的字节数,`>= image_size` |
| `_image_end - 0x400000` | 链接器视角的内存末尾,应该等于 `memory_size` |

**最后一行和 `memory_size` 必须相等。** 值得在 Makefile 里加一句断言(或者打印出来让人一眼看到),因为不等的时候症状是"某个全局变量莫名其妙是错的",离原因很远。

### 2. 清零那一截不是可选的

`build_address_space()` 现在对每一页都 `memset(dst, 0, PAGE_SIZE)` 然后再拷。加了尾段之后,**尾段的页也要清零**。不清零的话,VM 进程的 guest RAM 里会有上一个进程的残留 —— 那正是 D4 承诺的隔离被破坏的样子,而且它是**安静的**:一个 DOS 程序读到未初始化内存会得到看似合理的垃圾。

这不是理论风险:现在只有一个进程所以看不出来,`process_run` 的注释也说了"with one process at a time the two calls are a round trip through the same memory and change nothing"。但这里**必须一次做对**,因为第二个进程已经在路上了。

### 3. 失败路径同样要完整

`build_address_space()` 失败时返回 `false` 并且不泄漏。加了尾段之后,失败可能发生在映射到一半的时候,已经映射的页帧要释放。现在这段代码是"边映射边检查",新增的循环要沿用同样的形状。`process_create()` 的四层失败清理(`vmm_destroy_address_space` / `kfree(fpu_state)` / `kfree(kernel_stack)` / `kfree(p)`)也要跟着检查一遍。

### 4. `main.c` 里那两个 `selftest=` 分支别弄坏

`kernel/main.c` 现在用 `strstr(cmdline, "selftest=...")` 选 `arg`,有 `userfault` / `userexit` / `fputest` 三种。它们是 M2 验收的证据。改 `process_create()` 调用点时把这三种都跑一遍。

---

## 四、完成标准

1. `make` 全绿,`-Werror` 下零警告
2. `make test` 与 `make test-all` 全绿 —— 特别是 `selftest=userfault` / `userexit` / `fputest` 三条路径
3. **至少有一个测试证明尾段真的存在、真的被清零、真的可写。** 不能只证明"能起来"。具体来说:一个程序声明一块明显大于它自身代码的 BSS,在尾段的最末尾写一个值再读回来,并且确认它读到的是 0 而不是别的。三条断言都要有,因为"映射了"、"清零了"、"可写"是三个不同的失败方式。
4. **至少有一个测试证明 `memory_size < image_size` 被拒绝**,并且不泄漏。
5. 报告里写:你选了哪种方式把 `memory_size` 从 ELF 传到内核,以及为什么;上面四个数的关系你是怎么保证的。

---

## 五、参考

- `kernel/proc/process.c` 的 `build_address_space()` 与 `process_destroy()`
- `user/link.ld` 与 Makefile 里 user program 那一段(注意 `$(USER_BIN)` 上面那段关于 objcopy 的注释)
- ELF 的 `p_filesz` / `p_memsz` 语义 —— 这次改动就是把这一个概念补上
- [mark-inferred-vs-checked-claims]:报告里凡出现"某某已经是对的"这类断言,标明是查证过还是推测的
