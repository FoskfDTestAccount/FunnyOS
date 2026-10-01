#!/usr/bin/env python3
"""W5/W6 BIOS+UEFI acceptance: disk resource -> Ring 3 FAT -> DOS;
8042 make/break/prefix -> raw syscall -> INT09 -> BIOS/DOS input.
Waits for markers, not boot-time guesses, and checks framebuffer pixels.
Artifacts are kept in the native build directory for diagnosis.
"""
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get('FUNYOS_BUILD_DIR', '/var/tmp/funyos-build'))
LIMIT = int(os.environ.get('QEMU_TIMEOUT', '90'))


def wait_for(predicate, process, description):
    deadline = time.monotonic() + LIMIT
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise RuntimeError('QEMU ended while waiting for ' + description)
        time.sleep(0.05)
    raise RuntimeError('timeout waiting for ' + description)


class Monitor:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.connect(str(path))
        self.socket.settimeout(10)
        self.file = self.socket.makefile('rwb')
        self.file.readline()  # QMP greeting
        self.command('qmp_capabilities')

    def command(self, execute, arguments=None):
        request = {'execute': execute}
        if arguments:
            request['arguments'] = arguments
        self.file.write(json.dumps(request).encode() + b'\n')
        self.file.flush()
        while True:
            response = json.loads(self.file.readline())
            if 'error' in response:
                raise RuntimeError(str(response['error']))
            if 'return' in response:
                return response['return']

    def key(self, name):
        result = self.command('human-monitor-command',
                              {'command-line': 'sendkey ' + name})
        if result.strip():
            raise RuntimeError('sendkey rejected: ' + result)
        time.sleep(0.2)  # allow QEMU's 100ms release event, including modifiers

    def close(self):
        self.file.close()
        self.socket.close()


def run(kind, firmware):
    work = BUILD / 'dos-resources-test' / (kind + '-' + firmware)
    work.mkdir(parents=True, exist_ok=True)
    tree = work / 'root'
    if tree.exists():
        shutil.rmtree(tree)
    shutil.copytree(BUILD / 'iso_root', tree)
    (tree / 'boot/limine.conf').write_text(
        'timeout: 0\nserial: yes\nserial_baudrate: 115200\n'
        '/FunnyOS\n    protocol: limine\n'
        '    kernel_path: fslabel(FUNNYOS):/boot/funyos.elf\n'
        '    resolution: 1024x768x32\n    cmdline: vm=' + kind + '\n')
    iso = work / 'test.iso'
    subprocess.run(['xorriso', '-as', 'mkisofs', '-R', '-r', '-J', '-V', 'FUNNYOS',
                    '-b', 'boot/limine-bios-cd.bin', '-no-emul-boot',
                    '-boot-load-size', '4', '-boot-info-table', '--efi-boot',
                    'boot/limine-uefi-cd.bin', '-efi-boot-part', '--efi-boot-image',
                    str(tree), '-o', str(iso)], check=True, capture_output=True)
    log = work / 'serial.log'
    ppm = work / 'screen.ppm'
    qmp = work / 'qmp.sock'
    for path in (log, ppm, qmp):
        path.unlink(missing_ok=True)
    # Keep the socket below AF_UNIX's 108-byte limit even for custom build paths.
    import tempfile
    with tempfile.TemporaryDirectory(prefix='funyos-qmp-', dir='/var/tmp') as tmp:
        qmp = pathlib.Path(tmp) / 'monitor'
        command = ['qemu-system-x86_64', '-m', '512', '-cdrom', str(iso),
                   '-display', 'none', '-serial', 'file:' + str(log),
                   '-qmp', 'unix:' + str(qmp) + ',server=on,wait=off', '-no-reboot']
        if firmware == 'uefi':
            variables = work / 'OVMF_VARS.fd'
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
            command += ['-M', 'q35', '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                        '-drive', 'if=pflash,format=raw,unit=1,file=' + str(variables)]
        else:
            command += ['-boot', 'd']
        command += ['-enable-kvm', '-cpu', 'host'] if os.access('/dev/kvm', os.W_OK) else ['-cpu', 'max']
        with (work / 'qemu.log').open('w') as diagnostics:
            process = subprocess.Popen(command, stdout=diagnostics, stderr=diagnostics)
            monitor = None
            try:
                wait_for(qmp.exists, process, 'QMP socket')
                monitor = Monitor(qmp)
                read = lambda: log.read_text(errors='replace') if log.exists() else ''
                if kind == 'keys':
                    wait_for(lambda: 'W6 input ready' in read(), process, 'guest BIOS/DOS empty polls and prompt')
                    for key in ('a', 'shift-b', 'up', 'f1', 'c', 'd', 'backspace', 'e', 'ret'):
                        monitor.key(key)
                number = 6 if kind == 'keys' else 5
                marker = f'W{number} screen ready'
                wait_for(lambda: marker in read(), process, marker)
                time.sleep(0.1)
                monitor.command('screendump', {'filename': str(ppm)})
                subprocess.run([sys.executable, str(ROOT / 'tools/check-screen-pixels.py'),
                                str(ppm), str(log), str(ROOT / 'kernel/console/font8x16.c')], check=True)
                text = read()
                expected = ['code 0', f'W{number} {kind}: PASS']
                if kind == 'files':
                    expected.append('W5 FAT says hello')
                else:
                    if text.count('W6 pair: PASS') != 4:
                        raise RuntimeError('not exactly four independent scan/ASCII checks')
                    expected += ['W6 BIOS ready', 'W6 DOS ready']
                for value in expected:
                    if value not in text:
                        raise RuntimeError('missing: ' + value)
                for bad in ('FAIL', 'WARNING', 'PANIC', 'RUN LIMIT', 'CPU EXCEPTION'):
                    if bad in text:
                        raise RuntimeError('unexpected diagnostic: ' + bad)
                monitor.key('ret')
                wait_for(lambda: f'W{number} resource acceptance: PASS' in read(), process, 'resource cleanup')
                if 'translated nonblocking release key: PASS' not in read():
                    raise RuntimeError('nonblocking translated input never delivered the release key')
                monitor.command('quit')
                process.wait(timeout=5)
            finally:
                if monitor:
                    monitor.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    print(f'  [ok] W{number} {kind} ({firmware}): DOS, resource cleanup and pixels', flush=True)


def main():
    modes = sys.argv[1:] or ['bios', 'uefi']
    for mode in modes:
        if mode not in ('bios', 'uefi'):
            raise SystemExit('usage: run-dos-resources-test.py [bios|uefi ...]')
        for kind in ('files', 'keys'):
            run(kind, mode)
    print('====> W5/W6 DOS resource tests PASSED')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print('====> W5/W6 DOS resource tests FAILED:', error, file=sys.stderr)
        raise SystemExit(1)
