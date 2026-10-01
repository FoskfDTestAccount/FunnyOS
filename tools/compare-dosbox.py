#!/usr/bin/env python3
"""Run the SAME FILES.COM in DOSBox and compare stable DOS observables.
Optional external reference: DOSBOX=/path/to/dosbox python3 tools/compare-dosbox.py.
No emulator is installed by this script. Normal make check stays self-contained.
"""
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(os.environ.get('FUNYOS_BUILD_DIR', '/var/tmp/funyos-build'))


def main():
    executable = os.environ.get('DOSBOX') or shutil.which('dosbox')
    if not executable:
        raise SystemExit('DOSBox unavailable: install separately or set DOSBOX=/absolute/path')
    image = BUILD / 'vmcorpus/dos_files.bin'
    if not image.exists():
        raise SystemExit('build FILES.COM first: make')
    work = BUILD / 'dosbox-compare'
    work.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(image, work / 'FILES.COM')
    (work / 'HELLO.TXT').write_bytes(b'W5 FAT says hello\r\n')
    for name in ('OUTPUT.TXT', 'RESULT.TXT', 'output.txt', 'result.txt'):
        (work / name).unlink(missing_ok=True)
    config = work / 'dosbox.conf'
    config.write_text('[sdl]\nfullscreen=false\n[dosbox]\nmemsize=16\n'
                      '[midi]\nmpu401=none\nmididevice=none\n'
                      '[sblaster]\nsbtype=none\n[speaker]\npcspeaker=false\n')
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    result = subprocess.run([executable, '-conf', str(config),
                             '-c', 'mount f ' + str(work), '-c', 'f:',
                             '-c', 'files > output.txt', '-c', 'exit'],
                            capture_output=True, timeout=30, env=env)
    (work / 'dosbox.log').write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit('DOSBox failed; see ' + str(work / 'dosbox.log'))
    output = next((p for p in work.iterdir() if p.name.upper() == 'OUTPUT.TXT'), None)
    expected = b'W5 FAT says hello\r\nW5 files: PASS\r\n'
    if output is None or output.read_bytes() != expected:
        raise SystemExit('DOSBox output differs from the independent expected bytes')
    if any(p.name.upper() == 'RESULT.TXT' for p in work.iterdir()):
        raise SystemExit('DOSBox did not perform the expected deletion')
    # QEMU independently executed exactly this binary via the FAT mount.
    log = BUILD / 'dos-resources-test/files-bios/serial.log'
    if not log.exists():
        raise SystemExit('run make test-dos-resources first for the FunnyOS comparison')
    text = log.read_text(errors='replace')
    for value in ('W5 FAT says hello', 'W5 files: PASS', 'code 0', 'W5 resource acceptance: PASS'):
        if value not in text:
            raise SystemExit('FunnyOS lacks the same observable: ' + value)
    if (work / 'FILES.COM').read_bytes() != image.read_bytes():
        raise SystemExit('reference run used different .COM bytes')
    print('====> DOSBox comparison PASSED: same .COM, exact output, internal register/DTA checks, deletion')
    print('Scope: mounted host-directory DOSBox backend versus FunnyOS FAT12; not keyboard timing or exit-code ABI.')


if __name__ == '__main__':
    main()
