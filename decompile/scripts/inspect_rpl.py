#!/usr/bin/env python3
"""Inventaire d'un RPL décompressé : sections, types de relocations, imports, FILEINFO.

Usage: python inspect_rpl.py <carrot.elf>
"""
import struct
import sys
from collections import Counter

from rpl_elf import RplElf, R_PPC_NAMES, SHT_RPL_FILEINFO


def main():
    elf = RplElf(sys.argv[1])
    print(f"entry = 0x{elf.entry:08X}")
    print("\n## Sections")
    for s in elf.sections:
        print(f"  [{s.index:2d}] {s.name:<28} type=0x{s.type:08X} flags=0x{s.flags:08X} "
              f"addr=0x{s.addr:08X} size=0x{s.size:08X} align={s.align}")

    print("\n## Relocations par section cible")
    for rs, tgt in elf.rela_sections():
        cnt = Counter()
        imp = Counter()
        for r in elf.relas(rs):
            cnt[R_PPC_NAMES.get(r.type, str(r.type))] += 1
            sym = elf.symbols[r.sym]
            if elf.import_lib(sym):
                imp[R_PPC_NAMES.get(r.type, str(r.type))] += 1
        print(f"  {rs.name} -> {tgt.name}: {dict(cnt)}")
        if imp:
            print(f"      dont vers imports : {dict(imp)}")

    fi = next((s for s in elf.sections if s.type == SHT_RPL_FILEINFO), None)
    if fi:
        raw = elf.contents(fi)
        words = struct.unpack_from(">%dI" % (min(len(raw), 0x60) // 4), raw)
        names = ["magic_version", "textSize", "textAlign", "dataSize", "dataAlign", "loadSize",
                 "loadAlign", "tempSize", "trampAdjust", "sdaBase", "sda2Base", "stackSize",
                 "filename", "flags", "heapSize", "tagOffset", "minVersion", "compressionLevel",
                 "trampAddition", "fileInfoPad", "cafeSdkVersion", "cafeSdkRevision",
                 "tlsModuleIndex", "tlsAlignShift"]
        print("\n## FILEINFO")
        for n, w in zip(names, words):
            print(f"  {n:<18} 0x{w:08X}")

    # Quelques exemples de REL24 vers imports : mot brut à l'emplacement
    text = elf.section_by_name(".text")
    rt = elf.section_by_name(".rela.text")
    if text and rt:
        print("\n## Exemples REL24 -> import (mot brut dans .text)")
        n = 0
        for r in elf.relas(rt):
            sym = elf.symbols[r.sym]
            if r.type == 10 and elf.import_lib(sym):
                word = struct.unpack_from(">I", elf.data, text.offset + r.offset - text.addr)[0]
                print(f"  site 0x{r.offset:08X} -> {elf.import_lib(sym)}::{sym.name} "
                      f"(sym=0x{sym.value:08X}, add={r.addend}) mot=0x{word:08X}")
                n += 1
                if n >= 6:
                    break
        print("\n## Exemples ADDR16_HA/LO -> import data")
        n = 0
        for r in elf.relas(rt):
            sym = elf.symbols[r.sym]
            if r.type in (4, 6) and elf.import_lib(sym):
                word = struct.unpack_from(">I", elf.data, text.offset + (r.offset & ~3) - text.addr)[0]
                print(f"  site 0x{r.offset:08X} {R_PPC_NAMES[r.type]} -> {elf.import_lib(sym)}::{sym.name} "
                      f"(sym=0x{sym.value:08X}) mot=0x{word:08X}")
                n += 1
                if n >= 6:
                    break
        # Vérifie si les relocations internes sont déjà appliquées (image liée à son adresse)
        bad = 0
        tot = 0
        for r in elf.relas(rt):
            sym = elf.symbols[r.sym]
            if r.type == 10 and not elf.import_lib(sym) and sym.shndx != 0:
                tot += 1
                word = struct.unpack_from(">I", elf.data, text.offset + r.offset - text.addr)[0]
                disp = word & 0x03FFFFFC
                if disp & 0x02000000:
                    disp -= 0x04000000
                if (r.offset + disp) & 0xFFFFFFFF != (sym.value + r.addend) & 0xFFFFFFFF:
                    bad += 1
        print(f"\n## REL24 internes déjà résolus dans l'image : {tot - bad}/{tot}")


if __name__ == "__main__":
    main()
