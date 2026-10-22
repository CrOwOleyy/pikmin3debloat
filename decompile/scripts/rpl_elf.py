#!/usr/bin/env python3
"""Petit parseur ELF32 big-endian pour les RPL/RPX Cafe OS décompressés.

Module partagé par les scripts du runtime (link_rpl.py, inspect_rpl.py).
Aucune dépendance externe.
"""
import struct
from dataclasses import dataclass

SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHT_RPL_EXPORTS = 0x80000001
SHT_RPL_IMPORTS = 0x80000002
SHT_RPL_CRCS = 0x80000003
SHT_RPL_FILEINFO = 0x80000004

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

R_PPC_NAMES = {
    0: "NONE", 1: "ADDR32", 2: "ADDR24", 3: "ADDR16", 4: "ADDR16_LO", 5: "ADDR16_HI",
    6: "ADDR16_HA", 7: "ADDR14", 10: "REL24", 11: "REL14", 26: "REL32",
    67: "TLS", 68: "DTPMOD32", 78: "DTPREL32",
    109: "EMB_SDA21", 116: "EMB_RELSDA",
    201: "DIAB_SDA21_LO", 202: "DIAB_SDA21_HI", 203: "DIAB_SDA21_HA",
    204: "DIAB_RELSDA_LO", 205: "DIAB_RELSDA_HI", 206: "DIAB_RELSDA_HA",
    248: "GHS_REL16_HA", 249: "GHS_REL16_HI", 250: "GHS_REL16_LO",
}


@dataclass
class Section:
    index: int
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    info: int
    align: int
    entsize: int


@dataclass
class Symbol:
    index: int
    name: str
    value: int
    size: int
    info: int
    shndx: int

    @property
    def type(self):
        return self.info & 0xF

    @property
    def bind(self):
        return self.info >> 4


@dataclass
class Rela:
    offset: int
    sym: int
    type: int
    addend: int


class RplElf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 2:
            raise ValueError("pas un ELF32 big-endian")
        (self.e_type, self.e_machine, _, self.entry, _, shoff, _, _, _, _,
         shentsize, shnum, shstrndx) = struct.unpack_from(">HHIIIIIHHHHHH", d, 0x10)
        raw = []
        for i in range(shnum):
            raw.append(struct.unpack_from(">10I", d, shoff + i * shentsize))
        shstr_off = raw[shstrndx][4]
        self.sections = []
        for i, (nm, ty, fl, ad, of, sz, ln, inf, al, es) in enumerate(raw):
            self.sections.append(Section(i, self._cstr(shstr_off + nm), ty, fl, ad, of, sz,
                                         ln, inf, al, es))
        self.symbols = []
        symtab = next((s for s in self.sections if s.type == SHT_SYMTAB), None)
        if symtab:
            stroff = self.sections[symtab.link].offset
            es = symtab.entsize or 16
            for i in range(symtab.size // es):
                nm, val, sz, info, _, shndx = struct.unpack_from(">IIIBBH", d, symtab.offset + i * es)
                self.symbols.append(Symbol(i, self._cstr(stroff + nm), val, sz, info, shndx))

    def _cstr(self, off):
        end = self.data.index(b"\x00", off)
        return self.data[off:end].decode("latin-1")

    def section_by_name(self, name):
        return next((s for s in self.sections if s.name == name), None)

    def contents(self, sec):
        if sec.type == SHT_NOBITS:
            return bytes(sec.size)
        return self.data[sec.offset:sec.offset + sec.size]

    def relas(self, sec):
        out = []
        for i in range(sec.size // 12):
            off, info, add = struct.unpack_from(">IIi", self.data, sec.offset + i * 12)
            out.append(Rela(off, info >> 8, info & 0xFF, add))
        return out

    def rela_sections(self):
        """(section rela, section cible) pour chaque .rela.*"""
        return [(s, self.sections[s.info]) for s in self.sections if s.type == SHT_RELA]

    def import_lib(self, sym):
        """Nom de la bibliothèque si le symbole vit dans une section .fimport_/.dimport_."""
        if sym.shndx == 0 or sym.shndx >= len(self.sections):
            return None
        n = self.sections[sym.shndx].name
        for p in (".fimport_", ".dimport_"):
            if n.startswith(p):
                lib = n[len(p):]
                return lib[:-4] if lib.endswith(".rpl") else lib
        return None

    def is_func_import(self, sym):
        return sym.shndx < len(self.sections) and self.sections[sym.shndx].name.startswith(".fimport_")
