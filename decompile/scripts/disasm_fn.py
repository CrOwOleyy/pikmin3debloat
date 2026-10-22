#!/usr/bin/env python3
"""Désassemble une plage de l'image liée avec les noms des cibles d'appel.

Usage: python disasm_fn.py <début> <fin|+octets> [--elf carrot_linked.elf]
  ex.: python disasm_fn.py 0x3130100 0x3130140
       python disasm_fn.py 0x3130100 +0x80
Les `bl` vers un import Cafe OS sont annotés `lib::symbole`, ceux vers le code du jeu `sub_XXXX`.
Nécessite capstone (pip install capstone) ; les paired-singles sont décodés à la main (capstone
ne connaît pas le Gekko).
"""
import argparse
import re
import struct
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from rpl_elf import RplElf  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent / "recomp"

PS_XO10 = {528: "ps_merge00", 560: "ps_merge01", 592: "ps_merge10", 624: "ps_merge11", 72: "ps_mr",
           40: "ps_neg", 264: "ps_abs", 136: "ps_nabs", 0: "ps_cmpu0", 32: "ps_cmpo0"}
PS_XO5 = {21: "ps_add", 20: "ps_sub", 18: "ps_div", 25: "ps_mul", 12: "ps_muls0", 13: "ps_muls1",
          24: "ps_res", 26: "ps_rsqrte", 29: "ps_madd", 14: "ps_madds0", 15: "ps_madds1", 28: "ps_msub",
          31: "ps_nmadd", 30: "ps_nmsub", 10: "ps_sum0", 11: "ps_sum1", 23: "ps_sel"}


def decode_ps(w):
    prim = w >> 26
    d, a, b, c = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    if prim in (56, 60):
        disp = (w & 0xFFF) ^ 0x800
        disp -= 0x800
        return f"{'psq_l' if prim == 56 else 'psq_st'} f{d}, {disp}(r{a}), {(w >> 15) & 1}, {(w >> 12) & 7}"
    if prim != 4:
        return None
    xo10, xo5 = (w >> 1) & 0x3FF, (w >> 1) & 0x1F
    if xo10 in PS_XO10:
        n = PS_XO10[xo10]
        return f"{n} f{d}, f{b}" if n in ("ps_mr", "ps_neg", "ps_abs", "ps_nabs") else f"{n} f{d}, f{a}, f{b}"
    if xo5 in PS_XO5:
        n = PS_XO5[xo5]
        if n in ("ps_add", "ps_sub", "ps_div"):
            return f"{n} f{d}, f{a}, f{b}"
        if n in ("ps_mul", "ps_muls0", "ps_muls1"):
            return f"{n} f{d}, f{a}, f{c}"
        if n in ("ps_res", "ps_rsqrte"):
            return f"{n} f{d}, f{b}"
        return f"{n} f{d}, f{a}, f{c}, f{b}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("start")
    ap.add_argument("end")
    ap.add_argument("--elf", default=str(ROOT / "carrot_linked.elf"))
    args = ap.parse_args()
    start = int(args.start, 16)
    end = start + int(args.end[1:], 16) if args.end.startswith("+") else int(args.end, 16)

    elf = RplElf(args.elf)
    text = elf.section_by_name(".text")
    imports = {}
    inc = ROOT / "runtime" / "generated" / "imports.inc"
    if inc.exists():
        for m in re.finditer(r'CAFE_IMPORT_FUNC\((\w+), "([^"]+)", \w+, 0x([0-9A-F]+)\)', inc.read_text(encoding="utf-8")):
            imports[int(m.group(3), 16)] = f"{m.group(1)}::{m.group(2)}"

    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    md.skipdata = True
    data = elf.data[text.offset + start - text.addr: text.offset + end - text.addr]
    for off in range(0, len(data), 4):
        addr = start + off
        w = struct.unpack_from(">I", data, off)[0]
        ps = decode_ps(w)
        if ps:
            print(f"  {addr:08X}  {w:08X}  {ps}   <ps>")
            continue
        ins = next(md.disasm(data[off:off + 4], addr), None)
        txt = f"{ins.mnemonic} {ins.op_str}" if ins else f".long 0x{w:08X}"
        note = ""
        if ins and ins.mnemonic in ("bl", "b", "bla") and ins.op_str.startswith("0x"):
            tgt = int(ins.op_str, 16)
            note = f"   ; {imports.get(tgt, f'sub_{tgt:X}')}"
        print(f"  {addr:08X}  {w:08X}  {txt}{note}")


if __name__ == "__main__":
    main()
