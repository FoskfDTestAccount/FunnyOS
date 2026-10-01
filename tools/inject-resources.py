#!/usr/bin/env python3
"""Inject W5/W6 defects into a private native copy; require runtime failures
with specific diagnoses (compiler errors, timeouts and unmatched patches
are not catches). Builds the four DOS resource suites from scratch.
"""
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
ROOT = pathlib.Path(__file__).resolve().parent.parent
EXPECTED = {
    'W5 mount ignores boot signature': 'FAIL  signature',
    'W5 read never shortens at EOF': 'FAIL  short read at EOF',
    'W5 close leaves descriptor live': 'FAIL  double close',
    'W5 seek loses signed displacement': 'FAIL  position:',
    'W5 writes only the first FAT': 'FAIL  copies kept in sync',
    'W5 truncate retains old size': 'FAIL  metadata size',
    'W5 allows writing a readonly file': 'FAIL  readonly write open denied',
    'W5 search restarts on FindNext': 'FAIL  FindNext end CF',
    'W5 DTA size is one byte late': 'FAIL  DTA size at 1Ah',
    'W5 read ignores invalid guest buffers': 'FAIL  invalid buffer CF',
    'W5 file read answer always zero': 'FAIL  read AX short count',
    'W5 seek swaps AX and DX': 'FAIL  position low AX',
    'W6 availability says zero for a waiting key': 'FAIL  available status FFh',
    'W6 direct empty input clears ZF': 'FAIL  empty direct input ZF',
    'W6 echo call never echoes': 'FAIL  01h echoes',
    'W6 no echo call echoes': 'FAIL  07h does not echo',
    'W6 extended key loses its scan byte': 'FAIL  extended second byte scan',
    'W6 line count includes carriage return': 'FAIL  line count excludes CR',
    'W6 line buffer does not reserve carriage return': 'FAIL  capacity reserves CR',
    'W6 backspace never removes a character': 'FAIL  line count excludes CR',
    'W6 IRET forgets the outer interrupt frame': 'FAIL  no dropped flags',
}


def load():
    spec = importlib.util.spec_from_file_location('mutations', ROOT / 'tools/verify-mutations.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.M56_MUTATIONS


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    mutations = [m for m in load() if not only or only in m[0]]
    if not mutations:
        raise RuntimeError('no mutation matched')
    with tempfile.TemporaryDirectory(prefix='funyos-w56-inject-', dir='/var/tmp') as temp:
        tree = pathlib.Path(temp)
        for part in ('dos', 'libk', 'tools'):
            shutil.copytree(ROOT / part, tree / part)
        command = ['make', '-B', '-j4', 'BUILD=' + str(tree / 'build'),
                   'test-fat', 'test-files', 'test-input', 'test-int21']
        def run():
            result = subprocess.run(command, cwd=tree / 'dos', capture_output=True,
                                    text=True, errors='replace', timeout=120)
            return result, result.stdout + result.stderr
        baseline, output = run()
        if baseline.returncode:
            print(output)
            raise RuntimeError('pristine suites are not green')
        bad = 0
        for name, path, old, new in mutations:
            target = tree / 'dos' / path
            original = target.read_text()
            status = 'PATCH'
            diagnosis = EXPECTED[name]
            if original.count(old) == 1:
                try:
                    target.write_text(original.replace(old, new))
                    result, output = run()
                    if not result.returncode:
                        status = 'GREEN'
                    elif ': error:' in output or ': fatal error:' in output:
                        status = 'BUILD'
                    elif diagnosis not in output:
                        status = 'WRONG'
                    else:
                        status = 'caught'
                except subprocess.TimeoutExpired:
                    status = 'HANG'
                finally:
                    target.write_text(original)
            if status != 'caught':
                bad += 1
                if status in ('BUILD', 'WRONG'):
                    for line in output.splitlines():
                        if 'error:' in line or 'FAIL ' in line:
                            print('    ' + line, flush=True)
            print(f'{name:54} {status:8} {diagnosis}', flush=True)
        print(f'{len(mutations)-bad} of {len(mutations)} caught with the expected runtime diagnosis')
        return bool(bad)


if __name__ == '__main__':
    sys.exit(main())
