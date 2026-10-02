#!/usr/bin/env python3
"""M5 tasks 1/2: pinned real-app baseline and a read-only compatibility probe.

Runs an unchanged MORE.COM. Missing DOS calls are NOT emulated by this tool.
A successful investigation may establish that the application is incompatible;
never confuse that with a successful application acceptance test.

WSL/Linux only. Optional reference requires DOSBOX or dosbox on PATH.
Artifacts default to /var/tmp/funyos-build/m5-app-probe, separate from make check.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / 'dos/apps/msdos2-more.json'
CASES = {
    'empty': b'',
    'short': b'First line\r\nSecond line\r\n',
    'controls': b'left\tright\r\nabc\bD\r\n',
    'ctrlz': b'Before EOF\r\n\x1aMUST NOT APPEAR\r\n',
    'wrap': b'W' * 81 + b'\r\n',
    'near-page': b''.join(f'Line {i:02d}\r\n'.encode() for i in range(1, 23)),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def expected_output(data):
    # Independent contract: startup CRLF, then unchanged bytes until DOS ^Z/EOF.
    # No case reaches the interactive page prompt; no renderer normalization.
    return b'\r\n' + data.split(b'\x1a', 1)[0]


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def read_manifest():
    return json.loads(MANIFEST.read_text(encoding='utf-8-sig'))


def verify_source(reference):
    manifest = read_manifest()
    binary = (reference / manifest['binary_path']).read_bytes()
    if len(binary) != manifest['size'] or digest(binary) != manifest['sha256']:
        raise RuntimeError('MORE.COM size/hash differs from the pinned upstream binary')
    source = (reference / manifest['source_path']).read_bytes()
    if digest(source) != manifest['source_sha256']:
        raise RuntimeError('MORE.ASM hash differs from the pinned source')
    # Verify the exact Git objects, not a dirty clone's HEAD or line-ending status.
    for path, current in ((manifest['binary_path'], binary),
                          (manifest['source_path'], source)):
        upstream = subprocess.run(['git', '-C', str(reference), 'show',
                                   manifest['revision'] + ':' + path],
                                  check=True, capture_output=True).stdout
        if current != upstream:
            raise RuntimeError('working copy differs from pinned Git object: ' + path)
    license_bytes = subprocess.run(['git', '-C', str(reference), 'show',
                                   manifest['revision'] + ':' + manifest['license_path']],
                                  check=True, capture_output=True).stdout
    return manifest, binary, license_bytes


def baseline(work, manifest, binary, license_bytes):
    executable = os.environ.get('DOSBOX') or shutil.which('dosbox')
    if not executable:
        raise RuntimeError('DOSBox unavailable; set DOSBOX (no automatic install)')
    version = subprocess.run([executable, '-version'], check=True,
                             capture_output=True, timeout=10)
    version_text = (version.stdout + version.stderr).decode(errors='replace')
    if not re.search(r'DOSBox version 0\.74-3(?:,|\s|$)', version_text):
        raise RuntimeError('reference DOSBox version differs from manifest')
    results = []
    for name, data in CASES.items():
        case = work / 'reference' / name
        case.mkdir(parents=True, exist_ok=True)
        (case / 'MORE.COM').write_bytes(binary)
        (case / 'LICENSE').write_bytes(license_bytes)
        (case / 'INPUT.TXT').write_bytes(data)
        # DOSBox may use either case on a host-directory mount.
        for filename in ('OUTPUT.TXT', 'output.txt'):
            (case / filename).unlink(missing_ok=True)
        config = case / 'dosbox.conf'
        config.write_text('[sdl]\nfullscreen=false\n[dosbox]\nmemsize=16\n'
                          '[midi]\nmpu401=none\nmididevice=none\n'
                          '[sblaster]\nsbtype=none\n[speaker]\npcspeaker=false\n')
        result = subprocess.run([executable, '-conf', str(config),
                                 '-c', 'mount f ' + str(case), '-c', 'f:',
                                 '-c', 'more < input.txt > output.txt', '-c', 'exit'],
                                capture_output=True, timeout=30,
                                env=dict(os.environ, SDL_VIDEODRIVER='dummy',
                                         SDL_AUDIODRIVER='dummy'))
        (case / 'dosbox.log').write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError('DOSBox process failed: ' + name)
        candidates = [p for p in case.iterdir() if p.name.upper() == 'OUTPUT.TXT']
        if len(candidates) != 1:
            raise RuntimeError('reference output missing/ambiguous: ' + name)
        output = candidates[0].read_bytes()
        expected = expected_output(data)
        if output != expected:
            raise RuntimeError('reference exact-byte output mismatch: ' + name)
        if (case / 'MORE.COM').read_bytes() != binary:
            raise RuntimeError('reference binary changed')
        results.append({'case': name, 'input_bytes': len(data),
                        'input_sha256': digest(data), 'output_bytes': len(output),
                        'output_sha256': digest(output), 'expected_hex': expected.hex(),
                        'exact_match': True})
        print('  Reference exact bytes: ' + name, flush=True)
    return {'emulator': manifest['reference_emulator'], 'cases': results,
            'pagination_tested': False, 'guest_exit_code_observed': False,
            'backend': 'DOSBox host-directory mount, not MS-DOS 2.0 boot disk'}


def build(work, binary, traced):
    build_dir = work / ('trace-build' if traced else 'plain-build')
    build_dir.mkdir(parents=True, exist_ok=True)
    arguments = ['make', '-j4', 'BUILD_DIR=' + str(build_dir),
                 'DOS_TRACE=' + ('1' if traced else '0'), 'all']
    # The modes always use disjoint object trees; flags are not dependencies.
    with (build_dir / 'probe-build.log').open('w') as log:
        subprocess.run(arguments, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=300)
        spec = importlib.util.spec_from_file_location('probe_fat', ROOT / 'tools/make-fat-image.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        files = [('HELLO.TXT', b'W5 FAT says hello\r\n'), ('EMPTY.TXT', b''),
                 ('BIG.BIN', bytes(i % 251 for i in range(4097))),
                 ('MORE.COM', binary), ('INPUT.TXT', CASES['short'])]
        for stem in ('files', 'keys', 'int21', 'psp'):
            files.append((stem.upper() + '.COM',
                          (build_dir / 'vmcorpus' / ('dos_' + stem + '.bin')).read_bytes()))
        (build_dir / 'dos.img').write_bytes(module.build(files))
        # Independent FAT implementation verifies what will be embedded.
        with tempfile.TemporaryDirectory(prefix='funyos-more-fat-', dir='/var/tmp') as temp:
            for name, expected in (('MORE.COM', binary), ('INPUT.TXT', CASES['short'])):
                extracted = Path(temp) / name
                subprocess.run(['mcopy', '-i', str(build_dir / 'dos.img'),
                                '::' + name, str(extracted)], check=True,
                               stdout=log, stderr=subprocess.STDOUT, timeout=10)
                if extracted.read_bytes() != expected:
                    raise RuntimeError('embedded FAT resource differs: ' + name)
        subprocess.run(arguments, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=300)
    return build_dir


def validate_trace(text):
    # Service observations, not direct unit calls; caller offsets are in the
    # pinned binary's actual INT sequence. Return IP is after the INT opcode.
    calls = [(int(n), ah.lower(), int(ip, 16)) for n, ah, ip in re.findall(
        r'\[DOS-TRACE\] #(\d+) AH=([0-9a-f]+) caller=[0-9a-f]+:([0-9a-f]+)', text)]
    expected = [(1, '30', 0x104), (2, '09', 0x11b), (3, '45', 0x121),
                (4, '3e', 0x127), (5, '45', 0x12e), (6, '3f', 0x13b)]
    if calls[:6] != expected:
        raise RuntimeError('unexpected first DOS call sequence')
    for number, ax in ((3, '4500'), (4, '0006'), (5, '4500'), (6, '0006')):
        if not re.search(rf'\[DOS-TRACE\] #{number} after ax={ax} .*cf=1', text):
            raise RuntimeError('missing expected failure return at call ' + str(number))
    if not re.search(r'\[DOS-TRACE\] #6 AH=3f .*bx=4500 .*bp=4500', text):
        raise RuntimeError('MORE did not consume failed duplicate AX as a handle')
    if '[DOS-TRACE] buffer-before 00 00 00 00 00 00' not in text:
        raise RuntimeError('unexpected initial input buffer')
    return {'first_missing_call': 'INT 21h AH=45h, BX=0000h',
            'int_offset': '011Fh', 'return_ip': '0121h',
            'actual_ax': '4500h', 'actual_cf': 1,
            'subsequent_read_handle': '4500h', 'read_error_ax': '0006h',
            'read_error_cf': 1, 'first_six_calls': [ah for _, ah, _ in calls[:6]]}


def validate_run(text, traced, marker):
    if 'RUN LIMIT' not in text or 'DOS program returned 1' not in text:
        raise RuntimeError('expected incompatible bounded run was not observed')
    # Check an executed echo line, never only keyboard echo or a substring.
    if not re.search(r'^' + re.escape(marker) + r'\r?$', text, re.MULTILINE):
        raise RuntimeError('Shell did not execute the post-VM command')
    for bad in ('PANIC', 'CPU EXCEPTION', 'INTERNAL ERROR'):
        if bad in text:
            raise RuntimeError('unexpected host/emulator failure: ' + bad)
    if not traced and '[DOS-TRACE]' in text:
        raise RuntimeError('trace unexpectedly present in the normal build')
    return validate_trace(text) if traced else None


def qemu_run(work, build_dir, firmware, traced):
    spec = importlib.util.spec_from_file_location('probe_qmp', ROOT / 'tools/run-dos-resources-test.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    case = work / 'qemu' / (('trace-' if traced else 'plain-') + firmware)
    case.mkdir(parents=True, exist_ok=True)
    serial = case / 'serial.log'
    serial.unlink(missing_ok=True)
    results = []
    with tempfile.TemporaryDirectory(prefix='funyos-more-', dir='/var/tmp') as temp:
        qmp = Path(temp) / 'qmp'
        command = ['qemu-system-x86_64', '-m', '512', '-cdrom', str(build_dir / 'funyos.iso'),
                   '-display', 'none', '-serial', 'file:' + str(serial),
                   '-qmp', 'unix:' + str(qmp) + ',server=on,wait=off', '-no-reboot']
        if firmware == 'uefi':
            variables = case / 'OVMF_VARS.fd'
            shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
            command += ['-M', 'q35', '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                        '-drive', 'if=pflash,format=raw,unit=1,file=' + str(variables)]
        else:
            command += ['-boot', 'd']
        command += ['-enable-kvm', '-cpu', 'host'] if os.access('/dev/kvm', os.W_OK) else ['-cpu', 'max']
        write_json(case / 'command.json', command)
        with (case / 'qemu.log').open('w') as diagnostics:
            process = subprocess.Popen(command, stdout=diagnostics, stderr=diagnostics)
            monitor = None
            try:
                helper.wait_for(qmp.exists, process, 'QMP socket')
                monitor = helper.Monitor(qmp)
                read = lambda: serial.read_text(errors='replace') if serial.exists() else ''
                helper.wait_for(lambda: 'F:\\>' in read(), process, 'Shell prompt')
                def type_line(line):
                    for char in line:
                        names = {' ': 'spc', '.': 'dot', '<': 'shift-comma', '>': 'shift-dot', '-': 'minus'}
                        monitor.key(names.get(char, char))
                    monitor.key('ret')
                for index, invocation in enumerate(('dos more.com', 'dos more.com < input.txt'), 1):
                    start = len(read())
                    type_line(invocation)
                    helper.wait_for(lambda: 'DOS program returned 1' in read()[start:], process, 'bounded MORE failure')
                    marker = 'aftermore' + str(index)
                    type_line('echo ' + marker)
                    helper.wait_for(lambda: re.search(r'^' + marker + r'\r?$', read()[start:], re.MULTILINE),
                                    process, 'post-VM Shell echo')
                    text = read()[start:]
                    observation = validate_run(text, traced, marker)
                    if traced:
                        tail = '' if index == 1 else '< input.txt'
                        if f'psp-tail length={len(tail)} text="{tail}"' not in text:
                            raise RuntimeError('PSP command tail differs from Shell invocation')
                        observation['psp_tail'] = tail
                    excerpt = case / ('run-' + str(index) + '.log')
                    excerpt.write_text(text)
                    results.append({'firmware': firmware, 'traced': traced,
                                    'invocation': invocation, 'application_status': 'incompatible',
                                    'stop': 'RUN LIMIT', 'runner_code': 1,
                                    'shell_recovered': True, 'trace': observation,
                                    'log': str(excerpt), 'log_sha256': digest(excerpt.read_bytes())})
                monitor.command('screendump', {'filename': str(case / 'shell-after.ppm')})
                monitor.command('quit')
                process.wait(timeout=10)
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
    print(f'  Investigation: {firmware}, trace={traced}, 2 incompatibilities confirmed; Shell recovered', flush=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=('all', 'reference', 'qemu'), default='all')
    parser.add_argument('--reference-dir', type=Path, default=ROOT / 'reference-msdos')
    parser.add_argument('--work-dir', type=Path, default=Path('/var/tmp/funyos-build/m5-app-probe'))
    parser.add_argument('--firmware', nargs='+', choices=('bios', 'uefi'), default=['bios', 'uefi'])
    args = parser.parse_args()
    work = args.work_dir.resolve()
    if work == ROOT.resolve() or ROOT.resolve() in work.parents:
        parser.error('keep artifacts outside the repository, preferably /var/tmp')
    manifest, binary, license_bytes = verify_source(args.reference_dir)
    work.mkdir(parents=True, exist_ok=True)
    (work / ('results-' + args.phase + '.json')).unlink(missing_ok=True)
    (work / 'MORE.COM').write_bytes(binary)
    (work / 'LICENSE').write_bytes(license_bytes)
    disassembly = subprocess.run(['ndisasm', '-b16', '-o0x100', str(work / 'MORE.COM')],
                                 check=True, capture_output=True).stdout
    (work / 'MORE.disasm').write_bytes(disassembly)
    report = {'schema': 1, 'application': manifest, 'phase': args.phase,
              'application_compatible': None, 'reference': None, 'funnyos_runs': []}
    if args.phase in ('all', 'reference'):
        report['reference'] = baseline(work, manifest, binary, license_bytes)
    if args.phase in ('all', 'qemu'):
        for traced in (True, False):
            build_dir = build(work, binary, traced)
            for firmware in args.firmware:
                report['funnyos_runs'] += qemu_run(work, build_dir, firmware, traced)
    if report['funnyos_runs']:
        report['application_compatible'] = False
    report['investigation_checks_completed'] = True
    write_json(work / ('results-' + args.phase + '.json'), report)
    status = 'MORE is NOT accepted as compatible' if report['funnyos_runs'] else 'reference only; FunnyOS compatibility not tested'
    print('Investigation completed. ' + status + '; see ' + str(work), flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print('Investigation failed: ' + str(exc), file=sys.stderr)
        raise SystemExit(1)
