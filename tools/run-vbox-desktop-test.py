#!/usr/bin/env python3
"""WSL + Windows VBox acceptance. Creates/deletes only its own temporary VM.

Requires PowerShell, Windows VirtualBox and Pillow. Uses VBox's public COM
Mouse API, not USB/tablet input, guest additions or Windows SendInput.
Run after building a selftest=desktop ISO (see M4-G-preview.md).
"""
import argparse
import base64
import importlib.util
from pathlib import Path
import re
import subprocess
import time
import uuid
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def run(iso, vbox, powershell, terminals=False):
    token = uuid.uuid4().hex[:8]
    name = 'FunnyOS-G-test-' + token
    work = ROOT / '.cache' / 'm4g-vbox' / token
    work.mkdir(parents=True)

    def windows(path):
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip()

    def cli(*args):
        result = subprocess.run([vbox, *map(str, args)], text=True, capture_output=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout

    def mouse(dx, dy, buttons=0):
        # VM identity is a generated UUID; event values are integers only.
        script = f"""
$ErrorActionPreference='Stop'
$vbox=New-Object -ComObject VirtualBox.VirtualBox
$session=New-Object -ComObject VirtualBox.Session
$machine=$vbox.FindMachine('{identity}')
try {{
  $machine.LockMachine($session,1)
  $session.Console.Mouse.PutMouseEvent({int(dx)},{int(dy)},0,0,{int(buttons)})
}} finally {{
  if ($session.State -eq 2) {{ $session.UnlockMachine() }}
}}
"""
        result = subprocess.run([powershell, '-NoProfile', '-EncodedCommand',
                                 base64.b64encode(script.encode('utf-16le')).decode()],
                                text=True, capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    spec = importlib.util.spec_from_file_location('vbox_pixels', ROOT / 'tools/run-desktop-test.py')
    pixels = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pixels)
    registered = False
    running = False
    try:
        result = cli('createvm', '--name', name, '--ostype', 'Other_64',
                     '--basefolder', windows(work / 'vm'), '--register')
        registered = True
        identity = re.search(r'UUID:\s*([0-9a-f-]+)', result, re.I).group(1)
        cli('modifyvm', identity, '--memory', 256, '--cpus', 1, '--firmware', 'bios',
            '--graphicscontroller', 'vboxvga', '--vram', 16, '--mouse', 'ps2',
            '--keyboard', 'ps2', '--nic1', 'none', '--boot1', 'dvd', '--boot2', 'none',
            '--boot3', 'none', '--boot4', 'none', '--uart1', '0x3f8', 4)
        cli('storagectl', identity, '--name', 'IDE', '--add', 'ide')
        cli('storageattach', identity, '--storagectl', 'IDE', '--port', 1, '--device', 0,
            '--type', 'dvddrive', '--medium', windows(iso))
        for enabled in ('off', 'on'):
            log = work / ('ioapic-' + enabled + '.log')
            cli('modifyvm', identity, '--ioapic', enabled, '--uartmode1', 'file', windows(log))
            cli('startvm', identity, '--type', 'headless')
            running = True

            def read():
                return log.read_text(errors='replace') if log.exists() else ''

            def wait(predicate, description):
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    text = read()
                    if 'FAIL' in text or 'PANIC' in text or 'CPU EXCEPTION' in text:
                        raise RuntimeError(text[-4000:])
                    if predicate(text):
                        return
                    time.sleep(.1)
                raise RuntimeError('timeout: ' + description + '\n' + read()[-4000:])

            def marker(text):
                wait(lambda output: text in output, text)

            def key(*codes):
                cli('controlvm', identity, 'keyboardputscancode', *codes)
                time.sleep(.25)

            pointer_position=[512,384]
            def position():
                if terminals: return tuple(pointer_position)
                found = re.findall(r'Mouse event\s+: .* at=(\d+),(\d+)', read())
                return tuple(map(int, found[-1])) if found else (512, 384)

            def movement(dx, dy, buttons=0):
                start = len(read())
                mouse(dx, dy, buttons)
                if terminals:
                    pointer_position[0]=max(0,min(1023,pointer_position[0]+dx))
                    pointer_position[1]=max(0,min(767,pointer_position[1]+dy))
                    time.sleep(.12)
                else: wait(lambda text: 'Mouse event' in text[start:], 'PS/2 packet')

            def move_to(x, y):
                while position() != (x, y):
                    px, py = position()
                    movement(max(-80, min(80, x-px)), max(-80, min(80, y-py)))

            def capture(label, title=None):
                time.sleep(.15)
                png = work / (enabled + '-' + label + '.png')
                cli('controlvm', identity, 'screenshotpng', windows(png))
                if title:
                    origins = re.findall(r'Screen\s+: a program has the screen, 80 columns at cell (\d+),(\d+)', read())
                    ppm = png.with_suffix('.ppm')
                    with Image.open(png) as image:
                        image.convert('RGB').save(ppm)
                    pixels.check_guest(ppm, title, tuple(map(int, origins[0])), position())

            marker('F:\\> ' if terminals else 'G parent ready')
            if enabled == 'off':
                text = read()
                if not ('I/O APIC       : NOT AVAILABLE' in text and
                        'Keyboard       : NOT AVAILABLE' in text and
                        'Mouse          : NOT AVAILABLE' in text):
                    raise RuntimeError('missing APIC falsely advertised as a working device')
                mouse(7, -3)
                key('38', '02', '82', 'b8')
                if 'Mouse event' in read() or 'selected slot' in read():
                    raise RuntimeError('unexpected ISA input without I/O APIC')
                capture('missing-apic')
                print('VBox I/O APIC off: accurate unavailable-device diagnostics PASS', flush=True)
            elif terminals:
                spec=importlib.util.spec_from_file_location('vbox_terminals', ROOT/'tools/run-terminal-test.py')
                oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)
                def type_line(text):
                    cli('controlvm',identity,'keyboardputstring',text)
                    key('1c','9c')
                def check(label,expected,forbidden=()):
                    move_to(1023,767)
                    png=work/('on-'+label+'.png')
                    cli('controlvm',identity,'screenshotpng',windows(png))
                    ppm=png.with_suffix('.ppm')
                    with Image.open(png) as image: image.convert('RGB').save(ppm)
                    oracle.check_text(ppm,expected,forbidden)
                type_line('echo vboxalpha')
                cli('controlvm',identity,'keyboardputstring','echo unfinished')
                # New terminal by actual PS/2 mouse click on [+], column 21.
                move_to(21*8+2,8);movement(0,0,1);movement(0,0,0)
                marker('created session 2')
                type_line('echo vboxbeta')
                check('terminal-b',['vboxbeta'],['vboxalpha','unfinished'])
                key('38','02','82','b8');marker('selected session 1')
                key('1c','9c');marker('unfinis')
                check('terminal-a',['vboxalpha','unfinished'],['vboxbeta'])
                type_line('dos keys.com');time.sleep(.3)
                key('38','03','83','b8');marker('selected session 2')
                type_line('echo otherwhilewaiting')
                check('while-dos',['vboxbeta','otherwhilewaiting'],['vboxalpha'])
                key('38','02','82','b8')
                key('1e','9e','2a','30','b0','aa','e0','48','e0','c8','3b','bb',
                    '2e','ae','20','a0','0e','8e','12','92','1c','9c')
                marker('DOS program returned 0')
                check('dos-return',['vboxalpha','DOS program returned 0'],['vboxbeta'])
                # Active terminal close button: session 1 at cell 17.
                move_to(17*8+2,8);movement(0,0,1);movement(0,0,0)
                type_line('tab');marker('1 open tab(s)')
                check('closed',['vboxbeta','1 open tab(s)'],['vboxalpha'])
                key('1d','2a','14','94','aa','9d');marker('created session 1')
                type_line('echo reused')
                check('reused',['reused'],['vboxalpha','vboxbeta'])
                type_line('exit');marker('session 1 exited 0')
                type_line('echo survived')
                check('survived',['vboxbeta','survived'],['vboxalpha','reused'])
                print('VBox native terminals: mouse new/close, half input, DOS suspension, isolated Shells and reuse PASS',flush=True)
            else:
                marker('Mouse          : PS/2, IRQ 12 routed through the IO APIC')
                movement(7, -3)
                marker('dx=7 dy=-3 total=7,-3')
                capture('pointer', 'G parent kernel snapshot')
                # Both left/right Alt host chords must be consumed, including E0.
                for prefix in ((), ('e0',)):
                    key(*prefix, '38', '02', '82', *prefix, 'b8')
                    marker('selected slot 0')
                    key('2d', 'ad')  # hidden owner must not receive X
                    key(*prefix, '38', '03', '83', *prefix, 'b8')
                    marker('selected slot 1')
                if 'G raw key:' in read():
                    raise RuntimeError('host shortcut or hidden-page key leaked')
                move_to(40, 8); movement(0, 0, 1); movement(0, 0, 0)
                if re.findall(r'selected slot (\d+)', read())[-1] != '0':
                    raise RuntimeError('mouse click did not select Log')
                move_to(200, 8); movement(0, 0, 1); movement(0, 0, 0)
                if re.findall(r'selected slot (\d+)', read())[-1] != '1':
                    raise RuntimeError('mouse click did not select Program')
                capture('click-restored', 'G parent kernel snapshot')
                # Normal extended keys must not terminate the interactive fixture.
                key('e0', '48', 'e0', 'c8')
                expected = ['e0', '48', 'e0', 'c8']
                if re.findall(r'G raw key: ([0-9a-f]+)', read()) != expected:
                    raise RuntimeError('extended keyboard stream corrupted')
                key('1c', '9c'); marker('G child ready')
                key('1c', '9c'); marker('G parent restored'); marker('G child returned 23')
                # Child destruction selects Log; explicitly restore the parent focus.
                key('38', '03', '83', 'b8')
                key('1c', '9c'); marker('G desktop acceptance: PASS')
                raw = re.findall(r'G raw key: ([0-9a-f]+)', read())
                if raw[:4] != expected or raw[4:].count('1c') != 3 or any(
                        byte not in ('1c', '9c') for byte in raw[4:]):
                    raise RuntimeError('unexpected raw input in parent/child lifecycle')
                capture('completed')
                print('VBox I/O APIC on: mouse/pixels/clicks/left+right Alt/extended keys/nested pages PASS', flush=True)
            cli('controlvm', identity, 'poweroff')
            running = False
            time.sleep(1)
    finally:
        if registered:
            if running:
                cli('controlvm', identity, 'poweroff')
                time.sleep(1)
            cli('unregistervm', identity, '--delete')
        print('VBox test artifacts: ' + str(work), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=ROOT / 'dist/funyos-m4g-preview.iso')
    parser.add_argument('--vbox', default='/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe')
    parser.add_argument('--powershell', default='/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe')
    parser.add_argument('--terminals', action='store_true', help='test normal-boot terminal ISO instead of the desktop fixture')
    args = parser.parse_args()
    if args.terminals and args.iso==ROOT/'dist/funyos-m4g-preview.iso': args.iso=ROOT/'dist/funyos.iso'
    run(args.iso, args.vbox, args.powershell, args.terminals)
