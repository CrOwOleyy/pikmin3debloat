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
# Programmes dessinant les morceaux de cadre des fenêtres (identifiés avec --debug-colors) :
# coordonnées de texture générées à partir de la taille du morceau (lyt_frame.vsh).
FRAME_PROGRAMS = {3}


def blocks(data):
    pos = struct.unpack_from(">I", data, 4)[0]  # taille de l'en-tête de fichier
    while pos < len(data):
        assert data[pos:pos + 4] == b"BLK{", hex(pos)
        hsize, _, _, btype, dsize = struct.unpack_from(">IIIII", data, pos + 4)
        yield btype, bytearray(data[pos:pos + hsize + dsize])
        pos += hsize + dsize


DEBUG_PSH = """; $MODE = "UniformRegister"
; $SQ_PGM_RESOURCES_PS.NUM_GPRS = 1
; $SQ_PGM_EXPORTS_PS.EXPORT_MODE = 2
; $CB_SHADER_MASK.OUTPUT0_ENABLE = 15
; $CB_SHADER_CONTROL.RT0_ENABLE = true
00 ALU: ADDR(32) CNT({cnt})
   0  x: MOV             R0.x, ({r:#010x}, 0)
      y: MOV             R0.y, ({g:#010x}, 0)
      z: MOV             R0.z, ({b:#010x}, 0)
      w: MOV             R0.w, 1.0f
01 EXP_DONE: PIX0, R0.xyzw
END_OF_PROGRAM
"""


def debug_color(i):
    """Couleur unique du programme i : R = bits 0-1, V = bits 2-3, B = bit 4 (pas de 85/255)."""
    f = lambda v: struct.unpack(">I", struct.pack(">f", v))[0]
    return f((i & 3) * 85 / 255), f(((i >> 2) & 3) * 85 / 255), f((i >> 4) & 1)


def assemble(assembler, tmp, psh, vsh=HERE / "lyt.vsh"):
    one = Path(tmp) / "one.gsh"
    subprocess.run([assembler, "assemble", f"--vsh={vsh}", f"--psh={psh}", str(one)], check=True)
    data = one.read_bytes()
    program, end = [], []
    for btype, blk in blocks(data):
        (end if btype == 1 else program).append(blk)
    assert end and len(program) == 4, [struct.unpack_from(">I", b, 16)[0] for b in program]
    return data[:struct.unpack_from(">I", data, 4)[0]], program, end[0]


def main():
    assembler, out = str(Path(sys.argv[1]).resolve()), Path(sys.argv[2])
    debug = "--debug-colors" in sys.argv  # chaque programme peint une couleur unique
    with tempfile.TemporaryDirectory() as tmp:
        # --psh=<fichier> : shader de pixels de diagnostic pour tous les programmes.
        psh = next((Path(a[6:]) for a in sys.argv if a.startswith("--psh=")), HERE / "lyt.psh")
        header, program, end = assemble(assembler, tmp, psh)
        frame = assemble(assembler, tmp, psh, HERE / "lyt_frame.vsh")[1]
        programs = [frame if i in FRAME_PROGRAMS else program for i in range(PROGRAM_COUNT)]
        if debug:
            programs = []
            for i in range(PROGRAM_COUNT):
                r, g, b = debug_color(i)
                psh = Path(tmp) / "debug.psh"
                for cnt in range(4, 12):
                    psh.write_text(DEBUG_PSH.format(cnt=cnt, r=r, g=g, b=b))
                    if subprocess.run([assembler, "assemble", f"--vsh={HERE / 'lyt.vsh'}", f"--psh={psh}",
                                       str(Path(tmp) / "probe.gsh")], capture_output=True).returncode == 0:
                        break
                programs.append(assemble(assembler, tmp, psh)[1])

    result = bytearray(header)
    for i in range(PROGRAM_COUNT):
        for n, blk in enumerate(programs[i]):
            blk = bytearray(blk)
            struct.pack_into(">I", blk, 28, i)  # champ index du bloc
            if n == 0:
                # En-tête GX2VertexShader : ringItemsize (+0x10C), lu seulement en mode
                # géométrie, porte le numéro du programme pour les sondes (--probe).
                struct.pack_into(">I", blk, 0x20 + 0x10C, i)
            result += blk
    result += end
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(result)
    print(f"{out} : {PROGRAM_COUNT} programmes, {len(result)} octets")


if __name__ == "__main__":
    main()
