import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('spatial_signatures', Path(__file__).resolve().parents[1] / 'scripts/check_spatial_signatures.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class SignatureChecks(unittest.TestCase):
    def test_elf_sections_and_wildcards(self):
        data = bytearray(256)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', data, 18, 183)
        struct.pack_into('<Q', data, 40, 64)
        struct.pack_into('<HH', data, 58, 64, 1)
        struct.pack_into('<QQQQ', data, 72, 4, 0x1000, 128, 32)
        data[132:136] = bytes.fromhex('aa 11 cc dd')
        sections = checker.executable_sections(data)
        self.assertEqual(sections, [(128, 32, 0x1000)])
        self.assertEqual(checker.find_matches(data, sections, 'AA ? CC DD'), [0x1004])
        self.assertEqual(checker.find_matches(data, sections, 'AA 12 CC DD'), [])
        self.assertEqual(checker.find_matches(data, sections, '11 CC DD'), [])  # unaligned

    def test_rejects_bad_elf_and_patterns(self):
        with self.assertRaises(ValueError):
            checker.executable_sections(bytes(100))
        with self.assertRaises(ValueError):
            checker.find_matches(b'1234', [(0, 4, 0)], '? ?')
        with self.assertRaises(ValueError):
            checker.find_matches(b'1234', [(0, 4, 0)], 'AA GG')


if __name__ == '__main__':
    unittest.main()
