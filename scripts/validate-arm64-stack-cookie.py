#!/usr/bin/env python3
"""Reject the observed Windows ARM64 stack-cookie return-address miscompilation.

This checks the final PE image, where LTCG can change code from both application
objects and static dependencies. It recognizes the MSVC stack-cookie pop helper
and rejects direct calls followed only by stack adjustments/NOPs and RET: BL
replaces LR, and that helper does not restore the caller's LR.

This is a guard for the demonstrated instruction sequence, not a general ARM64
control-flow or stack-unwind verifier.
"""

import argparse
from array import array
from dataclasses import dataclass
from pathlib import Path
import struct
import sys


ARM64 = 0xAA64
EXECUTABLE = 0x20000000
RET_LR = 0xD65F03C0
NOP = 0xD503201F

# Address materialization and the failure-branch displacement vary per link.
# The register choices, cookie stack slot and successful return path identify
# LIBCMT's __security_pop_cookie implementation used by the affected toolchain.
POP_COOKIE = (
    (0x9F00001F, 0x90000011),  # ADRP x17, cookie page
    (0xFFFFFFFF, 0xF94007F0),  # LDR x16, [sp, #8]
    (0xFFC003FF, 0xF9400231),  # LDR x17, [x17, cookie offset]
    (0xFFFFFFFF, 0xCB3063F0),  # SUB x16, sp, x16
    (0xFFFFFFFF, 0xEB11021F),  # CMP x16, x17
    (0xFF00001F, 0x54000001),  # B.NE failure
    (0xFFFFFFFF, 0x910043FF),  # ADD sp, sp, #16
    (0xFFFFFFFF, RET_LR),
)


class ImageValidationError(ValueError):
    pass


@dataclass(frozen=True)
class CodeSection:
    name: str
    rva: int
    code: bytes


@dataclass(frozen=True)
class ScanResult:
    helper_rvas: tuple[int, ...]
    call_count: int
    unsafe_call_rvas: tuple[int, ...]


def executable_sections(data: bytes) -> tuple[CodeSection, ...]:
    """Read native ARM64 PE32+ executable sections with checked file bounds."""
    def require_range(offset, length, label):
        if offset < 0 or length < 0 or offset + length > len(data):
            raise ImageValidationError(f"Truncated or invalid {label}")

    require_range(0, 64, "DOS header")
    if data[:2] != b"MZ":
        raise ImageValidationError("Expected a PE executable with an MZ header")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    require_range(pe, 24, "PE header")
    if data[pe:pe + 4] != b"PE\0\0":
        raise ImageValidationError("Invalid PE signature")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    if machine != ARM64:
        raise ImageValidationError(f"Expected native ARM64 machine 0xAA64, found 0x{machine:04X}")
    if not count:
        raise ImageValidationError("PE image has no sections")
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    require_range(optional, optional_size, "optional header")
    if optional_size < 112 or struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ImageValidationError("Expected a complete PE32+ optional header")
    image_size = struct.unpack_from("<I", data, optional + 56)[0]
    if not image_size:
        raise ImageValidationError("PE image has no virtual image size")
    table = optional + optional_size
    require_range(table, count * 40, "section table")
    sections = []
    virtual_ranges = []
    for index in range(count):
        header = table + index * 40
        name = data[header:header + 8].rstrip(b"\0").decode("ascii", errors="replace")
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", data, header + 8)
        characteristics = struct.unpack_from("<I", data, header + 36)[0]
        require_range(raw_offset, raw_size, f"section {name} data")
        extent = max(virtual_size, raw_size)
        if rva + extent > image_size:
            raise ImageValidationError(f"Section {name} extends beyond the virtual image")
        if extent:
            if any(rva < end and start < rva + extent for start, end in virtual_ranges):
                raise ImageValidationError("PE sections overlap in the virtual image")
            virtual_ranges.append((rva, rva + extent))
        if characteristics & EXECUTABLE and raw_size:
            if rva % 4:
                raise ImageValidationError(f"Executable section {name} is not ARM64 instruction aligned")
            size = min(raw_size, virtual_size) if virtual_size else raw_size
            sections.append(CodeSection(name, rva, data[raw_offset:raw_offset + size]))
    if not sections:
        raise ImageValidationError("PE image has no executable section data")
    return tuple(sections)


def scan_image(data: bytes, *, allow_missing_helper=False) -> ScanResult:
    sections = executable_sections(data)
    decoded = []
    for section in sections:
        words = array("I")
        words.frombytes(section.code[:len(section.code) // 4 * 4])
        if sys.byteorder != "little":
            words.byteswap()
        decoded.append((section, words))
    helpers = set()
    for section, words in decoded:
        for index in range(len(words) - len(POP_COOKIE) + 1):
            if words[index] & POP_COOKIE[0][0] != POP_COOKIE[0][1]:
                continue
            if all(words[index + offset] & mask == expected
                   for offset, (mask, expected) in enumerate(POP_COOKIE)):
                helpers.add(section.rva + index * 4)
    if not helpers and not allow_missing_helper:
        raise ImageValidationError(
            "No recognized __security_pop_cookie implementation; the ARM64 guard could not verify this image")

    calls = 0
    unsafe = []
    for section, words in decoded:
        for index, instruction in enumerate(words):
            if instruction & 0xFC000000 != 0x94000000:  # BL, signed imm26 << 2
                continue
            displacement = instruction & 0x03FFFFFF
            if displacement & 0x02000000:
                displacement -= 0x04000000
            call_rva = section.rva + index * 4
            if call_rva + displacement * 4 not in helpers:
                continue
            calls += 1
            # Only inspect a terminal straight-line chain whose instructions
            # cannot restore LR. Stop at every other instruction, including LR
            # loads, branches, other calls and returns through another register.
            for following_index in range(index + 1, len(words)):
                following = words[following_index]
                if following == RET_LR:
                    unsafe.append(call_rva)
                    break
                if following != NOP and following & 0xFF8003FF not in (0x910003FF, 0xD10003FF):
                    break
    return ScanResult(tuple(sorted(helpers)), calls, tuple(unsafe))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Native Windows ARM64 PE executable to validate")
    parser.add_argument("--allow-missing-helper", action="store_true",
                        help="Allow unrelated images that do not contain the recognized CRT helper")
    arguments = parser.parse_args(argv)
    try:
        result = scan_image(arguments.image.read_bytes(),
                            allow_missing_helper=arguments.allow_missing_helper)
    except (OSError, ImageValidationError) as error:
        print(f"ARM64 stack-cookie validation failed: {error}", file=sys.stderr)
        return 1
    if result.unsafe_call_rvas:
        locations = ", ".join(f"0x{rva:X}" for rva in result.unsafe_call_rvas)
        print(f"ARM64 stack-cookie validation failed: {len(result.unsafe_call_rvas)} calls return "
              f"without restoring LR (RVAs: {locations})", file=sys.stderr)
        return 1
    print(f"ARM64 stack-cookie validation passed: {result.call_count} recognized helper calls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
