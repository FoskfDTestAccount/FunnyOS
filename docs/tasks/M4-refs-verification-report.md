# `docs/dos-refs.md` 第一节:把"存疑"的那几条真的查掉

**交付:** 对 `docs/dos-refs.md` 的修改(第一节那一小节 + 第八节两条处置 + 第七节两个 BDA 字节 + 第零节与第九节的等级说明)、本报告
**日期:** 2026-10-01
**结论:** 五条里 **三条查实、一条作废、一条反转**;另外撞出**三条新的**。**其中一条是我自己上一轮报错的**,报告里单独写。

---

## 一、方法:为什么"复读源码"和"引源码"不是一回事

上一轮那一小节标着"唯一不依赖手册转述的依据",而它是**转述的转述**:B 读了源码 → 写成报告 → 协调方抄进参考文件。这个链条上每一环都保留了"我读过原文"的语气,但**硬度不随转述传递**。

这一轮换了个做法:**把两份源码取到本地**。

```
curl -o vgabios-qemu.c    https://raw.githubusercontent.com/qemu/vgabios/master/vgabios.c     # 3923 行
curl -o vgatables.h       https://raw.githubusercontent.com/qemu/vgabios/master/vgatables.h
curl -o vgabios-seabios.c https://raw.githubusercontent.com/coreboot/seabios/master/vgasrc/vgabios.c   # 1133 行
curl -o vgafb-seabios.c   https://raw.githubusercontent.com/coreboot/seabios/master/vgasrc/vgafb.c
curl -o vgabios-seabios.h https://raw.githubusercontent.com/coreboot/seabios/master/vgasrc/vgabios.h
```

然后在本地 `grep` / `sed` 读。**这是这一轮唯一真正重要的那一步**:它可以被**指认**——"`vgasrc/vgafb.c` 的 `vgafb_clear_chars` 第 588 行"是一个任何人可以复现、可以反驳的说法;而"vgabios 这么干"不是。上一轮那处错误(第三节)之所以发生,正是因为我在**浏览器摘要**里读了一段代码,而那份摘要**没有给我被调函数的实现**。

---

## 二、五条的逐条结果

### 2.1 `0Eh` 的 `BS` —— **查实**(两份独立实现逐字一致)

```c
/* qemu/vgabios, vgabios.c, biosfn_write_teletype() */
   case 8:
    if(xcurs>0)xcurs--;
    break;

/* coreboot/seabios, vgasrc/vgabios.c, write_teletype() */
    case 8:
        if (pcp->x > 0)
            pcp->x--;
        break;
```

列退一格、**到 0 为止**,不爬到上一行。两份独立实现完全一致——这是这一类查证里能得到的最强结论。

### 2.2 `0Eh` 往哪一页写 —— **查实**(而且两份里是同一句注释)

```c
/* qemu/vgabios, vgabios.c, 第 672 行, case 0x0E 分支 */
   case 0x0E:
     // Ralf Brown Interrupt list is WRONG on bh(page)
     // We do output only on the current page !
     biosfn_write_teletype(GET_AL(),0xff,GET_BL(),NO_ATTR);
```
```c
/* coreboot/seabios, vgasrc/vgabios.c, handle_100e() */
static void noinline
handle_100e(struct bregs *regs)
{
    // Ralf Brown Interrupt list is WRONG on bh(page)
    // We do output only on the current page !
    struct carattr ca = {regs->al, regs->bl, 0};
    struct cursorpos cp = get_cursor_pos(GET_BDA(video_page));
```

vgabios 用 `0xff` 当"当前页"的哨兵(函数开头 `if (page == 0xff) page = read_byte(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE);`)——所以**这个入口根本无法指定页**。SeaBIOS 直接读 `video_page`。

**附带的收获:两份都证实 `BL` 在文本模式下不被使用**(SeaBIOS 传 `{..., 0}`、vgabios 传 `NO_ATTR`,即"属性无效")——B 那处与任务书的偏离,现在有**两份源码**支持,不再只是"我记得是这样"。

### 2.3 `0Eh` 的 `TAB` —— **规则作废**

```c
/* qemu/vgabios, vgabios.c, biosfn_write_teletype() */
   case '\t':
    do
     {
      biosfn_write_teletype(' ',page,attr,flag);
      biosfn_get_cursor_pos(page,&dummy,&cursor);
      xcurs=cursor&0x00ff;ycurs=(cursor&0xff00)>>8;
     }while(xcurs%8==0);
    break;
```

**循环条件是反的**:它在列是 8 的倍数时**继续**。所以从列 0 出发只写**一个**空格、停在列 1;从列 7 出发写两个、停在列 9。**这不是制表位,是 vgabios 的一个 bug**(写成 `while(xcurs%8!=0)` 才是"补到下一个制表位")。

```c
/* coreboot/seabios, vgasrc/vgabios.c, write_teletype() —— 没有 case 0x09 */
    switch (ca.car) {
    case 7:  /* FIXME should beep */ break;
    case 8:  ... break;
    case '\r': ... break;
    case '\n': ... break;
    default: write_char(pcp, ca); break;
    }
```

SeaBIOS **根本没有 `TAB` 分支**:`0x09` 落到默认分支,被当成一个字形打出去。

**所以参考里那条"补空格到下一个 8 的倍数"两边都不是。** 处置写进了 `dos-refs.md`:**保留本机的行为,但不再声称它有出处**——它是**我们的选择**(理由:那是终端该有的行为,而 SeaBIOS 那样打一个字形只会让光标动一格);后果也写明:**三种行为在真机上都不一致,所以几乎没有程序会依赖 `0Eh` 的 TAB**。

> 这条正好说明"查证"不等于"找到支持":有时候**查证的结果是发现规则本身没有出处**,而那和"规则是错的"是两件事。

### 2.4 `00h` 的 `AL` bit 7 —— **查实**(两份一致)

```c
/* qemu/vgabios, vgabios.c, biosfn_set_video_mode() */
static void biosfn_set_video_mode(mode) Bit8u mode;
{// mode: Bit 7 is 1 if no clear screen
 Bit8u noclearmem=mode&0x80;
 ...
 if(noclearmem==0x00)
  {
   if(vga_modes[line].class==TEXT)
     memsetw(vga_modes[line].sstart,0,0x0720,0x4000); // 32k
```
```c
/* coreboot/seabios, vgasrc/vgabios.c, handle_1000() */
    if (regs->al & 0x80)
        flags |= MF_NOCLEARMEM;
```

并且两份都把这**一位记进数据区**(vgabios:`write_byte(BIOSMEM_SEG,BIOSMEM_VIDEO_CTL,(0x60|noclearmem));`,SeaBIOS:`SET_BDA(video_ctl, 0x60 | (flags & MF_NOCLEARMEM ? 0x80 : 0x00));`)。

**这一条的用处**:任务书坑 3 要求 B"要么确认,要么写成没实现"。现在 B 可以把出处写进断言注释了。

### 2.5 `09h`/`0Ah` 在屏底 —— **查实,而且结论比上一轮更完整**

```c
/* qemu/vgabios, vgabios.c, biosfn_write_char_attr() —— 文本分支 */
   address=SCREEN_MEM_START(nbcols,nbrows,page)+(xcurs+ycurs*nbcols)*2;
   dummy=((Bit16u)attr<<8)+car;
   memsetw(vga_modes[line].sstart,address,dummy,count);
```

**一次 `memsetw`,写 `count` 个字,不问行尾、不回绕、不停。** 注意同一个函数的**图形**分支反而有边界(`while((count-->0)&&(xcurs<nbcols))`)——文本分支没有。

```c
/* coreboot/seabios, vgasrc/vgabios.c, handle_1009() */
    while (count--)
        write_char(&cp, ca);          /* write_char 只回绕,不上滚 */
```

SeaBIOS 显式回绕,但 `write_char` **不含上滚**,所以越过最底行之后它同样继续往后写。

**结论:两份都不停。** 页面内存是线性的,所以 B 的"回绕"与它们的线性推进**在页内逐字节相同**;差别只在**最后一行之后**——它们写进下一页的内存,**B 停住**。B 的选择更安全,而且它是**决定**,不是事实。

---

## 三、我自己上一轮报错的那一条(这一节是给下一轮的)

上一轮我向协调方提出了"更正":SeaBIOS 在上滚时不改属性。**那是错的。** 协调方接受了它,把它写进了参考文件(标为"两份其实一致"),我这轮读源码时把它翻了回来。

**错在哪:**

我读到的是**调用点**——

```c
/* coreboot/seabios, vgasrc/vgabios.c, write_teletype() */
        struct carattr attr = {' ', 0, 0};
        vgafb_scroll(win, winsize, 1, attr);
```

然后**从第三个字段的名字 `use_attr` 推断出它的语义**("属性无效 → 不改属性"),写在报告里,语气是"复读当前 master 后指出"。

而**被调函数**里写的是——

```c
/* coreboot/seabios, vgasrc/vgafb.c, vgafb_clear_chars() */
    u16 attr = ((ca.use_attr ? ca.attr : 0x07) << 8) | ca.car;
```

**同一个标志,在同一个文件的两个函数里含义不同**:`vgafb_write_char` 里 `use_attr == 0` 表示"只写字符、不碰属性字节";`vgafb_clear_chars` 里表示"**用 0x07**"。

**两条教训,都比结论本身值钱:**

1. **不要从参数名推断被调函数的语义。** 名字是调用者的语言,不是约定的行为。`use_attr` 听起来只有一个意思,而它有两个。
2. **更有意思的是它为什么没被发现。** 我那条"更正"读起来**非常像一次查证**:它有代码片段、有函数名、有"复读 master"这样的措辞——**而它只是把一次推断包装成了查证的样子**。协调方没有理由怀疑它(他们没读源码),于是它进了参考文件并**把一个本来正确的条目划掉了**。这正是这个项目一直在防的那个形状:**带理由的错误比没理由的错更难发现**,而"我读了源码"是一个非常好用的理由。

处置:参考文件里那条改回"**确实是分歧**",并把这段错法写进注二(连同真相)——**因为下一个按参数名推断语义的人会踩同一处,而那一处现在有名字了。**

---

## 四、顺带撞出来的三条(都不在任务清单上)

### 4.1 `0Fh` 返回的 `AL` 带 bit 7

```c
/* qemu/vgabios, vgabios.c, biosfn_get_video_mode() —— 汇编块 */
  mov   bx, # BIOSMEM_VIDEO_CTL
  mov   ah, [bx]
  and   ah, #0x80
  mov   bx, # BIOSMEM_CURRENT_MODE
  mov   al, [bx]
  or    al, ah
```
```c
/* coreboot/seabios, vgasrc/vgabios.c, handle_100f() */
    regs->al = GET_BDA(video_mode) | (GET_BDA(video_ctl) & 0x80);
```

**`0Fh` 的 `AL` 是"模式 | 上次不清屏那一位"**,不是裸模式。参考原来的表写的是"AL = 模式"。

**对 M4 的含义**:本机的 `set_mode()` 把 bit 7 掩掉之后就丢了(`mode = al & 0x7F`),也没有那个控制字节,所以 `0Fh` 返回的永远是裸模式。一个用 `AL & 0x80` 判断"屏幕上还有没有东西"的程序,在本机拿不到那个信息。**这是一条新发现的差异,交给协调方决定要不要补。**

### 4.2 数据区里还有两个字节是视频的

`vgatables.h` 里:**`BIOSMEM_NB_ROWS = 0x84`**(行数减一)、**`BIOSMEM_VIDEO_CTL = 0x87`**(视频控制字节,bit 7 就是上面那一位)。两份源码都用它们(`read_byte(BIOSMEM_NB_ROWS)+1` / `GET_BDA(video_rows) + 1`)。已补进 `dos-refs.md` 第七节,标【源码】。

### 4.3 `06h`/`07h` 的越界矩形:**两份都夹取,而 B 拒绝**

```c
/* qemu/vgabios, vgabios.c, biosfn_scroll() 开头 */
 if(rul>rlr)return;
 if(cul>clr)return;
 ...
 if(rlr>=nbrows)rlr=nbrows-1;
 if(clr>=nbcols)clr=nbcols-1;
```
```c
/* coreboot/seabios, vgasrc/vgabios.c, verify_scroll() */
    if (lry >= nbrows) lry = nbrows-1;
    if (lrx >= nbcols) lrx = nbcols-1;
    int wincols = lrx - ulx + 1, winrows = lry - uly + 1;
    if (wincols <= 0 || winrows <= 0) return;
```

两条独立实现**都是夹取**,只有"左上大于右下"才是原样返回。而 `bios10.c` 是**拒绝**越界(`bottom >= BIOS10_ROWS || right >= st->columns → return`)。

**可达的后果是具体的:40 列模式下 `DL = 79`**(一个按 80 列写的清屏)——硬件夹到 39 并清屏,**本机什么都不做**。B 的注释给了理由(那是一个 bug,悄悄滚一半会掩盖它),但**现在有两条独立实现站在另一边**。已写进参考文件的注四,建议转给 B。

**顺带**:SeaBIOS 的 `verify_scroll` 也把 `06h`/`07h` 的四个寄存器钉死了(`ulx = cl, uly = ch, lrx = dl, lry = dh`)——`CL` 是**列**、`CH` 是**行**。这条以前只是任务书说的,现在是源码说的。

---

## 五、改了 `docs/dos-refs.md` 的哪些地方

| 位置 | 改动 |
|---|---|
| 第零节 | 加一段:第一节的 vgabios 小节是**唯一**【源码】级的部分,其余仍是二手 |
| 第一节 | 那一小节整段重写:标题改成"复读源码查证",补上方法、十行逐条出处表、注一到注四 |
| 第七节 | 补 `0040:0084`(行数减一)与 `0040:0087`(视频控制字节),标【源码】 |
| 第八节 | "上滚属性"那条**改回"确实是分歧"**并写明错法;`AH=04h` 与 `AL=0` 两条从"未定"改成任务 D 已定 |
| 第九节 | 加一条:这次查的是**两份开源 BIOS**,不是 IBM 的固件;"两份一致"≠"真机如此" |

---

## 六、没有查证的(说清楚,免得被当成查过了)

- **真机 IBM BIOS 的行为。** 两份开源实现不能代替原件。两份**不一致**的地方(上滚属性、`TAB`、越界矩形)真机怎么办**仍然不知道**——这几条现在的状态是"两个实现中的一个",不是"事实"。
- **`0Eh` 的 `BEL`。** 两份都是 `//FIXME should beep` —— 所以"忽略"有出处,但那是因为**没有实现**,不是因为"真机不该响"。这个区别值得留着。
- **`INT 10h` 的图形模式。** M7。
- **`09h`/`0Ah` 的其余语义**(比如它们是否影响别的寄存器):这轮只查了屏底那一条。
