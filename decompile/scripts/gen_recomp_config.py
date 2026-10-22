#!/usr/bin/env python3
"""Génère la config XenonRecomp complète pour carrot_linked.elf.

  * bornes de fonctions : départs Ghidra (carrot_functions.toml) + cibles de `bl`,
    puis partition contiguë de chaque section de code (taille = jusqu'au départ
    suivant). Une fonction coupée en deux par un faux départ reste correcte : le
    saut vers la 2e partie devient un appel terminal.
  * jump tables (`[[switch]]`) : motif Green Hills
        lis rT,tbl@ha ; addi rT,rT,tbl@l ; (slwi rI,rX,2) ; lwzx rD,rT,rI ; mtctr rD ; bctr
    La table (dans .rodata/.data) est lue tant que ses mots portent une relocation
    ADDR32 vers l'intérieur de la fonction ; borne affinée par `cmplwi rX,N` si trouvé.
    Les entrées sont émises sans `r` : le fork XenonRecomp aiguille alors sur la
    valeur de CTR (adresse cible) plutôt que sur un registre d'index.

Usage: python gen_recomp_config.py carrot.elf carrot_linked.elf ghidra_functions.toml \\
           out_dir_config  (écrit carrot.toml + carrot_switch.toml)
"""
import os
import re
import struct
import sys
from bisect import bisect_right

from rpl_elf import RplElf, SHF_ALLOC, SHF_EXECINSTR

BCTR = 0x4E800420


def simm16(ins):
    v = ins & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def find_mtctr(insn_at, bctr_addr):
    """mtctr rD dans les 3 instructions avant le bctr -> (adresse, rD)"""
    for back in range(1, 4):
        ins = insn_at(bctr_addr - 4 * back)
        if ins is None:
            return None
        if (ins & 0xFC1FFFFF) == 0x7C0903A6:
            return bctr_addr - 4 * back, (ins >> 21) & 31
    return None


def find_lis_add(insn_at, from_addr, reg, window=16):
    """Valeur de `reg` construite par lis reg,hi ; addi/addic reg,reg,lo avant from_addr."""
    lo = None
    for back in range(1, window):
        ins = insn_at(from_addr - 4 * back)
        if ins is None:
            return None
        op = ins >> 26
        if (ins >> 21) & 31 != reg:
            continue
        if op in (12, 13, 14) and lo is None and ((ins >> 16) & 31) == reg:  # addic(.)/addi
            lo = simm16(ins)
            continue
        if op == 15 and ((ins >> 16) & 31) == 0:                              # lis
            return (((ins & 0xFFFF) << 16) + (lo or 0)) & 0xFFFFFFFF
        return None
    return None


def find_bound(insn_at, from_addr, window=24):
    """Nombre d'entrées d'après le `cmplwi rX,N` + bgt/bge qui garde le switch."""
    for back in range(1, window):
        a = from_addr - 4 * back
        ins = insn_at(a)
        if ins is None:
            return None
        if (ins >> 26) == 10 and ((ins >> 21) & 0x1C) == 0:   # cmpli cr0,0,rA,N
            n = ins & 0xFFFF
            nxt = insn_at(a + 4)
            # bc 4,1 (bgt => ble faux) / bc 12,0 ... : on regarde BO/BI de bc
            if nxt is not None and nxt >> 26 == 16:
                bo, bi = (nxt >> 21) & 31, (nxt >> 16) & 31
                if bi == 0 and bo == 4:        # bge  (branche si !lt)
                    return n
            return n + 1
    return None


def find_branch_table(insn_at, bctr_addr, fbase, fend):
    """Motif GHS : ctr = table + 4*i, la table est une suite de `b case` dans le code."""
    m = find_mtctr(insn_at, bctr_addr)
    if not m:
        return None
    mt_addr, rD = m
    for back in range(1, 6):
        ins = insn_at(mt_addr - 4 * back)
        if ins is None:
            return None
        if (ins >> 26) == 31 and ((ins >> 1) & 0x1FF) == 266 and ((ins >> 21) & 31) == rD:  # add
            add_addr = mt_addr - 4 * back
            rA, rB = (ins >> 16) & 31, (ins >> 11) & 31
            table = None
            for cand in (rA, rB):
                table = find_lis_add(insn_at, add_addr, cand)
                if table is not None:
                    break
            if table is None:
                return None
            return collect_branches(insn_at, table, fend, find_bound(insn_at, add_addr), fbase)
        if (ins >> 26) == 14 and ((ins >> 21) & 31) == rD:      # addi rD, rS, lo
            rS = (ins >> 16) & 31
            prev = insn_at(mt_addr - 4 * back - 4)
            # addis rS, rIdx, hi  =>  ctr = rIdx + table
            if prev is not None and (prev >> 26) == 15 and ((prev >> 21) & 31) == rS                     and ((prev >> 16) & 31) != 0:
                table = (((prev & 0xFFFF) << 16) + simm16(ins)) & 0xFFFFFFFF
                return collect_branches(insn_at, table, fend, find_bound(insn_at, mt_addr - 4 * back),
                                        fbase)
            return None
        if (ins >> 21) & 31 == rD:
            return None
    return None


def collect_branches(insn_at, table, fend, bound, fbase):
    if not (fbase <= table < fend):
        return None
    labels = []
    k = 0
    while table + 4 * k < fend and (bound is None or k < bound):
        b = insn_at(table + 4 * k)
        if b is None or (b >> 26) != 18 or (b & 3) != 0:
            break
        labels.append(table + 4 * k)
        k += 1
    return labels or None


def find_address_table(insn_at, read32, addr32_sites, bctr_addr, fbase, fend):
    """Motif classique : lwzx rD, rT, rI sur une table d'adresses (relocations ADDR32)."""
    m = find_mtctr(insn_at, bctr_addr)
    if not m:
        return None
    mt_addr, rD = m
    for back in range(1, 7):
        ins = insn_at(mt_addr - 4 * back)
        if ins is None:
            return None
        if (ins >> 26) == 31 and ((ins >> 1) & 0x3FF) == 23 and ((ins >> 21) & 31) == rD:  # lwzx
            lw_addr = mt_addr - 4 * back
            rT, rI = (ins >> 16) & 31, (ins >> 11) & 31
            table = None
            for cand in (rT, rI):
                table = find_lis_add(insn_at, lw_addr, cand)
                if table is not None:
                    break
            if table is None:
                return None
            bound = find_bound(insn_at, lw_addr)
            labels = []
            k = 0
            while bound is None or k < bound:
                site = table + 4 * k
                if site not in addr32_sites:
                    break
                v = read32(site)
                if v is None or not (fbase <= v < fend):
                    break
                labels.append(v)
                k += 1
            return labels or None
    return None


def main():
    orig_path, linked_path, ghidra_toml, out_dir = sys.argv[1:5]
    orig = RplElf(orig_path)          # pour les relocations
    elf = RplElf(linked_path)         # image liée

    code_secs = [s for s in elf.sections if s.flags & SHF_ALLOC and s.flags & SHF_EXECINSTR
                 and s.addr < 0x10000000]
    data_secs = [s for s in elf.sections if s.flags & SHF_ALLOC and s.addr >= 0x10000000
                 and s.type != 8]

    def read32(addr):
        for s in code_secs + data_secs:
            if s.addr <= addr < s.addr + s.size:
                return struct.unpack_from(">I", elf.data, s.offset + addr - s.addr)[0]
        return None

    # Emplacements portant une relocation ADDR32 (pointeurs) dans les données
    addr32_sites = set()
    for rs, tgt in orig.rela_sections():
        for r in orig.relas(rs):
            if r.type == 1:
                addr32_sites.add(r.offset)

    # --- Départs de fonctions
    starts = set()
    txt = open(ghidra_toml, encoding="utf-8").read()
    for m in re.finditer(r"address\s*=\s*0x([0-9A-Fa-f]+)", txt):
        starts.add(int(m.group(1), 16))
    n_ghidra = len(starts)
    for s in code_secs:
        for off in range(0, s.size, 4):
            insn = struct.unpack_from(">I", elf.data, s.offset + off)[0]
            if insn >> 26 == 18 and insn & 3 == 1:           # bl
                disp = insn & 0x03FFFFFC
                if disp & 0x02000000:
                    disp -= 0x04000000
                tgt = s.addr + off + disp
                if any(c.addr <= tgt < c.addr + c.size for c in code_secs):
                    starts.add(tgt)
    starts.add(elf.entry)
    n_bl = len(starts) - n_ghidra

    def partition(starts):
        functions = []
        for s in code_secs:
            sec_starts = sorted(a for a in starts if s.addr <= a < s.addr + s.size)
            if not sec_starts or sec_starts[0] != s.addr:
                sec_starts.insert(0, s.addr)
            for i, a in enumerate(sec_starts):
                end = sec_starts[i + 1] if i + 1 < len(sec_starts) else s.addr + s.size
                functions.append((a, end - a))
        functions.sort()
        return functions

    def detect_switches(functions):
        fstarts = [f[0] for f in functions]
        switches = []
        n_bctr = n_nomatch = 0
        for s in code_secs:
            for off in range(0, s.size, 4):
                if struct.unpack_from(">I", elf.data, s.offset + off)[0] != BCTR:
                    continue
                n_bctr += 1
                bctr_addr = s.addr + off
                fbase, fsize = functions[bisect_right(fstarts, bctr_addr) - 1]
                fend = fbase + fsize

                def insn_at(a):
                    return read32(a) if a >= fbase else None

                labels = find_branch_table(insn_at, bctr_addr, fbase, fend) or                     find_address_table(insn_at, read32, addr32_sites, bctr_addr, fbase, fend)
                if not labels:
                    n_nomatch += 1
                    continue
                switches.append((bctr_addr, labels))
        return switches, n_bctr, n_nomatch

    # 1re passe : jump tables avec les départs Ghidra + bl
    switches, _, _ = detect_switches(partition(starts))
    in_switch = set()
    for _, labels in switches:
        for l in labels:
            in_switch.add(l)
            ins = read32(l)
            if ins is not None and ins >> 26 == 18:          # cible du `b case`
                d = ins & 0x03FFFFFC
                in_switch.add(l + (d - 0x04000000 if d & 0x02000000 else d))

    # Adresses de code référencées par relocation : entrées de vtables et pointeurs de
    # fonction (ADDR32 dans les données, paires lis/addi dans le code). Couvre les
    # thunks C++ `addi r3,r3,-N ; b f` (exclus par l'export Ghidra, isThunk) et les
    # petites fonctions virtuelles (`li r3,N ; blr`…) que Ghidra n'a pas créées.
    # Les branch tables des switch, elles aussi référencées par lis/addi, sont exclues.
    text_idx = {s.index for s in orig.sections if s.flags & SHF_EXECINSTR and s.addr < 0x10000000}
    before = len(starts)
    for rs, tgt in orig.rela_sections():
        for r in orig.relas(rs):
            if r.type not in (1, 4):          # ADDR32, ADDR16_LO
                continue
            sym = orig.symbols[r.sym]
            if sym.shndx not in text_idx:
                continue
            a = (sym.value + r.addend) & 0xFFFFFFFF
            if a & 3 == 0 and a not in in_switch and any(c.addr <= a < c.addr + c.size for c in code_secs):
                starts.add(a)
    n_reloc = len(starts) - before

    functions = partition(starts)
    switches, n_bctr, n_nomatch = detect_switches(functions)

    def branch_target(addr, ins):
        op = ins >> 26
        if op == 18 and ins & 2 == 0:
            d = ins & 0x03FFFFFC
            d = d - 0x04000000 if d & 0x02000000 else d
        elif op == 16 and ins & 2 == 0:
            d = ins & 0xFFFC
            d = d - 0x10000 if d & 0x8000 else d
        else:
            return None
        return (addr + d) & 0xFFFFFFFF

    def is_terminator(ins):
        if ins in (0x4E800020, 0x4E800420, 0x4C000064):        # blr, bctr, rfi
            return True
        if ins >> 26 == 18 and ins & 1 == 0:                    # b (sans lien)
            return True
        if ins >> 26 == 16 and (ins >> 21) & 0x14 == 0x14 and ins & 1 == 0:  # bc toujours
            return True
        return False

    def sec_end(a):
        for c in code_secs:
            if c.addr <= a < c.addr + c.size:
                return c.addr + c.size
        return a

    # Fonctions qui « tombent » dans la suivante (dernière instruction non nulle qui n'est
    # pas un saut inconditionnel) : routines GHS de sauvegarde `stw r14..r31 ; mflr r31 ;
    # blr` dont chaque entrée est une cible de bl, boucles finissant par un bc… On les
    # prolonge jusqu'au prochain terminateur, en chevauchant les fonctions suivantes.
    def extend_end(fb, end):
        limit = min(sec_end(fb), fb + 0x10000)
        last = end - 4
        while last >= fb and read32(last) == 0:
            last -= 4
        if last >= fb and is_terminator(read32(last)):
            return end
        while end < limit and not is_terminator(read32(end - 4)):
            end += 4
        return end

    partition = functions
    fstarts = [f[0] for f in partition]
    extended = 0
    functions = []
    for fb, fsz in partition:
        end = extend_end(fb, fb + fsz)
        extended += end != fb + fsz
        functions.append((fb, end - fb))

    # Branchements vers le milieu d'une autre fonction : code partagé par le linker
    # (fonctions identiques fusionnées) et routines GHS de restauration de registres
    # (`b` vers l'entrée « r28 » d'une suite lwz r24..r31 ; blr). Comme XenonRecomp le
    # fait pour __restgprlr_N sur 360, chaque cible devient une fonction qui chevauche
    # la fonction englobante jusqu'à sa fin (prolongée elle aussi). Point fixe : le code
    # des fonctions prolongées ou ajoutées est lui-même parcouru.
    known = set(fstarts)
    overlapping = {}
    work = list(functions)
    while work:
        fb, fsz = work.pop()
        for addr in range(fb, fb + fsz, 4):
            tgt = branch_target(addr, read32(addr))
            if tgt is None or tgt in known or fb <= tgt < fb + fsz:
                continue
            if not any(c.addr <= tgt < c.addr + c.size for c in code_secs):
                continue
            tb, tsz = partition[bisect_right(fstarts, tgt) - 1]
            overlapping[tgt] = extend_end(tgt, tb + tsz) - tgt
            known.add(tgt)
            work.append((tgt, overlapping[tgt]))
    functions = sorted(functions + list(overlapping.items()))

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "carrot_switch.toml"), "w", encoding="utf-8") as f:
        f.write("# Jump tables de carrot.rpx détectées par scripts/gen_recomp_config.py.\n")
        f.write("# Sans `r` : le fork aiguille sur la valeur de CTR (adresse de la case).\n\n")
        for base, labels in switches:
            f.write(f"[[switch]]\nbase = 0x{base:08X}\nlabels = [\n")
            for l in labels:
                f.write(f"    0x{l:08X},\n")
            f.write("]\n\n")

    with open(os.path.join(out_dir, "carrot.toml"), "w", encoding="utf-8") as f:
        f.write("# Config XenonRecomp de Pikmin 3 (généré par scripts/gen_recomp_config.py).\n")
        f.write("# Chemins relatifs à ce fichier.\n\n")
        f.write("[main]\n")
        f.write('file_path = "../carrot_linked.elf"\n')
        f.write('out_directory_path = "../generated"\n')
        f.write('switch_table_file_path = "carrot_switch.toml"\n')
        f.write('import_file_path = "imports.toml"\n\n')
        for a, sz in functions:
            f.write(f"[[main.functions]]\naddress = 0x{a:08X}\nsize = 0x{sz:X}\n\n")

    print(f"Fonctions : {len(functions)} (Ghidra {n_ghidra}, +{n_bl} cibles de bl, "
          f"+{n_reloc} références par relocation)")
    print(f"Fonctions chevauchantes (entrées en milieu de fonction) : {len(overlapping)}")
    print(f"Fonctions prolongées jusqu'à leur terminateur (fall-through) : {extended}")
    print(f"bctr : {n_bctr}, jump tables reconnues : {len(switches)}, non reconnus : {n_nomatch}")
    print(f"Cases au total : {sum(len(l) for _, l in switches)}")


if __name__ == "__main__":
    main()
