#!/usr/bin/env python3
"""Fabrique lyt_BuildinShader.gsh (remplacement) à partir de lyt.vsh / lyt.psh.

nw::lyt charge 30 programmes (sommets + pixels) depuis ce fichier ; nos programmes sont
génériques (pilotés par les uniformes), on duplique donc la même paire 30 fois.

Usage : python make_lyt_gsh.py <latte_assembler.exe> <sortie.gsh>
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROGRAM_COUNT = 30


def blocks(data):
    pos = struct.unpack_from(">I", data, 4)[0]  # taille de l'en-tête de fichier
    while pos < len(data):
        assert data[pos:pos + 4] == b"BLK{", hex(pos)
        hsize, _, _, btype, dsize = struct.unpack_from(">IIIII", data, pos + 4)
        yield btype, bytearray(data[pos:pos + hsize + dsize])
        pos += hsize + dsize


def main():
    assembler, out = str(Path(sys.argv[1]).resolve()), Path(sys.argv[2])
    with tempfile.TemporaryDirectory() as tmp:
        one = Path(tmp) / "one.gsh"
        subprocess.run([assembler, "assemble", f"--vsh={HERE / 'lyt.vsh'}", f"--psh={HERE / 'lyt.psh'}",
                        str(one)], check=True)
        data = one.read_bytes()

    header = data[:struct.unpack_from(">I", data, 4)[0]]
    program, end = [], []
    for btype, blk in blocks(data):
        (end if btype == 1 else program).append(blk)
    assert end and len(program) == 4, [struct.unpack_from(">I", b, 16)[0] for b in program]

    result = bytearray(header)
    for i in range(PROGRAM_COUNT):
        for blk in program:
            blk = bytearray(blk)
            struct.pack_into(">I", blk, 28, i)  # champ index du bloc
            result += blk
    result += end[0]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(result)
    print(f"{out} : {PROGRAM_COUNT} programmes, {len(result)} octets")


if __name__ == "__main__":
    main()
