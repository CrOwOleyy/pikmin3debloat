#!/usr/bin/env python3
"""Scanne le .text d'un ELF PPC (Wii U) et recense les instructions paired-singles Gekko.

But : borner le travail de recompilation. Les paired-singles sont :
  - primaire 4  : ps_* (arithmétique SIMD 2xf32), sous-opcode en bits 1-10 (X) ou 1-5 (A)
  - primaire 56 : psq_l   | 57 : psq_lu   | 60 : psq_st   | 61 : psq_stu  (load/store quantifiés)
(En PPC classique, 4 = inutilisé/AltiVec et 56/57/60/61 = inutilisés → repris par le Gekko.)

Usage: python3 scan_paired_singles.py <carrot.elf>
"""
import struct
import sys
from collections import Counter


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def be16(b, off):
    return struct.unpack_from(">H", b, off)[0]


# Sous-opcodes X-form (bits 1-10) du primaire 4
XO10 = {
    0: "ps_cmpu0", 32: "ps_cmpo0", 64: "ps_cmpu1", 96: "ps_cmpo1",
    40: "ps_neg", 72: "ps_mr", 136: "ps_nabs", 264: "ps_abs",
    528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10", 624: "ps_merge11",
    1014: "dcbz_l",
}
# Sous-opcodes A-form (bits 1-5) du primaire 4
XO5 = {
    10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1",
    14: "ps_madds0", 15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add",
    23: "ps_sel", 24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte",
    28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd",
}
PRIMARY = {56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu"}


def main():
    data = open(sys.argv[1], "rb").read()
    e_shoff = be32(data, 0x20)
    e_shentsize = be16(data, 0x2E)
    e_shnum = be16(data, 0x30)
    e_shstrndx = be16(data, 0x32)
    shstr_off = be32(data, e_shoff + e_shstrndx * e_shentsize + 16)

    def name(off):
        end = data.index(b"\x00", shstr_off + off)
        return data[shstr_off + off:end].decode("latin-1")

    text_ranges = []  # (name, addr, offset, size)
    for i in range(e_shnum):
        b = e_shoff + i * e_shentsize
        nm = name(be32(data, b))
        sh_flags = be32(data, b + 8)
        sh_addr = be32(data, b + 12)
        sh_off = be32(data, b + 16)
        sh_size = be32(data, b + 20)
        # sections exécutables réelles (.text/.syscall), pas les stubs d'import 0xC000...
        if (sh_flags & 0x4) and sh_addr < 0x10000000 and sh_size > 0:
            text_ranges.append((nm, sh_addr, sh_off, sh_size))

    prim = Counter()
    ps_ops = Counter()   # nom d'op -> nombre (classification X-form prioritaire, sinon A-form)
    psq = Counter()
    total = 0
    for nm, addr, off, size in text_ranges:
        for p in range(0, size & ~3, 4):
            insn = be32(data, off + p)
            op = (insn >> 26) & 0x3F
            prim[op] += 1
            total += 1
            if op == 4:
                xo10 = (insn >> 1) & 0x3FF
                xo5 = (insn >> 1) & 0x1F
                if xo10 in XO10:
                    ps_ops[XO10[xo10]] += 1
                elif xo5 in XO5:
                    ps_ops[XO5[xo5]] += 1
                else:
                    ps_ops[f"op4_XO10={xo10}?"] += 1
            elif op in PRIMARY:
                psq[op] += 1
                ps_ops[PRIMARY[op]] += 1

    print(f"Sections scannées : {[r[0] for r in text_ranges]}")
    print(f"Total instructions : {total}\n")

    op4 = prim.get(4, 0)
    psq_total = sum(psq.values())
    print(f"=== Paired-singles détectés ===")
    print(f"  primaire 4  (ps_* arithmétique) : {op4}")
    print(f"  psq_l/lu/st/stu (56/57/60/61)   : {psq_total}")
    for op in (56, 57, 60, 61):
        if psq.get(op):
            print(f"      {PRIMARY[op]:8} (op {op}) : {psq[op]}")
    pct = 100.0 * (op4 + psq_total) / total if total else 0
    print(f"  => {op4 + psq_total} instr. paired-single sur {total} ({pct:.3f} %)\n")

    print("  Jeu d'opcodes paired-single utilisés (à implémenter) :")
    for nm, c in ps_ops.most_common():
        print(f"      {nm:16} : {c}")

    print("\n=== Primaires les plus fréquents (contexte) ===")
    for op, c in prim.most_common(12):
        print(f"  op {op:2} : {c}")


if __name__ == "__main__":
    main()
