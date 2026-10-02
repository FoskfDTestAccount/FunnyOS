"""Tests for the investigation oracle, not tests of MORE or DOS services."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('app_probe', ROOT / 'tools/probe-dos-app.py')
PROBE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROBE)

# Synthetic service observations make each rejection independently testable.
TRACE = '''[DOS-TRACE] #1 AH=30 caller=1000:0104
[DOS-TRACE] #2 AH=09 caller=1000:011b
[DOS-TRACE] #3 AH=45 caller=1000:0121
[DOS-TRACE] #3 after ax=4500 bx=0000 cf=1
[DOS-TRACE] #4 AH=3e caller=1000:0127
[DOS-TRACE] #4 after ax=0006 bx=0000 cf=1
[DOS-TRACE] #5 AH=45 caller=1000:012e
[DOS-TRACE] #5 after ax=4500 bx=0002 cf=1
[DOS-TRACE] #6 AH=3f caller=1000:013b ax=3f00 bx=4500 cx=1000 bp=4500
[DOS-TRACE] buffer-before 00 00 00 00 00 00
[DOS-TRACE] #6 after ax=0006 bx=4500 cf=1
'''
PLAIN = 'result : RUN LIMIT\nDOS program returned 1\nF:\\> echo aftermore1\naftermore1\n'


class ProbeTests(unittest.TestCase):
    def test_manifest_pin(self):
        manifest = PROBE.read_manifest()
        self.assertEqual(manifest['size'], 4364)
        self.assertEqual(manifest['sha256'],
                         '56830889fef22d56ef5938df0394bf30a8416cc06225b8dc35adc74bb13416b7')
        self.assertEqual(manifest['binary_int21_dependencies'],
                         ['30', '09', '45', '3E', '3F', '02', '0C'])

    def test_corrupt_binary_rejected_before_running_tools(self):
        manifest = PROBE.read_manifest()
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / manifest['binary_path']
            path.parent.mkdir(parents=True)
            path.write_bytes(b'not MORE')
            with self.assertRaisesRegex(RuntimeError, 'size/hash'):
                PROBE.verify_source(Path(temp))

    def test_empty_output(self):
        self.assertEqual(PROBE.expected_output(b''), b'\r\n')

    def test_exact_control_bytes(self):
        self.assertEqual(PROBE.expected_output(b'x\ty\bZ\r\n'), b'\r\nx\ty\bZ\r\n')

    def test_ctrlz_truncates(self):
        self.assertEqual(PROBE.expected_output(b'yes\x1ano'), b'\r\nyes')

    def test_ctrlz_at_start(self):
        self.assertEqual(PROBE.expected_output(b'\x1ano'), b'\r\n')

    def test_valid_trace(self):
        result = PROBE.validate_trace(TRACE)
        self.assertEqual(result['actual_ax'], '4500h')
        self.assertEqual(result['first_six_calls'], ['30', '09', '45', '3e', '45', '3f'])

    def test_wrong_call_sequence_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'sequence'):
            PROBE.validate_trace(TRACE.replace('AH=45', 'AH=46', 1))

    def test_wrong_return_ip_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'sequence'):
            PROBE.validate_trace(TRACE.replace('1000:0121', '1000:0120'))

    def test_missing_cf_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'failure return'):
            PROBE.validate_trace(TRACE.replace('cf=1', 'cf=0', 1))

    def test_wrong_read_handle_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'as a handle'):
            PROBE.validate_trace(TRACE.replace('bx=4500 cx=1000', 'bx=0005 cx=1000'))

    def test_missing_memory_observation_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'input buffer'):
            PROBE.validate_trace(TRACE.replace('buffer-before 00', 'buffer-before ff'))

    def test_valid_plain_observation(self):
        self.assertIsNone(PROBE.validate_run(PLAIN, False, 'aftermore1'))

    def test_traced_observation(self):
        self.assertEqual(PROBE.validate_run(TRACE + PLAIN, True, 'aftermore1')['actual_cf'], 1)

    def test_external_timeout_is_not_guest_run_limit(self):
        with self.assertRaisesRegex(RuntimeError, 'bounded run'):
            PROBE.validate_run(PLAIN.replace('RUN LIMIT', 'timeout'), False, 'aftermore1')

    def test_keyboard_echo_alone_is_not_recovery(self):
        with self.assertRaisesRegex(RuntimeError, 'execute'):
            PROBE.validate_run(PLAIN.replace('\naftermore1\n', '\n'), False, 'aftermore1')

    def test_panic_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'host/emulator'):
            PROBE.validate_run(PLAIN + 'PANIC\n', False, 'aftermore1')

    def test_trace_in_plain_build_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'normal build'):
            PROBE.validate_run(TRACE + PLAIN, False, 'aftermore1')


if __name__ == '__main__':
    unittest.main()
