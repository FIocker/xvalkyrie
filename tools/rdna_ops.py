#!/usr/bin/env python3
"""Writes the instruction table of the console shader translator.

Every opcode of every instruction format is encoded with plain operands and
handed to LLVM's disassembler for the console GPU's instruction set; what it
names goes into the table. Needs `llvm-mc` (LLVM 14 or later, with the AMDGPU
target).

    python tools/rdna_ops.py > src/runtime/modules/console_shader_rdna_ops.inc
    python tools/rdna_ops.py --llvm-mc "wsl llvm-mc-18" --check
"""
import argparse
import pathlib
import re
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLE = ROOT / "src/runtime/modules/console_shader_rdna_ops.inc"
CPU = "gfx1013"

V1, V2, V3, S1, S2, S3 = 0x101, 0x102, 0x103, 1, 2, 3
NULL_OFFSET = 0x7D << 25


def candidates(fmt, op):
    """Encodings to try for one opcode: operand choices differ by instruction."""
    if fmt == "sop2":
        yield [2 << 30 | op << 23 | S1 << 16 | S3 << 8 | S2]
        yield [2 << 30 | op << 23 | 4 << 16 | 6 << 8 | 2]
    elif fmt == "sopk":
        yield [0xB << 28 | op << 23 | S1 << 16 | 4]
        yield [0xB << 28 | op << 23 | S1 << 16 | 4, 0]
        yield [0xB << 28 | op << 23 | 0xF814, 0]
    elif fmt == "sop1":
        yield [0x17D << 23 | S1 << 16 | op << 8 | S2]
        yield [0x17D << 23 | 4 << 16 | op << 8 | 2]
        yield [0x17D << 23 | op << 8 | S2]
        yield [0x17D << 23 | 4 << 16 | op << 8]
    elif fmt == "sopc":
        yield [0x17E << 23 | op << 16 | S2 << 8 | S1]
        yield [0x17E << 23 | op << 16 | 4 << 8 | 2]
    elif fmt == "sopp":
        yield [0x17F << 23 | op << 16]
        yield [0x17F << 23 | op << 16 | 4]
    elif fmt == "smem":
        for base in (1, 2):
            yield [0x3D << 26 | op << 18 | 4 << 6 | base, NULL_OFFSET]
            yield [0x3D << 26 | op << 18 | base, NULL_OFFSET]
        yield [0x3D << 26 | op << 18, NULL_OFFSET]
    elif fmt == "vop2":
        yield [op << 25 | 1 << 17 | 3 << 9 | V2]
        yield [op << 25 | 1 << 17 | 3 << 9 | V2, 0x3F800000]
    elif fmt == "vop1":
        yield [0x3F << 25 | 1 << 17 | op << 9 | V2]
        yield [0x3F << 25 | 1 << 17 | op << 9 | S2]
        yield [0x3F << 25 | op << 9]
    elif fmt == "vopc":
        yield [0x3E << 25 | op << 17 | 2 << 9 | V1]
    elif fmt == "vop3":
        head = 0x35 << 26 | op << 16
        for dst in (1, 4):
            yield [head | dst, V1 | V2 << 9 | V3 << 18]
            yield [head | dst, V1 | V2 << 9 | 6 << 18]
            yield [head | dst | 6 << 8, V1 | V2 << 9]
            yield [head | dst, V1 | V2 << 9]
            yield [head | dst, V1 | 2 << 9]
            yield [head | dst, V1]
            yield [head | dst, 2]
        yield [head | 126, V1 | V2 << 9]
        yield [head, 0]
    elif fmt == "vop3p":
        yield [0x33 << 26 | op << 16 | 1, V1 | V2 << 9 | V3 << 18]
        yield [0x33 << 26 | op << 16 | 1, V1 | V2 << 9]
        yield [0x33 << 26 | op << 16 | 1 | 0x40 << 8, V1 | V2 << 9 | 3 << 27]
    elif fmt == "vintrp":
        yield [0x32 << 26 | 1 << 18 | op << 16 | 2]
    elif fmt == "ds":
        yield [0x36 << 26 | op << 18, 1 | 2 << 8 | 3 << 16 | 4 << 24]
        yield [0x36 << 26 | op << 18, 1 | 4 << 24]
        yield [0x36 << 26 | op << 18, 1 | 2 << 8]
        yield [0x36 << 26 | op << 18, 1 | 2 << 8 | 3 << 16]
        yield [0x36 << 26 | op << 18, 0]
    elif fmt == "flat":
        for segment in (2, 0, 1):
            head = 0x37 << 26 | op << 18 | segment << 14
            yield [head, 2 | 0x7D << 16 | 4 << 24]
            yield [head, 2 | 4 << 8 | 0x7D << 16]
            yield [head, 2 | 4 << 8 | 0x7D << 16 | 4 << 24]
    elif fmt == "mubuf":
        yield [0x38 << 26 | op << 18, 1 | 4 << 8 | 2 << 16 | 0x80 << 24]
        yield [0x38 << 26 | op << 18, 0]
    elif fmt == "mtbuf":
        yield [0x3A << 26 | (op & 7) << 16 | 1 << 13, 1 | 4 << 8 | 2 << 16 | (op >> 3) << 21 | 0x80 << 24]
    elif fmt == "mimg":
        head = 0x3C << 26 | (op & 0x7F) << 18 | (op >> 7)
        for dmask in (1, 0xF, 0):
            for dim in (1, 0):
                yield [head | dmask << 8 | dim << 3, 2 | 4 << 8 | 1 << 16 | 2 << 21]
                yield [head | dmask << 8 | dim << 3, 2 | 4 << 8 | 1 << 16]
    elif fmt == "exp":
        yield [0x3E << 26 | 0xF, 0]


RANGES = {
    "sop2": 0x60, "sopk": 0x20, "sop1": 0x100, "sopc": 0x80, "sopp": 0x80, "smem": 0x100,
    "vop2": 0x3E, "vop1": 0x100, "vopc": 0x100, "vop3": 0x400, "vop3p": 0x80, "vintrp": 4,
    "ds": 0x100, "flat": 0x80, "mubuf": 0x80, "mtbuf": 0x10, "mimg": 0x100, "exp": 1,
}
ORDER = list(RANGES)


MARK = 0xBF8007B5  # s_nop with an operand nothing else here has


def disassemble(llvm_mc, encodings):
    """The mnemonic of each encoding, or None for one the disassembler turns down."""
    # The disassembler reads one stream, so an encoding that is short of a word would take
    # the next one's: three marks follow each, and what lies between two runs of marks
    # counts only if it is that encoding and nothing else.
    stream = [word for words in encodings for word in (*words, MARK, MARK, MARK)]
    text = "".join(" ".join(f"0x{byte:02x}" for byte in word.to_bytes(4, "little")) + chr(10)
                   for word in stream)
    run = subprocess.run(
        shlex.split(llvm_mc) + ["--disassemble", "-triple=amdgcn", f"-mcpu={CPU}", "--show-encoding"],
        input=text, capture_output=True, text=True)
    mark = ",".join(f"0x{b:02x}" for b in MARK.to_bytes(4, "little"))
    # a run of marks closes one encoding for every three in it, give or take the one an
    # instruction before it may have taken
    groups, group, marks = [], [], 0
    lines = [re.match(r"\s*(\S+).*encoding: \[(.*)\]", line) for line in run.stdout.splitlines()]
    for match in [found for found in lines if found] + [None]:
        if match and match.group(2) == mark:
            marks += 1
            continue
        if marks:
            groups.append(group)
            groups += [[] for _ in range((marks + 1) // 3 - 1)]
            group, marks = [], 0
        if match:
            group.append((match.group(1), match.group(2)))
    names = []
    for words, group in zip(encodings, groups):
        whole = ",".join(f"0x{b:02x}" for word in words for b in word.to_bytes(4, "little"))
        names.append(group[0][0] if len(group) == 1 and group[0][1] == whole else None)
    return names + [None] * (len(encodings) - len(names))


def build(llvm_mc):
    table = []
    for fmt in ORDER:
        tries, owners = [], []
        for op in range(RANGES[fmt]):
            for words in candidates(fmt, op):
                tries.append(words)
                owners.append(op)
        names = {}
        for op, name in zip(owners, disassemble(llvm_mc, tries)):
            if name and op not in names:
                names[op] = re.sub(r"_(e32|e64)$", "", name)
        table += [(fmt, op, names[op]) for op in sorted(names)]
    return table


def render(table):
    return "".join(f'    {{F::{fmt}, 0x{op:x}, "{name}"}},\n' for fmt, op, name in table)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--llvm-mc", default="llvm-mc", help="the disassembler's command line")
    parser.add_argument("--check", action="store_true",
                        help="compare with the table in the tree instead of printing")
    arguments = parser.parse_args()
    text = render(build(arguments.llvm_mc))
    if not arguments.check:
        sys.stdout.write(text)
        return 0
    have = set(TABLE.read_text().splitlines())
    made = set(text.splitlines())
    for line in sorted(have - made):
        print("only in the tree:", line.strip())
    for line in sorted(made - have):
        print("not in the tree: ", line.strip())
    return 1 if have != made else 0


if __name__ == "__main__":
    sys.exit(main())
