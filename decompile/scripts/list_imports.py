#!/usr/bin/env python3
"""Liste les symboles importés d'un RPL/RPX Cafe OS décompressé (ELF32 big-endian).

Usage: python3 -I list_imports.py <carrot.elf> [out.txt]

Pour chaque bibliothèque système (.fimport_<lib> / .dimport_<lib>), extrait les
symboles que le jeu importe en lisant .symtab/.strtab. C'est le cahier des charges
du runtime HLE : tout ce qui est listé ici doit être réimplémenté.
"""
import struct
import sys
from collections import defaultdict


def be(fmt, buf, off):
    return struct.unpack_from(">" + fmt, buf, off)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else None
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        print("Pas un ELF", file=sys.stderr)
        return 1
    # En-tête ELF32 big-endian : offsets fixes
    e_shoff = be("I", data, 0x20)[0]
    e_shentsize = be("H", data, 0x2E)[0]
    e_shnum = be("H", data, 0x30)[0]
    e_shstrndx = be("H", data, 0x32)[0]

    sections = []  # (name_off, type, offset, size, link, entsize)
    for i in range(e_shnum):
        b = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, \
            sh_info, sh_align, sh_entsize = be("10I", data, b)
        sections.append((sh_name, sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    shstr_off = sections[e_shstrndx][2]

    def cstr(base, off):
        end = data.index(b"\x00", base + off)
        return data[base + off:end].decode("latin-1")

    sec_names = [cstr(shstr_off, s[0]) for s in sections]

    # Trouver .symtab (type 2) et son .strtab lié
    sym_idx = next((i for i, s in enumerate(sections) if s[1] == 2), None)
    if sym_idx is None:
        print("Pas de .symtab", file=sys.stderr)
        return 1
    _, _, sym_off, sym_size, sym_link, sym_es = sections[sym_idx]
    str_off = sections[sym_link][2]
    es = sym_es or 16

    imports = defaultdict(lambda: {"func": [], "data": []})
    n = sym_size // es
    for i in range(n):
        b = sym_off + i * es
        st_name, st_value, st_size, st_info, st_other, st_shndx = be("IIIBBH", data, b)
        if st_shndx >= len(sec_names):
            continue
        sname = sec_names[st_shndx]
        if sname.startswith(".fimport_") or sname.startswith(".dimport_"):
            kind = "func" if sname.startswith(".fimport_") else "data"
            lib = sname.split("_", 1)[1]
            if lib.endswith(".rpl"):
                lib = lib[:-4]
            symbol = cstr(str_off, st_name)
            # Ignorer le symbole-label de la section elle-meme (.fimport_xxx)
            if symbol and not symbol.startswith("."):
                imports[lib][kind].append(symbol)

    libs = sorted(imports)
    lines = []
    total_f = total_d = 0
    lines.append("# Imports RPL de carrot.rpx (Update v96) - cahier des charges HLE")
    lines.append("# Genere par scripts/list_imports.py\n")
    lines.append("## Resume par bibliotheque\n")
    for lib in libs:
        nf = len(imports[lib]["func"])
        nd = len(imports[lib]["data"])
        total_f += nf
        total_d += nd
        lines.append(f"  {lib:<14} {nf:4d} fonctions, {nd:3d} data")
    lines.append(f"\n  TOTAL : {len(libs)} bibliotheques, {total_f} fonctions, {total_d} data\n")

    lines.append("## Detail par bibliotheque\n")
    for lib in libs:
        lines.append(f"### {lib}")
        for sym in sorted(imports[lib]["func"]):
            lines.append(f"  [F] {sym}")
        for sym in sorted(imports[lib]["data"]):
            lines.append(f"  [D] {sym}")
        lines.append("")

    text = "\n".join(lines)
    if out_path:
        with open(out_path, "w") as f:
            f.write(text + "\n")
        print(f"Ecrit : {out_path}")
    # Toujours afficher le resume
    print("\n".join(lines[: 4 + len(libs) + 2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
