#!/usr/bin/env python3
"""Génère un mini-ELF32 PowerPC big-endian contenant quelques instructions paired-single.

Sert à tester VITE le recompilateur (XenonRecomp) sans scanner les 21 Mo de .text de carrot.elf
(Recompiler::Analyse() scanne toute la section .text, même pour une seule fonction ciblée).

Usage: python3 make_tiny_ps_elf.py [sortie.elf]
Puis config XenonRecomp minimale :
    [main]
    file_path = "tiny.elf"
    out_directory_path = "ppc"
    [[main.functions]]
    address = 0x02000000
    size = 0x18
"""
import struct
import sys

# Instructions big-endian (vérifiées via scripts/test_disasm.cpp)
TEXT = [
    0x1022182A,  # ps_add     f1,f2,f3
    0x108501B2,  # ps_mul     f4,f5,f6   (A x C)
    0x10E84CA0,  # ps_merge10 f7,f8,f9
    0xE0230000,  # psq_l      f1,0(r3),0,0  (GQR0)
    0xF0240000,  # psq_st     f1,0(r4),0,0  (GQR0)
    0x4E800020,  # blr
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tiny.elf"
    tbytes = b"".join(struct.pack(">I", w) for w in TEXT)
    shstr = b"\x00.text\x00.shstrtab\x00"
    text_off = 52
    shstr_off = text_off + len(tbytes)
    shoff = (shstr_off + len(shstr) + 3) & ~3

    def shdr(name, typ, flags, addr, off, size, link, info, align, ent):
        return struct.pack(">10I", name, typ, flags, addr, off, size, link, info, align, ent)

    sects = shdr(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    sects += shdr(1, 1, 6, 0x02000000, text_off, len(tbytes), 0, 0, 4, 0)   # .text ALLOC|EXEC
    sects += shdr(7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)            # .shstrtab

    e_ident = bytes([0x7F, 0x45, 0x4C, 0x46, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    ehdr = e_ident + struct.pack(">HHIIIIIHHHHHH",
                                 2, 20, 1, 0x02000000, 0, shoff, 0, 52, 0, 0, 40, 3, 2)

    data = bytearray(shoff + len(sects))
    data[0:len(ehdr)] = ehdr
    data[text_off:text_off + len(tbytes)] = tbytes
    data[shstr_off:shstr_off + len(shstr)] = shstr
    data[shoff:shoff + len(sects)] = sects
    open(out, "wb").write(data)
    print(f"{out} écrit : {len(data)} octets (.text @0x02000000, {len(TEXT)} instr.)")


if __name__ == "__main__":
    main()
