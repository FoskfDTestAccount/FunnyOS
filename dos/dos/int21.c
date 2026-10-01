/*
 * INT 21h: the function dispatcher.
 *
 * See int21.h for what is here, what is deliberately not, and where the
 * boundary to the kernel is. This file is the dispatcher and the twelve
 * functions W4 covers; docs/dos-refs-dos.md section 9 is the reference
 * they were written against, and every number in it came from a source
 * rather than from this file.
 *
 * ---------------------------------------------------------------------
 * The three decisions this file made that the reference did not settle
 *
 *   **The carry flag is cleared on every successful return**, including
 *   the functions that do not document a carry flag at all -- 02h, 09h,
 *   25h, 35h, 1Ah, 2Fh, 0Dh. DOS does not document a return for those,
 *   which means it does not promise what CF is afterwards either, and a
 *   program that tests CF after one of them is asking a question. The two
 *   answers available are "whatever the previous call left" and "clear",
 *   and the first one is a stale flag from an unrelated operation: a
 *   program that checks CF after `AH=09h` on a machine where the call
 *   before it happened to fail would take a successful print for a
 *   failed one. Clearing costs nothing and is the same answer every time.
 *   This is a decision, not a fact about DOS, and it is written here and
 *   in section 9 because a reader who finds it and cannot tell which it
 *   is will not know whether to change it.
 *
 *   **An unimplemented function sets AL to zero and the carry flag**, and
 *   leaves AH alone. That is what FreeDOS does now, and it is deliberately
 *   not what FreeDOS did once: returning an error code in AX broke 4DOS,
 *   because a program that sees CF set reads AX and a code of 1 is not the
 *   same thing to it as a byte of zero. The citation is in section 9.
 *
 *   **An unterminated string stops at the end of the guest's memory**
 *   rather than at the end of the address space. See print_string below.
 *
 * ---------------------------------------------------------------------
 * What writes to the screen
 *
 * bios10_tty(), which is the same routine INT 10h AH=0Eh runs. On a real
 * machine DOS's console output called the BIOS and this is that call; a
 * second implementation here would be a second place for CR, LF, BS and
 * TAB to be wrong.
 *
 * The consequence worth naming is that `AH=02h` moves the cursor and
 * scrolls the screen, because the teletype does. That is correct -- it is
 * what DOS did -- and it is the reason the console functions are not
 * simply memory writes.
 */
#include <vm86/int21.h>

#include <vm86/dos.h>
#include <vm86/host.h>
#include <vm86/mem.h>

#include "../bios/bios10.h"
#include "../bios/bios16.h"
#include <vm86/fat.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Saying yes and no                                                   */
/* ------------------------------------------------------------------ */

/*
 * Both halves of an error, together, so they cannot come apart. An
 * implementation that writes a code into AX and forgets the carry flag is
 * worse than one that does nothing at all: the caller tests CF, sees
 * success, and carries on with whatever was already in the buffer.
 */
static void dos_fail(struct vm86_cpu *cpu, uint16_t code)
{
    vm86_flag_set(cpu, VM86_CF, true);
    cpu->ax = code;
}

/*
 * Success, and the carry flag cleared -- see the note at the top about
 * which functions this applies to.
 */
static void dos_ok(struct vm86_cpu *cpu)
{
    vm86_flag_set(cpu, VM86_CF, false);
}

/* ------------------------------------------------------------------ */
/* 02h: output a character                                             */
/* ------------------------------------------------------------------ */

static void output_char(struct vm86_cpu *cpu, struct int21_state *st)
{
    bios10_tty(cpu, st->video, cpu->dl);

    /* AL is left holding the character. DOS's own documentation says this
     * function returns nothing, and every MS-DOS from 2.1 to 7.0 leaves
     * the character in AL anyway -- which is in the reference, with its
     * source, because "documented as nothing" and "actually nothing" are
     * different claims and only one of them is true here. */
    cpu->al = cpu->dl;

    dos_ok(cpu);
}

/* ------------------------------------------------------------------ */
/* 09h: output a '$'-terminated string                                 */
/* ------------------------------------------------------------------ */

/*
 * Walk from DS:DX until the terminator, printing as we go.
 *
 * ---------------------------------------------------------------------
 * Why there is a bound, and why it is the size of memory
 *
 * DOS's loop has no bound: a program whose string is never terminated
 * hangs the machine, and that is what happens on the real thing too. So
 * the bound is not there to be more correct than DOS. It is there because
 * of where this loop runs -- inside a service, inside the trap, inside a
 * single instruction that never retires. A guest that spins in ordinary
 * code retires instructions, and the host driving the machine gets a look
 * in between slices and can give up; a guest that spins *here* never
 * returns to the instruction layer at all, and the machine is not merely
 * hung but unobservable.
 *
 * The bound is the guest's memory, and that is the honest number rather
 * than a round one: a string cannot be longer than the machine has bytes,
 * so if the terminator has not turned up by the time every byte of memory
 * has been examined, there is not one.
 *
 * There is a second stop in front of it -- the first byte with nothing
 * behind it -- and it is not an optimisation. An address past the end of
 * memory reads as 0xFF and will read as 0xFF for as long as anything
 * looks, so a run of them can never contain the terminator and walking
 * through one is work with a known answer. Stopping there is also what
 * makes the runaway cheap: the cost is bounded by the memory the machine
 * actually has rather than by the address space it can name.
 *
 * The byte is read before either test rather than after, and that is what
 * makes the paragraph below true: an unreadable address still reads, as
 * 0xFF, so a walk that is about to stop on one has a byte in hand when it
 * stops.
 *
 * The one case the memory bound is still needed for is a machine with the
 * A20 gate closed, where the address wraps at a megabyte and a walk that
 * never leaves mapped memory can go round for ever. See vm86_mem_offset.
 *
 * ---------------------------------------------------------------------
 * What AL holds when it gives up
 *
 * The last byte it looked at, which is the same rule DOS's own loop ends
 * by -- it stops on the byte that is '$' and leaves that in AL -- applied
 * to a loop that stopped for a different reason. A runaway therefore
 * leaves 0xFF, which is the byte that stopped it and is a value no
 * terminated string can produce. It is not a lie about having found a
 * terminator, and it is not a value a correct program can see, because a
 * correct program's string is terminated.
 */
static void print_string(struct vm86_cpu *cpu, struct int21_state *st)
{
    struct vm86_mem *mem = cpu->mem;
    uint32_t         at  = ((uint32_t)cpu->ds << 4) + cpu->dx;
    uint32_t         left;
    uint8_t          ch = 0;

    for (left = mem->size; left > 0u; left--) {
        ch = vm86_mem_read8(mem, at);

        if (ch == (uint8_t)VM86_INT21_STRING_END)
            break;

        if (vm86_mem_offset(mem, at) == VM86_MEM_UNMAPPED)
            break;

        bios10_tty(cpu, st->video, ch);
        at++;
    }

    cpu->al = ch;

    dos_ok(cpu);
}

/* ------------------------------------------------------------------ */
/* 25h and 35h: the interrupt vector table                             */
/* ------------------------------------------------------------------ */

/*
 * The vector table is guest memory at 0x0000-0x03FF and four bytes per
 * entry, and these two functions are a read and a write of one entry.
 * They are here rather than left to the program writing memory itself
 * because a program that hooks a vector has to be able to save what was
 * there, and reading the table directly is possible but not what DOS
 * programs did -- it is easy to get the segment and offset halves the
 * wrong way round, and getting them the wrong way round is a handler
 * that jumps into the middle of the old one.
 */

static void set_vector(struct vm86_cpu *cpu)
{
    uint32_t at = (uint32_t)cpu->al * 4u;

    vm86_mem_write16(cpu->mem, at,      cpu->dx);
    vm86_mem_write16(cpu->mem, at + 2u, cpu->ds);

    dos_ok(cpu);
}

static void get_vector(struct vm86_cpu *cpu)
{
    uint32_t at = (uint32_t)cpu->al * 4u;

    cpu->bx = vm86_mem_read16(cpu->mem, at);

    /* Through the accessor, because ES has a cached base and writing the
     * register directly would leave the cache describing the old
     * segment. See the note on seg_base in cpu.h. */
    vm86_set_seg(cpu, VM86_ES, vm86_mem_read16(cpu->mem, at + 2u));

    dos_ok(cpu);
}

/* ------------------------------------------------------------------ */
/* 1Ah and 2Fh: the disk transfer address                              */
/* ------------------------------------------------------------------ */

/*
 * Stored as the pair the program gave, not as a linear address. 2Fh has
 * to hand back exactly what 1Ah was given -- a program compares the
 * answer against an address it saved, or reconstructs its buffer from it
 * -- and a round trip through a linear address is a way to be right about
 * the arithmetic and wrong about the answer.
 */

static void set_dta(struct vm86_cpu *cpu, struct int21_state *st)
{
    st->dta_segment = cpu->ds;
    st->dta_offset  = cpu->dx;

    dos_ok(cpu);
}

static void get_dta(struct vm86_cpu *cpu, const struct int21_state *st)
{
    cpu->bx = st->dta_offset;

    vm86_set_seg(cpu, VM86_ES, st->dta_segment);

    dos_ok(cpu);
}

/* ------------------------------------------------------------------ */
/* The drive calls                                                     */
/* ------------------------------------------------------------------ */

/*
 * 19h: which drive are we on.
 *
 * The answer is a constant here because the machine has one drive and it
 * is always F: -- see the note on VM86_INT21_DRIVE_F for why F: and not
 * A:, which is a decision about this machine rather than about DOS.
 */
static void get_drive(struct vm86_cpu *cpu)
{
    cpu->al = (uint8_t)VM86_INT21_DRIVE_F;

    dos_ok(cpu);
}

/*
 * 0Eh: select a drive.
 *
 * The one drive is F:, so selecting it is a no-op that reports success --
 * that is what DOS does when the drive asked for is already current --
 * and selecting anything else fails with "no such drive". The failure
 * path is the interesting one and it is reachable: it is the only way a
 * test can tell this function from one that ignores its argument.
 *
 * AL on success is the count, which is 6 rather than 1; see the note on
 * VM86_INT21_DRIVE_COUNT for why DOS counts that way.
 */
static void select_drive(struct vm86_cpu *cpu)
{
    if (cpu->dl != (uint8_t)VM86_INT21_DRIVE_F) {
        dos_fail(cpu, VM86_INT21_ERR_DRIVE);
        return;
    }

    cpu->al = (uint8_t)VM86_INT21_DRIVE_COUNT;

    dos_ok(cpu);
}

/* ------------------------------------------------------------------ */
/* 30h: the version                                                    */
/* ------------------------------------------------------------------ */

/*
 * AL is the major version and AH the minor. The two halves are in the
 * order a reader of English would not guess -- 0x1E03 is 3.30 and not
 * 30.3 -- and dos.h says the same thing beside the constant.
 *
 * BX and CX are the OEM number and the user serial number, and they are
 * both zero. Zero is a real OEM number (IBM's), which is a claim, and it
 * is the claim every program that ignores these registers does not care
 * about. Returning an invented number would be worse: there is no OEM
 * here to name, and a program that checks for a particular one is looking
 * for a machine this is not.
 */
static void get_version(struct vm86_cpu *cpu)
{
    cpu->ax = VM86_DOS_VERSION;
    cpu->bx = 0;
    cpu->cx = 0;

    dos_ok(cpu);
}

/* ------------------------------------------------------------------ */
/* The dispatcher                                                      */
/* ------------------------------------------------------------------ */

/* W5/W6: byte-array files and the BIOS keyboard ring. Every guest range
 * is checked before consuming input, advancing a handle, or editing disk. */
static uint32_t guest_address(uint16_t seg, uint16_t off, unsigned i)
{
    return ((uint32_t)seg << 4) + (uint16_t)(off+i);
}
static bool guest_range(struct vm86_cpu *cpu, uint16_t seg, uint16_t off, unsigned n)
{
    for (unsigned i=0;i<n;i++)
        if(vm86_mem_offset(cpu->mem,guest_address(seg,off,i))==VM86_MEM_UNMAPPED) return false;
    return true;
}
static bool guest_path(struct vm86_cpu *cpu, char out[128])
{
    for(unsigned i=0;i<128;i++) {
        uint32_t a=guest_address(cpu->ds,cpu->dx,i);
        if(vm86_mem_offset(cpu->mem,a)==VM86_MEM_UNMAPPED) return false;
        out[i]=(char)vm86_mem_read8(cpu->mem,a);
        if(!out[i]) return true;
    }
    return false;
}
static void status(struct vm86_cpu *cpu,int error)
{
    if(error) dos_fail(cpu,(uint16_t)error); else dos_ok(cpu);
}
static bool console_take(struct vm86_cpu *cpu,struct int21_state *st,uint8_t *ch)
{
    if(st->extended_pending) { *ch=st->extended_key; st->extended_pending=false; return true; }
    uint16_t word;
    if(!bios16_take(cpu,&word)) return false;
    *ch=(uint8_t)word;
    if(!*ch) { st->extended_key=(uint8_t)(word>>8); st->extended_pending=true; }
    return true;
}
static void console_break(struct vm86_cpu *cpu,struct int21_state *st)
{
    st->line_active=false;
    vm86_interrupt(cpu,VM86_INT_CTRL_BREAK);
}
static void console_input(struct vm86_cpu *cpu,struct int21_state *st)
{
    uint8_t fn=cpu->ah, ch;
    uint16_t word;
    if(fn==0x0B) {
        if(!st->extended_pending && bios16_peek(cpu,&word) && (uint8_t)word==3) {
            (void)bios16_take(cpu,&word); console_break(cpu,st); return;
        }
        cpu->al=(st->extended_pending || bios16_peek(cpu,&word)) ? 0xFF : 0; dos_ok(cpu); return;
    }
    if(fn==0x06 && cpu->dl!=0xFF) { output_char(cpu,st); return; }
    if(fn==0x0A) {
        uint16_t seg=cpu->ds, off=cpu->dx;
        if(!guest_range(cpu,seg,off,2)) { dos_fail(cpu,13); return; }
        uint8_t max=vm86_mem_read8(cpu->mem,guest_address(seg,off,0));
        if(!max) { vm86_mem_write8(cpu->mem,guest_address(seg,off,1),0); dos_ok(cpu); return; }
        if(!guest_range(cpu,seg,off,(unsigned)max+2)) { dos_fail(cpu,13); return; }
        if(!st->line_active || st->line_segment!=seg || st->line_offset!=off) {
            st->line_active=true; st->line_segment=seg; st->line_offset=off; st->line_length=0;
        }
        while(console_take(cpu,st,&ch)) {
            if(!ch) { (void)console_take(cpu,st,&ch); continue; }
            if(ch==3) { console_break(cpu,st); return; }
            if(ch==13) {
                vm86_mem_write8(cpu->mem,guest_address(seg,off,2+st->line_length),13);
                vm86_mem_write8(cpu->mem,guest_address(seg,off,1),st->line_length);
                bios10_tty(cpu,st->video,13); st->line_active=false; dos_ok(cpu); return;
            }
            if(ch==8) {
                if(st->line_length) { st->line_length--; bios10_tty(cpu,st->video,8); bios10_tty(cpu,st->video,' '); bios10_tty(cpu,st->video,8); }
            } else if(st->line_length<max-1u) {
                vm86_mem_write8(cpu->mem,guest_address(seg,off,2+st->line_length++),ch);
                bios10_tty(cpu,st->video,ch);
            }
        }
        vm86_service_retry(cpu); return;
    }
    if(!console_take(cpu,st,&ch)) {
        if(fn==0x06) { cpu->al=0; vm86_flag_set(cpu,VM86_ZF,true); dos_ok(cpu); }
        else vm86_service_retry(cpu);
        return;
    }
    cpu->al=ch;
    if(ch==3 && (fn==0x01 || fn==0x08)) { console_break(cpu,st); return; }
    if(fn==0x01) bios10_tty(cpu,st->video,ch);
    if(fn==0x06) vm86_flag_set(cpu,VM86_ZF,false);
    dos_ok(cpu);
}
static void file_transfer(struct vm86_cpu *cpu,struct int21_state *st,bool writing)
{
    unsigned count=cpu->cx, total=0; uint8_t buf[512]; int error=0;
    if(!guest_range(cpu,cpu->ds,cpu->dx,count)) { dos_fail(cpu,13); return; }
    if(writing && (cpu->bx==1 || cpu->bx==2)) {
        for(unsigned i=0;i<count;i++) bios10_tty(cpu,st->video,vm86_mem_read8(cpu->mem,guest_address(cpu->ds,cpu->dx,i)));
        cpu->ax=(uint16_t)count; dos_ok(cpu); return;
    }
    if(!writing && cpu->bx==0) {
        uint8_t ch;
        if(!count) { cpu->ax=0; dos_ok(cpu); return; }
        if(!console_take(cpu,st,&ch)) { vm86_service_retry(cpu); return; }
        vm86_mem_write8(cpu->mem,guest_address(cpu->ds,cpu->dx,0),ch);
        cpu->ax=1; dos_ok(cpu); return;
    }
    if(!st->files) { dos_fail(cpu,6); return; }
    do {
        uint32_t n=count-total, done=0; if(n>sizeof(buf)) n=sizeof(buf);
        if(writing) {
            for(unsigned i=0;i<n;i++) buf[i]=vm86_mem_read8(cpu->mem,guest_address(cpu->ds,cpu->dx,total+i));
            error=fat_write(st->files,cpu->bx,buf,n,&done);
        } else {
            error=fat_read(st->files,cpu->bx,buf,n,&done);
            if(!error) for(unsigned i=0;i<done;i++) vm86_mem_write8(cpu->mem,guest_address(cpu->ds,cpu->dx,total+i),buf[i]);
        }
        total+=done;
        if(error || done<n) break;
    } while(total<count);
    if(error && !total) dos_fail(cpu,(uint16_t)error);
    else { cpu->ax=(uint16_t)total; dos_ok(cpu); }
}
static void find_file(struct vm86_cpu *cpu,struct int21_state *st,bool first)
{
    uint16_t seg=st->dta_segment, off=st->dta_offset, dir=0;
    uint8_t pattern[11], attr; uint32_t index=0, entry; int e;
    if(!guest_range(cpu,seg,off,43)) { dos_fail(cpu,13); return; }
    if(!st->files) { dos_fail(cpu,15); return; }
    if(first) {
        char path[128]; if(!guest_path(cpu,path)) { dos_fail(cpu,3); return; }
        e=fat_parent(st->files,path,&dir,pattern,true); if(e) { status(cpu,e); return; }
        attr=(uint8_t)cpu->cx;
    } else {
        if(vm86_mem_read8(cpu->mem,guest_address(seg,off,0))!=0xF5) { dos_fail(cpu,18); return; }
        for(unsigned i=0;i<11;i++) pattern[i]=vm86_mem_read8(cpu->mem,guest_address(seg,off,1+i));
        attr=vm86_mem_read8(cpu->mem,guest_address(seg,off,12));
        dir=vm86_mem_read16(cpu->mem,guest_address(seg,off,13));
        for(unsigned i=0;i<4;i++) index|=(uint32_t)vm86_mem_read8(cpu->mem,guest_address(seg,off,15+i))<<(8*i);
    }
    e=fat_find(st->files,dir,&index,pattern,attr,&entry);
    if(e) { status(cpu,e); return; }
    /* DOS leaves bytes 00h..14h reserved: our continuation is DTA-local,
     * so two searches using different DTAs can proceed independently. */
    uint8_t result[43]={0}; result[0]=0xF5;
    for(unsigned i=0;i<11;i++) result[1+i]=pattern[i];
    result[12]=attr; result[13]=(uint8_t)dir; result[14]=(uint8_t)(dir>>8);
    for(unsigned i=0;i<4;i++) result[15+i]=(uint8_t)(index>>(8*i));
    uint8_t *p=st->files->image+entry; result[21]=p[11];
    for(unsigned i=0;i<4;i++) { result[22+i]=p[22+i]; result[26+i]=p[28+i]; }
    unsigned n=30; for(unsigned i=0;i<8 && p[i]!=' ';i++) result[n++]=p[i];
    if(p[8]!=' ') { result[n++]='.'; for(unsigned i=8;i<11 && p[i]!=' ';i++) result[n++]=p[i]; }
    for(unsigned i=0;i<43;i++) vm86_mem_write8(cpu->mem,guest_address(seg,off,i),result[i]);
    dos_ok(cpu);
}
static void file_call(struct vm86_cpu *cpu,struct int21_state *st)
{
    uint8_t fn=cpu->ah; int e=0; char path[128]; uint16_t handle; uint32_t pos;
    if(fn==0x3F || fn==0x40) { file_transfer(cpu,st,fn==0x40); return; }
    if(fn==0x4E || fn==0x4F) { find_file(cpu,st,fn==0x4E); return; }
    if(!st->files) { dos_fail(cpu,15); return; }
    if(fn==0x3C || fn==0x3D || fn==0x41) {
        if(!guest_path(cpu,path)) { dos_fail(cpu,3); return; }
        if(fn==0x41) e=fat_unlink(st->files,path);
        else {
            uint8_t mode=fn==0x3C ? 2 : (uint8_t)(cpu->al&7);
            e=(fn==0x3D && (cpu->al&0x78)) ? 12 : fat_open(st->files,path,mode,fn==0x3C,cpu->cx,&handle);
            if(!e) cpu->ax=handle;
        }
    } else if(fn==0x3E) e=fat_close(st->files,cpu->bx);
    else if(fn==0x42) {
        int32_t delta=(int32_t)(((uint32_t)cpu->cx<<16)|cpu->dx);
        e=fat_seek(st->files,cpu->bx,cpu->al,delta,&pos);
        if(!e) { cpu->ax=(uint16_t)pos; cpu->dx=(uint16_t)(pos>>16); }
    }
    status(cpu,e);
}

void int21_service(struct vm86_cpu *cpu, void *ctx)
{
    struct int21_state *st = ctx;

    switch (cpu->ah) {
    case 0x01: case 0x06: case 0x07: case 0x08: case 0x0A: case 0x0B:
        console_input(cpu, st); return;
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40:
    case 0x41: case 0x42: case 0x4E: case 0x4F:
        file_call(cpu, st); return;
    case VM86_INT21_TERMINATE:
        /* AH=00h, the old exit. It carries no return code, and DOS 2 and
         * later treat it as AH=4Ch with AL=0 -- which is what a bare RET
         * from a .COM amounts to, since the loader's zero word lands on
         * INT 20h and that is the same ending by another door. */
        vm86_service_exit(cpu, 0u);
        return;

    case VM86_INT21_OUTPUT_CHAR:
        output_char(cpu, st);
        return;

    case VM86_INT21_OUTPUT_STRING:
        print_string(cpu, st);
        return;

    case VM86_INT21_DISK_RESET:
        /* Flush DOS's buffers and forget everything it thinks it knows
         * about the medium. This machine has no buffers and no medium, so
         * there is nothing to do -- and CF clear, because a program that
         * resets the disk before reading it must not be told the reset
         * failed. */
        dos_ok(cpu);
        return;

    case VM86_INT21_SELECT_DRIVE:
        select_drive(cpu);
        return;

    case VM86_INT21_GET_DRIVE:
        get_drive(cpu);
        return;

    case VM86_INT21_SET_DTA:
        set_dta(cpu, st);
        return;

    case VM86_INT21_SET_VECTOR:
        set_vector(cpu);
        return;

    case VM86_INT21_GET_DTA:
        get_dta(cpu, st);
        return;

    case VM86_INT21_GET_VERSION:
        get_version(cpu);
        return;

    case VM86_INT21_GET_VECTOR:
        get_vector(cpu);
        return;

    case VM86_INT21_TERMINATE_CODE:
        /* AL is the return code and it goes to the host. Nothing is
         * restored and nothing is freed -- see docs/dos-refs-dos.md
         * section 9 for what real DOS does here and why this machine does
         * not do it. */
        vm86_service_exit(cpu, cpu->al);
        return;

    default:
        /*
         * A function number this machine does not implement.
         *
         * AL goes to zero and the carry flag goes up, and AH is left
         * alone -- so a program that wants to know *which* call it made
         * that was not answered still has the number in front of it.
         * That is FreeDOS's behaviour now and the reference says why it
         * is not an error code: an early FreeDOS returned AX=1 here and
         * broke 4DOS, which reads AX after seeing CF and does not find a
         * byte of zero.
         */
        cpu->al = 0u;
        vm86_flag_set(cpu, VM86_CF, true);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* INT 20h                                                             */
/* ------------------------------------------------------------------ */

void int21_terminate_service(struct vm86_cpu *cpu, void *ctx)
{
    (void)ctx;   /* INT 20h carries nothing; see the note in int21.h */

    vm86_service_exit(cpu, 0u);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void int21_reset(struct int21_state *st, struct bios10_state *video,
                 uint16_t psp_segment)
{
    st->video = video;
    st->files = NULL;
    st->extended_key = 0;
    st->extended_pending = false;
    st->line_active = false;
    st->line_length = 0;
    st->line_segment = st->line_offset = 0;

    /*
     * The default DTA is the program's own command tail. A machine with
     * no program loaded gets zero for both halves, which is an address
     * with nothing behind it -- see the note in int21.h on why the
     * parameter exists at all.
     */
    st->dta_segment = psp_segment;
    st->dta_offset  = psp_segment ? (uint16_t)VM86_INT21_DEFAULT_DTA : 0u;
}

/* The default Ctrl-C vector terminates. Guest AH=25h can replace it,
 * and console_break follows that guest vector rather than bypassing it. */
void int21_break_service(struct vm86_cpu *cpu,void *ctx)
{
    (void)ctx;
    vm86_service_exit(cpu,3u);
}
