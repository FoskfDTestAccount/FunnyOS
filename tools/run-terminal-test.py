#!/usr/bin/env python3
"""Normal-boot native terminal acceptance: real Shells, half lines, DOS wait,
mouse/tab lifecycle, repeated allocation and pixel-level text isolation."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parent.parent
BUILD=Path(os.environ.get('FUNYOS_BUILD_DIR','/var/tmp/funyos-build'))
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
Q=load('terminal_qmp',ROOT/'tools/run-dos-resources-test.py')
P=load('terminal_pixels',ROOT/'tools/check-screen-pixels.py')
FONT=P.read_font(str(ROOT/'kernel/console/font8x16.c'))

def check_text(path,expected,forbidden=()):
    width,height,data=P.read_ppm(str(path));cols=width//8;rows=height//16
    lines=[];checks=0
    for r in range(1,rows-1):
        text=''
        for c in range(cols):
            bits=[]
            # Cursor occupies bottom two rows: ignore those rows only.
            for y in range(14):
                byte=0
                for x in range(8):
                    at=((r*16+y)*width+c*8+x)*3
                    rgb=data[at:at+3]
                    if rgb==b'\xaa\xaa\xaa': byte|=0x80>>x
                    elif rgb!=b'\0\0\0':
                        raise RuntimeError(f'{path.name}: unexpected terminal color at {c},{r}')
                    checks+=1
                bits.append(byte)
            glyph=[chr(ch) for ch in sorted(FONT) if 32<=ch<127 and list(FONT[ch][:14])==bits]
            if not glyph: raise RuntimeError(f'{path.name}: malformed terminal glyph at {c},{r}')
            text+=glyph[0]
        lines.append(text.rstrip())
    text='\n'.join(lines)
    for value in expected:
        if value not in text: raise RuntimeError(f'{path.name}: missing {value!r}\n{text}')
    for value in forbidden:
        if value in text: raise RuntimeError(f'{path.name}: foreign session output {value!r}')
    print(f'  [ok] {path.name}: {checks} pixels, expected output and isolation',flush=True)


def run(mode, build_dir=None, log_root=None, app_suite=False):
    build=Path(build_dir) if build_dir is not None else BUILD
    work=(Path(log_root) if log_root is not None else build/'terminal-test')/mode
    work.mkdir(parents=True,exist_ok=True)
    log=work/'serial.log';log.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='funyos-terminal-',dir='/var/tmp') as temp:
        socket=Path(temp)/'qmp'
        command=['qemu-system-x86_64','-m','512','-cdrom',str(build/'funyos.iso'),'-display','none','-serial','file:'+str(log),'-qmp','unix:'+str(socket)+',server=on,wait=off','-no-reboot','-cpu','max']
        if mode=='uefi':
            import shutil
            variables=work/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
            command+=['-M','q35','-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
        with (work/'qemu.log').open('w') as diagnostics:
            process=subprocess.Popen(command,stdout=diagnostics,stderr=diagnostics);monitor=None
            read=lambda: log.read_text(errors='replace') if log.exists() else ''
            try:
                Q.wait_for(socket.exists,process,'QMP');monitor=Q.Monitor(socket)
                def wait(marker,start=0): Q.wait_for(lambda:marker in read()[start:],process,marker)
                def key(value): monitor.key(value);time.sleep(.12)
                names={' ':'spc','.':'dot','/':'slash','-':'minus','\\':'backslash'}
                def text(value):
                    for ch in value: key(names.get(ch,ch))
                def line(value,marker=None):
                    start=len(read());text(value);key('ret')
                    if marker: wait(marker,start)
                def capture(name,expected=(),forbidden=()):
                    time.sleep(.1);path=work/(name+'.ppm');monitor.command('screendump',{'filename':str(path)})
                    if expected: check_text(path,expected,forbidden)
                    return path
                def select(n,id):
                    start=len(read());key('alt-'+str(n));wait('selected session '+str(id),start)
                position=[512,384]
                def move_to(x,y):
                    while position!=[x,y]:
                        dx=max(-60,min(60,x-position[0]));dy=max(-60,min(60,y-position[1]))
                        monitor.command('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]})
                        position[0]+=dx;position[1]+=dy;time.sleep(.07)
                def click_at(x,y):
                    move_to(x,y)
                    for down in (True,False):
                        monitor.command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.15)
                    move_to(1023,767) # pointer stays out of the text oracle
                wait('F:\\> ')
                line('echo alpha','alpha')
                text('echo unfinished')
                key('ctrl-shift-t');wait('created session 2');wait('FunnyCOM 0.1',read().find('created session 2'))
                line('echo beta','beta')
                line('cls')
                line('echo betaonly','betaonly')
                capture('session-b',['betaonly','F:\\> '],['alpha','unfinished'])
                select(1,1);key('backspace');text('x');key('ret');wait('unfinishex')
                capture('session-a',['alpha','unfinishex'],['betaonly'])
                # DOS waits in A, B must remain usable. Raw hotkeys must not
                # become guest characters or consume B's translated queue.
                line('dos keys.com');time.sleep(.4)
                key('ctrl-shift-w');wait('Foreground program is running')
                capture('busy-close-rejected')
                select(2,2);time.sleep(5.3);line('echo duringdos','duringdos')
                capture('shell-while-dos',['betaonly','duringdos'],['alpha'])
                select(1,1)
                for value in ('a','shift-b','up','f1','c','d','backspace','e','ret'): key(value)
                wait('DOS program returned 0')
                line('echo afterdos','afterdos')
                capture('dos-restored',['alpha','afterdos'],['betaonly','duringdos'])
                # M6: the FAT image contains a real MZ image. It must enter
                # through relocated CS:IP/SS:SP and return through AH=4Ch.
                line('dos mztest.exe');wait('M6 MZ loader PASS');wait('DOS program returned 0')
                # M6: EXEC.COM launches a second DOS image, waits for it,
                # restores the parent PSP, and observes AH=4Dh's return code.
                line('dos exec.com');wait('M6 EXEC child PASS');wait('M6 EXEC parent PASS');wait('DOS program returned 0')
                if app_suite:
                    # Opt-in smoke only. The binaries are external MS-DOS 4.0
                    # artifacts supplied through M6_APPS_DIR; this does not
                    # turn a prompt/exit check into full application coverage.
                    line('dos edlin.com','End of input file')
                    line('q','DOS program returned 0')
                    line('dos debug.com','-')
                    line('q','DOS program returned 0')
                line('echo afterexec','afterexec')
                line('echo aftermz','aftermz')
                line('check','check: resumable child ready')
                select(2,2);line('echo nestedother','nestedother')
                time.sleep(1.6);select(1,1)
                wait('check: resumable child PASS');wait('check: nested process PASS')
                wait('exact results : 0 wrong')
                # Starting a second VM must not overwrite A's VM globals.
                line('dos keys.com');time.sleep(.3);select(2,2)
                line('dos files.com','DOS program returned 0');select(1,1)
                for value in ('a','shift-b','up','f1','c','d','backspace','e','ret'): key(value)
                wait('DOS program returned 0',read().rfind('selected session 1'))
                line('dos keys.com');time.sleep(.3)
                key('ctrl-shift-k');wait('VM: stopped by user')
                wait('DOS program returned 1')
                line('echo cancelled','cancelled')
                # User tab creation command and mouse +/- entry paths.
                line('tab new','created session 3');wait('FunnyCOM 0.1',read().find('created session 3'))
                line('echo third','third')
                # Three 20-cell tabs -> plus at column 60.
                click_at(61*8+2,8);wait('created session 4')
                line('echo fourth','fourth')
                # x for the first tab at cell 17; closes inactive A without
                # deleting the running B/C/D sessions.
                click_at(17*8+2,8)
                line('tab','3 open tab(s)')
                capture('close-first',['betaonly','duringdos'],['third','fourth'])
                # LIMIT/rollback/reuse. Slots reused in order, labels ordinal.
                for i in range(5):
                    start=len(read());key('ctrl-shift-t');wait('created session',start)
                line('tab','8 open tab(s)')
                key('ctrl-shift-t');wait('session limit or insufficient memory')
                capture('limit')
                line('exit','exited 0')
                line('tab','7 open tab(s)')
                key('ctrl-shift-t');time.sleep(.4)
                line('tab','8 open tab(s)')
                start=len(read());line('tab stats','Terminal stats:')
                stats=lambda text: tuple(map(int,re.findall(r'Terminal stats: pages=(\d+) heap=(\d+) ring3=(\d+) offstack=(\d+)',text)[-1]))
                before=stats(read()[start:])
                # Repeated exit/new cycles catch stale context or raw ownership.
                for i in range(3):
                    key('ctrl-shift-w');time.sleep(.3)
                    line('tab','7 open tab(s)')
                    key('ctrl-shift-t');time.sleep(.3)
                    line('echo cycle'+str(i),'cycle'+str(i))
                start=len(read());line('tab stats','Terminal stats:')
                after=stats(read()[start:])
                if before[:2]!=after[:2] or after[3]!=0 or after[2]==0:
                    raise RuntimeError('resource leak or interrupt stack mismatch: '+str((before,after)))
                print('  [ok] repeated close/new: exact PMM+heap balance and Ring3 stack ownership',flush=True)
                # Close all sessions: last close creates a replacement Shell.
                for i in range(8): key('ctrl-shift-w');time.sleep(.2)
                line('tab','1 open tab(s)')
                capture('last-replaced',['1 open tab(s)','F:\\> '],['alpha','betaonly','cycle'])
                for bad in ('PANIC','CPU EXCEPTION','CLOBBERED','check: nested process FAIL','check: resumable child FAIL','RUN LIMIT','stream overflow'):
                    if bad in read(): raise RuntimeError('unexpected '+bad)
                monitor.command('quit');process.wait(timeout=5)
            finally:
                if monitor: monitor.close()
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill();process.wait()
    print(f'====> Native terminal sessions PASSED ({mode})',flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('modes',nargs='*',choices=('bios','uefi'),default=['bios','uefi'])
    parser.add_argument('--build-dir',type=Path,default=BUILD,
                        help='build directory containing funyos.iso')
    parser.add_argument('--log-dir',type=Path,
                        help='root for archived terminal logs and screenshots')
    parser.add_argument('--app-suite',action='store_true',
                        help='also smoke-test external EDLIN.COM and DEBUG.COM')
    args=parser.parse_args()
    for mode in args.modes or ['bios','uefi']:
        run(mode,args.build_dir,args.log_dir,args.app_suite)
