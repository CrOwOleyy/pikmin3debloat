#!/usr/bin/env python3
"""« Linke » statiquement un RPL décompressé pour la recompilation.

Le RPX est relogeable : ses ~12 000 appels vers Cafe OS (`bl OSBlockMove`…) ne
contiennent pas de cible valide dans l'image, ils ne sont résolus que par les
relocations .rela.text au chargement. Ce script fait le travail du loader Cafe OS :

  * les symboles importés (.fimport_*/.dimport_*, à 0xC000xxxx dans l'ELF) sont
    déplacés dans une zone « imports » à IMPORT_BASE, à portée d'un `bl` (±32 Mo)
    du .text ;
  * toutes les relocations (.rela.text/.data/.rodata) sont appliquées avec ces
    adresses (les relocations internes sont vérifiées : l'image est déjà liée à son
    adresse de chargement, elles ne doivent rien changer) ;
  * les sections non chargées (imports, symtab…) perdent SHF_ALLOC pour que
    XenonRecomp ne les prenne pas pour du code.

Sorties :
  <out.elf>                 ELF lié, entrée de XenonRecomp et du runtime
  <imports.toml>            [[import]] address/name (fonctions) pour XenonRecomp
  <imports.inc>             X-macros CAFE_IMPORT_FUNC / CAFE_IMPORT_DATA pour le runtime

Usage: python link_rpl.py carrot.elf out.elf imports.toml imports.inc
"""
import re
import struct
import sys
from collections import Counter

from rpl_elf import RplElf, R_PPC_NAMES, SHF_ALLOC

LOAD_BASE = 0xC0000000     # adresse des sections « loader » dans l'ELF
IMPORT_BASE = 0x03500000   # nouvelle adresse de ces sections (après .text, à portée de bl)
IMPORT_END = IMPORT_BASE + 0x10000
# Cible des références à « $UNDEF » (symboles faibles non définis) : on les fait
# pointer sur un stub HLE dédié plutôt que sur 0.
UNDEF_ADDR = IMPORT_BASE
UNDEF_NAME = "cafe_undefined__UNDEF"
# Les données importées (_iob, pointeurs MEMAllocFromDefaultHeap…) n'ont que 8 octets
# dans .dimport : chacune reçoit son propre bloc, que le runtime initialise.
DATA_IMPORT_BASE = 0x03510000
DATA_IMPORT_STRIDE = 0x1000


def remap(addr):
    if addr >= LOAD_BASE:
        return addr - LOAD_BASE + IMPORT_BASE
    return addr


def ident(name):
    return re.sub(r"[^0-9A-Za-z_]", "_", name)


def main():
    src, out_elf, out_toml, out_inc = sys.argv[1:5]
    elf = RplElf(src)
    data = bytearray(elf.data)

    # Adresse -> offset fichier, pour les sections chargées
    loaded = [s for s in elf.sections if s.flags & SHF_ALLOC and s.addr and s.addr < LOAD_BASE
              and s.type != 8]

    def file_off(addr):
        for s in loaded:
            if s.addr <= addr < s.addr + s.size:
                return s.offset + addr - s.addr
        raise ValueError(f"adresse 0x{addr:08X} hors des sections chargées")

    # Adresse finale de chaque symbole importé
    import_addr = {}
    n_data = 0
    for sym in elf.symbols:
        if elf.import_lib(sym) is None:
            continue
        if elf.is_func_import(sym) or not sym.name or sym.name.startswith("."):
            import_addr[sym.index] = remap(sym.value)
        else:
            import_addr[sym.index] = DATA_IMPORT_BASE + n_data * DATA_IMPORT_STRIDE
            n_data += 1

    stats = Counter()
    changed = Counter()
    for rs, tgt in elf.rela_sections():
        for r in elf.relas(rs):
            sym = elf.symbols[r.sym]
            is_import = elf.import_lib(sym) is not None
            S = import_addr[sym.index] if is_import else sym.value
            if sym.shndx == 0:
                # Le crt0 teste `&symbole_faible != 0` : l'adresse vaut 0 comme sur console ;
                # seul le `bl` (gardé par ce test) vise le stub, faute de pouvoir viser 0.
                is_import = True
                S = UNDEF_ADDR if r.type == 10 else 0
                print(f"  $UNDEF {R_PPC_NAMES.get(r.type, r.type)} en 0x{r.offset:08X} -> {UNDEF_NAME}")
            value = (S + r.addend) & 0xFFFFFFFF
            off = file_off(r.offset)
            tname = R_PPC_NAMES.get(r.type, str(r.type))
            stats[(tname, is_import)] += 1

            if r.type == 1:      # ADDR32
                new = struct.pack(">I", value)
                width = 4
            elif r.type in (4, 5, 6):   # ADDR16_LO / HI / HA
                if r.type == 4:
                    half = value & 0xFFFF
                elif r.type == 5:
                    half = value >> 16
                else:
                    half = ((value + 0x8000) >> 16) & 0xFFFF
                new = struct.pack(">H", half)
                width = 2
            elif r.type == 10:   # REL24
                disp = (value - r.offset) & 0xFFFFFFFF
                if disp & 0x80000000:
                    disp -= 0x100000000
                if not -0x2000000 <= disp < 0x2000000:
                    raise ValueError(f"REL24 hors portée en 0x{r.offset:08X} -> 0x{value:08X}")
                insn = struct.unpack_from(">I", data, off)[0]
                insn = (insn & 0xFC000003) | (disp & 0x03FFFFFC)
                new = struct.pack(">I", insn)
                width = 4
            else:
                raise ValueError(f"relocation non gérée : {tname} en 0x{r.offset:08X}")

            if bytes(data[off:off + width]) != new:
                changed[(tname, is_import)] += 1
                data[off:off + width] = new

    print("Relocations appliquées (type, vers import) : total / octets modifiés")
    for k in sorted(stats):
        print(f"  {k[0]:<10} import={k[1]!s:<5} {stats[k]:7d} / {changed[k]:7d}")
    internal_changed = sum(v for (t, imp), v in changed.items() if not imp)
    if internal_changed:
        print(f"ATTENTION : {internal_changed} relocations internes ont modifié l'image")

    # Retire SHF_ALLOC des sections « loader » (imports, symtab, strtab, shstrtab)
    shoff = struct.unpack_from(">I", elf.data, 0x20)[0]
    shentsize = struct.unpack_from(">H", elf.data, 0x2E)[0]
    for s in elf.sections:
        if s.addr >= LOAD_BASE:
            hdr = shoff + s.index * shentsize
            struct.pack_into(">I", data, hdr + 8, s.flags & ~SHF_ALLOC)
            struct.pack_into(">I", data, hdr + 12, remap(s.addr))

    with open(out_elf, "wb") as f:
        f.write(data)

    # Liste des imports
    funcs, datas = [], []
    seen = set()
    for sym in elf.symbols:
        lib = elf.import_lib(sym)
        if not lib or not sym.name or sym.name.startswith("."):
            continue
        key = (lib, sym.name)
        if key in seen:
            continue
        seen.add(key)
        entry = (lib, sym.name, import_addr[sym.index], sym.size)
        (funcs if elf.is_func_import(sym) else datas).append(entry)
    funcs.append(("undefined", "$UNDEF", UNDEF_ADDR, 0))
    funcs.sort(key=lambda e: e[2])
    datas.sort(key=lambda e: e[2])

    with open(out_toml, "w", encoding="utf-8") as f:
        f.write("# Imports Cafe OS de carrot.rpx relogés par scripts/link_rpl.py.\n")
        f.write("# Chaque appel du jeu vers ces adresses devient un appel à la fonction HLE nommée.\n\n")
        for lib, name, addr, _ in funcs:
            f.write(f"[[import]]\naddress = 0x{addr:08X}\nname = \"cafe_{ident(lib)}_{ident(name)}\"\n\n")

    with open(out_inc, "w", encoding="utf-8") as f:
        f.write("// Généré par scripts/link_rpl.py - ne pas éditer.\n")
        f.write(f"// Zone imports : 0x{IMPORT_BASE:08X}..0x{IMPORT_END:08X}\n")
        f.write("// CAFE_IMPORT_FUNC(lib, symbole, nom C, adresse)\n")
        f.write("// CAFE_IMPORT_DATA(lib, symbole, nom C, adresse, taille)\n")
        for lib, name, addr, _ in funcs:
            f.write(f"CAFE_IMPORT_FUNC({ident(lib)}, \"{name}\", cafe_{ident(lib)}_{ident(name)}, 0x{addr:08X})\n")
        for lib, name, addr, size in datas:
            f.write(f"CAFE_IMPORT_DATA({ident(lib)}, \"{name}\", cafe_{ident(lib)}_{ident(name)}, 0x{addr:08X}, {size})\n")

    print(f"{len(funcs)} fonctions importées, {len(datas)} données importées")
    for lib, name, addr, size in datas:
        print(f"  [D] {lib}::{name} @0x{addr:08X} ({size} o)")


if __name__ == "__main__":
    main()
