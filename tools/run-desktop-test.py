#!/usr/bin/env python3
"""G1-G4: real PS/2 events, raw hotkey isolation, nested tabs, RAM snapshots
and every guest pixel checked. Artifacts remain in WSL-native BUILD_DIR.
"""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parent.parent
BUILD=Path(os.environ.get('FUNYOS_BUILD_DIR','/var/tmp/funyos-build'))
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
Q=load('desktop_qmp',ROOT/'tools/run-dos-resources-test.py')
P=load('desktop_pixels',ROOT/'tools/check-screen-pixels.py')
FONT=P.read_font(str(ROOT/'kernel/console/font8x16.c'))

def pointer_pixel(dx,dy):
    if not (0<=dx<12 and 0<=dy<16): return None
    if not (dx<=dy//2 if dy<12 else 3<=dx<=5): return None
    edge=dx==0 or (dy<12 and dx==dy//2) or dy==0 or dy==15 or (dy>=12 and dx in (3,5))
    return b'\0\0\0' if edge else b'\xff\xff\xff'

def check_guest(path,title,origin,pointer=None):
    width,height,pixels=P.read_ppm(str(path));c0,r0=origin
    if not (0<=c0<width//8 and 1<=r0<height//16): raise RuntimeError('bad guest placement')
    checked=0;overlay=0
    for r in range(25):
        for c in range(80):
            char=ord(title[c]) if r==0 and c<len(title) else 32
            glyph=FONT[char]
            for y in range(16):
                for x in range(8):
                    px=(c0+c)*8+x;py=(r0+r)*16+y
                    expected=b'\xaa\xaa\xaa' if glyph[y]&(0x80>>x) else b'\0\0\xaa'
                    if r==1 and c==0 and y>=14: expected=b'\xaa\xaa\xaa'
                    if pointer:
                        over=pointer_pixel(px-pointer[0],py-pointer[1])
                        if over is not None:
                            expected=over;overlay+=1
                    at=(py*width+px)*3
                    if pixels[at:at+3]!=expected:
                        raise RuntimeError(f'{path.name}: pixel {px},{py} differs; snapshot/restore/overlay broken')
                    checked+=1
    # The strip is not just exempted: title glyphs and active attributes checked.
    for label,start in (('[1 Log]',0),('[2 Program]',20)):
        foreground=b'\xff\xff\xff' if label.startswith('[2') else b'\xaa\xaa\xaa'
        for c,char in enumerate(label):
            for y in range(16):
                for x in range(8):
                    px=(start+c)*8+x;py=y
                    expected=foreground if FONT[ord(char)][y]&(0x80>>x) else b'\0\0\xaa'
                    if pointer:
                        over=pointer_pixel(px-pointer[0],py-pointer[1])
                        if over is not None: expected=over
                    at=(py*width+px)*3
                    if pixels[at:at+3]!=expected:
                        raise RuntimeError(f'{path.name}: incorrect label/active highlight at {px},{py}')
    if pointer and c0*8<=pointer[0]<(c0+80)*8 and r0*16<=pointer[1]<(r0+25)*16 and not overlay:
        raise RuntimeError('pointer never changed any guest pixel')
    print(f'  [ok] {path.name}: {checked} guest pixels, tabs, cursor and {overlay} overlay pixels',flush=True)

def run(mode):
    work=BUILD/'desktop-test'/mode;work.mkdir(parents=True,exist_ok=True)
    tree=work/'root'
    if tree.exists(): shutil.rmtree(tree)
    shutil.copytree(BUILD/'iso_root',tree)
    (tree/'boot/limine.conf').write_text('timeout: 0\nserial: yes\nserial_baudrate: 115200\n/FunnyOS\n    protocol: limine\n    kernel_path: fslabel(FUNNYOS):/boot/funyos.elf\n    resolution: 1024x768x32\n    cmdline: selftest=desktop\n')
    iso=work/'test.iso'
    subprocess.run(['xorriso','-as','mkisofs','-R','-r','-J','-V','FUNNYOS','-b','boot/limine-bios-cd.bin','-no-emul-boot','-boot-load-size','4','-boot-info-table','--efi-boot','boot/limine-uefi-cd.bin','-efi-boot-part','--efi-boot-image',str(tree),'-o',str(iso)],check=True,capture_output=True)
    log=work/'serial.log';log.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='funyos-desktop-',dir='/var/tmp') as temp:
        socket=Path(temp)/'qmp'
        command=['qemu-system-x86_64','-m','512','-cdrom',str(iso),'-display','none','-serial','file:'+str(log),'-qmp','unix:'+str(socket)+',server=on,wait=off','-no-reboot','-cpu','max']
        if mode=='uefi':
            variables=work/'vars.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
            command+=['-M','q35','-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
        with (work/'qemu.log').open('w') as diagnostics:
            process=subprocess.Popen(command,stdout=diagnostics,stderr=diagnostics);monitor=None
            read=lambda: log.read_text(errors='replace') if log.exists() else ''
            try:
                Q.wait_for(socket.exists,process,'QMP');monitor=Q.Monitor(socket)
                def wait(marker): Q.wait_for(lambda: marker in read(),process,marker)
                def capture(name):
                    time.sleep(.15);path=work/(name+'.ppm');monitor.command('screendump',{'filename':str(path)});return path
                def origin():
                    matches=re.findall(r'Screen\s+: a program has the screen, 80 columns at cell (\d+),(\d+)',read())
                    return tuple(map(int,matches[-1]))
                def pointer():
                    matches=re.findall(r'Mouse event\s+: .* at=(\d+),(\d+)',read())
                    return tuple(map(int,matches[-1])) if matches else None
                def relative(dx,dy):
                    start=len(read())
                    monitor.command('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]})
                    Q.wait_for(lambda:'Mouse event' in read()[start:],process,'PS/2 relative packet')
                def move_to(x,y):
                    current=pointer() or (512,384)
                    while current!=(x,y):
                        dx=max(-80,min(80,x-current[0]));dy=max(-80,min(80,y-current[1]));relative(dx,dy);current=pointer()
                def tab(number,index):
                    start=len(read());monitor.key('alt-'+str(number))
                    Q.wait_for(lambda:f'selected slot {index}' in read()[start:],process,'Alt tab')
                def click():
                    for down in (True,False):
                        start=len(read());monitor.command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
                        Q.wait_for(lambda:'Mouse event' in read()[start:],process,'mouse button')
                wait('G parent ready')
                if 'Mouse          : PS/2' not in read(): raise RuntimeError('AUX mouse not initialized')
                parent_origin=origin();check_guest(capture('parent-clean'),'G parent kernel snapshot',parent_origin)
                relative(7,-3)
                if 'dx=7 dy=-3 total=7,-3' not in read(): raise RuntimeError('PS/2 sign or Y axis incorrect')
                check_guest(capture('pointer-first'),'G parent kernel snapshot',parent_origin,pointer())
                relative(21,11)
                if 'total=28,8' not in read(): raise RuntimeError('cumulative movement incorrect')
                check_guest(capture('pointer-moved'),'G parent kernel snapshot',parent_origin,pointer())
                tab(1,0);capture('log-alt');tab(2,1)
                check_guest(capture('parent-alt-restored'),'G parent kernel snapshot',parent_origin,pointer())
                move_to(40,8);click();wait('selected slot 0');capture('log-click')
                move_to(200,8);click();wait('selected slot 1')
                check_guest(capture('parent-click-restored'),'G parent kernel snapshot',parent_origin,pointer())
                monitor.key('ret');wait('G child ready');child_origin=origin()
                # Child is third active tab, checked below after selecting parent.
                tab(2,1);check_guest(capture('parent-while-child'),'G parent kernel snapshot',parent_origin,pointer())
                monitor.key('x') # hidden child must NOT receive this
                tab(3,2);capture('child-restored')
                monitor.key('ret');wait('G parent restored');wait('G child returned 23')
                tab(2,1);check_guest(capture('parent-after-child'),'G parent kernel snapshot',parent_origin,pointer())
                monitor.key('ret');wait('G desktop acceptance: PASS')
                capture('single-log-after-release')
                text=read()
                if re.findall(r'G raw key: ([0-9a-f]+)',text)!=['1c']*3: raise RuntimeError('unexpected raw input: Alt/digit leaked or Enter not delivered')
                for bad in ('FAIL','PANIC','CPU EXCEPTION'):
                    if bad in text: raise RuntimeError('unexpected diagnostic: '+bad)
                monitor.command('quit');process.wait(timeout=5)
            finally:
                if monitor: monitor.close()
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill();process.wait()
    print(f'====> G1-G4 desktop test PASSED ({mode})',flush=True)

if __name__=='__main__':
    for mode in sys.argv[1:] or ['bios','uefi']:
        if mode not in ('bios','uefi'): raise SystemExit('expected bios|uefi')
        run(mode)
