#!/usr/bin/env python3
"""Focused PE fixtures for the Windows ARM64 compiled-image return guard."""

import importlib.util
from pathlib import Path
import struct
import sys
import unittest


SCRIPT = Path(__file__).with_name("validate-arm64-stack-cookie.py")
SPEC = importlib.util.spec_from_file_location("arm64_stack_cookie", SCRIPT)
GUARD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GUARD
SPEC.loader.exec_module(GUARD)


HELPER = (0x90000011, 0xF94007F0, 0xF9404231, 0xCB3063F0,
          0xEB11021F, 0x54000081, 0x910043FF, GUARD.RET_LR)


def pe_image(code, *, machine=GUARD.ARM64, executable=True, rva=0x1000):
    data = bytearray(0x200 + len(code))
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, machine, 1)
    struct.pack_into("<H", data, 0x94, 0xF0)
    struct.pack_into("<H", data, 0x98, 0x20B)
    struct.pack_into("<I", data, 0x98 + 56, 0x4000)
    section = 0x188
    data[section:section + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", data, section + 8, len(code), rva, len(code), 0x200)
    struct.pack_into("<I", data, section + 36, GUARD.EXECUTABLE if executable else 0x40000000)
    data[0x200:] = code
    return bytes(data)


def fixture(after_call, *, helper=HELPER, helper_first=False):
    if helper_first:
        words = list(helper) + [GUARD.NOP] * 8
        call_offset = len(words) * 4
        words.append(0x94000000 | ((-call_offset // 4) & 0x03FFFFFF))
        words.extend(after_call)
    else:
        words = [0x94000010] + list(after_call)
        words += [GUARD.NOP] * (16 - len(words))
        words.extend(helper)
    return pe_image(struct.pack("<" + "I" * len(words), *words))


class Arm64StackCookieTests(unittest.TestCase):
    def test_observed_cookie_call_stack_adjust_and_return_is_rejected(self):
        result = GUARD.scan_image(fixture([0x910183FF, GUARD.RET_LR]))
        self.assertEqual(result.helper_rvas, (0x1040,))
        self.assertEqual(result.call_count, 1)
        self.assertEqual(result.unsafe_call_rvas, (0x1000,))

    def test_cookie_call_followed_by_lr_restore_is_valid(self):
        for restore in (0xA8C17BFD, 0xF9402BFE):  # LDP fp,lr,[sp],#16; LDR lr,[sp,#80]
            with self.subTest(restore=hex(restore)):
                result = GUARD.scan_image(fixture([restore, 0x910183FF, GUARD.RET_LR]))
                self.assertEqual(result.call_count, 1)
                self.assertEqual(result.unsafe_call_rvas, ())

    def test_nops_multiple_adjustments_and_direct_returns_are_rejected(self):
        for chain in ([GUARD.RET_LR],
                      [GUARD.NOP, 0x910183FF, 0xD10043FF, GUARD.NOP, GUARD.RET_LR]):
            with self.subTest(chain=chain):
                self.assertEqual(GUARD.scan_image(fixture(chain)).unsafe_call_rvas, (0x1000,))

    def test_other_calls_branches_and_return_registers_are_not_assumed_invalid(self):
        for instruction in (0x94000001, 0x14000001, 0xD65F0220, 0xAA0F03FE):
            with self.subTest(instruction=hex(instruction)):
                result = GUARD.scan_image(fixture([instruction, GUARD.RET_LR]))
                self.assertEqual(result.unsafe_call_rvas, ())

    def test_backward_call_and_cookie_address_relocations_are_recognized(self):
        helper = list(HELPER)
        helper[0] = 0xF0018A11
        helper[2] = 0xF9464231
        helper[5] = 0x54FFFF81
        result = GUARD.scan_image(fixture([0x910183FF, GUARD.RET_LR],
                                         helper=helper, helper_first=True))
        self.assertEqual(result.helper_rvas, (0x1000,))
        self.assertEqual(result.unsafe_call_rvas, (0x1040,))

    def test_calls_across_executable_sections_are_checked(self):
        caller = struct.pack("<III", 0x94000400, 0x910183FF, GUARD.RET_LR)
        helper = struct.pack("<" + "I" * len(HELPER), *HELPER)
        image = bytearray(pe_image(caller + helper))
        struct.pack_into("<H", image, 0x86, 2)
        struct.pack_into("<IIII", image, 0x188 + 8, len(caller), 0x1000, len(caller), 0x200)
        second = 0x188 + 40
        image[second:second + 8] = b".text2\0\0"
        struct.pack_into("<IIII", image, second + 8, len(helper), 0x2000,
                         len(helper), 0x200 + len(caller))
        struct.pack_into("<I", image, second + 36, GUARD.EXECUTABLE)
        result = GUARD.scan_image(bytes(image))
        self.assertEqual(result.helper_rvas, (0x2000,))
        self.assertEqual(result.unsafe_call_rvas, (0x1000,))
        struct.pack_into("<I", image, second + 36, 0x40000000)
        with self.assertRaisesRegex(GUARD.ImageValidationError, "No recognized"):
            GUARD.scan_image(bytes(image))
        struct.pack_into("<I", image, second + 12, 0x1000)
        with self.assertRaisesRegex(GUARD.ImageValidationError, "overlap"):
            GUARD.scan_image(bytes(image))

    def test_missing_or_changed_helper_fails_closed_unless_explicitly_allowed(self):
        altered = list(HELPER)
        altered[1] = 0xF9400BF0  # Different cookie stack slot is an unknown implementation.
        image = fixture([GUARD.RET_LR], helper=altered)
        with self.assertRaisesRegex(GUARD.ImageValidationError, "No recognized"):
            GUARD.scan_image(image)
        self.assertEqual(GUARD.scan_image(image, allow_missing_helper=True).helper_rvas, ())

    def test_non_native_arm64_or_non_executable_sections_are_rejected(self):
        for machine in (0x8664, 0xA641):
            with self.subTest(machine=machine), self.assertRaisesRegex(
                    GUARD.ImageValidationError, "native ARM64"):
                GUARD.scan_image(pe_image(struct.pack("<I", GUARD.RET_LR), machine=machine))
        with self.assertRaisesRegex(GUARD.ImageValidationError, "no executable section"):
            GUARD.scan_image(pe_image(struct.pack("<I", GUARD.RET_LR), executable=False))

    def test_pe_bounds_and_headers_are_checked(self):
        image = bytearray(fixture([GUARD.RET_LR]))
        mutations = {
            "DOS": lambda value: value.__setitem__(slice(0, 2), b"ZZ"),
            "PE offset": lambda value: struct.pack_into("<I", value, 0x3C, 0xFFFFFFFF),
            "PE signature": lambda value: value.__setitem__(slice(0x80, 0x84), b"ABCD"),
            "section table": lambda value: struct.pack_into("<H", value, 0x86, 0xFFFF),
            "optional header": lambda value: struct.pack_into("<H", value, 0x94, 2),
            "section data": lambda value: struct.pack_into("<I", value, 0x188 + 20, 0xFFFFFFF0),
            "image size": lambda value: struct.pack_into("<I", value, 0x98 + 56, 0x1000),
            "alignment": lambda value: struct.pack_into("<I", value, 0x188 + 12, 0x1001),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label), self.assertRaises(GUARD.ImageValidationError):
                broken = bytearray(image)
                mutate(broken)
                GUARD.scan_image(bytes(broken))
        for length in (0, 63, 0x80, 0x100, len(image) - 1):
            with self.subTest(length=length), self.assertRaises(GUARD.ImageValidationError):
                GUARD.scan_image(bytes(image[:length]))


if __name__ == "__main__":
    unittest.main()
