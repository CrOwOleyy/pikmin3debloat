#!/usr/bin/env python3
"""Fabrique lyt_BuildinShader.gsh (remplacement) à partir de lyt.vsh / lyt.psh.

nw::lyt charge 30 programmes (sommets + pixels) depuis ce fichier ; nos programmes sont
génériques (pilotés par les uniformes), on duplique donc la même paire 30 fois.

Usage : python make_lyt_gsh.py <latte_assembler.exe> <sortie.gsh>
"""
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROGRAM_COUNT = 30
# Programmes dessinant les morceaux de cadre des fenêtres (identifiés avec --debug-colors) :
# coordonnées de texture générées à partir de la taille du morceau (lyt_frame.vsh).
FRAME_PROGRAMS = set()  # lyt.vsh gère désormais les cadres (uFrameSpec) pour tous les programmes


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


def fix_clauses(assembler, tmp, vsh, text, psh=None):
    """Assemble `text` en corrigeant ADDR/CNT des clauses d'après les erreurs de l'assembleur.
    Sans `psh`, `text` est le shader de pixels (assemblé avec `vsh`) ; avec `psh`, `text` est le
    shader de sommets (assemblé avec ce `psh`)."""
    target = Path(tmp) / ("fixed.vsh" if psh else "fixed.psh")
    # Renumérotation des groupes ALU et instructions TEX (des groupes ont pu être insérés)
    n, out = 0, []
    for line in text.split("\n"):
        m = re.match(r"^\s+\d+(\s+(?:[xyzwt]:|SAMPLE).*)$", line)
        if m:
            line = f"  {n:2d}{m[1]}"
            n += 1
        out.append(line)
    text = "\n".join(out)
    for _ in range(60):
        target.write_text(text, encoding="utf-8")
        args = [f"--vsh={target}", f"--psh={psh}"] if psh else [f"--vsh={vsh}", f"--psh={target}"]
        r = subprocess.run([assembler, "assemble", *args, str(Path(tmp) / "probe.gsh")], capture_output=True, text=True)
        if r.returncode == 0:
            return target
        m = re.search(r"(\d+):\d+ Incorrect clause (count|addr) \d+, expected (\d+)", r.stdout + r.stderr)
        assert m, r.stdout + r.stderr
        lines = text.split("\n")
        for i in range(int(m[1]) - 1, -1, -1):
            if re.match(r"\d\d (ALU|TEX):", lines[i]):
                key = "CNT" if m[2] == "count" else "ADDR"
                lines[i] = re.sub(key + r"\(\d+\)", f"{key}({m[3]})", lines[i])
                break
        text = "\n".join(lines)
    raise RuntimeError("clauses non corrigées")


def main():
    assembler, out = str(Path(sys.argv[1]).resolve()), Path(sys.argv[2])
    debug = "--debug-colors" in sys.argv  # chaque programme peint une couleur unique
    with tempfile.TemporaryDirectory() as tmp:
        # --psh=<fichier> : shader de pixels de diagnostic pour tous les programmes.
        psh = next((Path(a[6:]) for a in sys.argv if a.startswith("--psh=")), HERE / "lyt.psh")
        header, program, end = assemble(assembler, tmp, psh)
        frame = assemble(assembler, tmp, psh, HERE / "lyt_frame.vsh")[1]
        programs = [frame if i in FRAME_PROGRAMS else program for i in range(PROGRAM_COUNT)]
        # Variante 1 (programmes 15 à 29, 0x034614F0) : panes blancs et opaques, le jeu n'envoie
        # pas uColor (0x0346151C) ; la couleur de sommet vaut 1 au lieu de couleur × uColor (sinon
        # la valeur laissée par le dessin précédent s'appliquait : texte qui clignotait).
        vsh_text = (HERE / "lyt.vsh").read_text(encoding="utf-8")
        white = vsh_text.replace("03 EXP_DONE: POS0", "  99  x: MOV R5.x, 1.0f\n      y: MOV R5.y, 1.0f\n"
                                 "      z: MOV R5.z, 1.0f\n      w: MOV R5.w, 1.0f\n03 EXP_DONE: POS0", 1)
        white_vsh = Path(tmp) / "white.vsh"
        white_vsh.write_text(fix_clauses(assembler, tmp, None, white, psh).read_text(encoding="utf-8"), encoding="utf-8")
        for i in range(15, PROGRAM_COUNT):
            programs[i] = assemble(assembler, tmp, psh, white_vsh)[1]
        if psh == HERE / "lyt.psh":
            # Programme = indice de base du matériau (0x0345F8F4) + 15 × variante (0x034614F0).
            # Bases 0-2 (aucune ou une texture) : le jeu n'envoie uTextureMode que si le
            # programme le demande, la valeur peut donc être celle d'un matériau précédent ; ces
            # programmes ignorent le mode, et la base 1 (sans texture) prend une texture blanche.
            # --tag-programs : constante inutilisée R20.w = 100 + numéro, pour reconnaître le
            # programme d'un dessin dans RenderDoc.
            source = psh.read_text(encoding="utf-8")
            tag = "--tag-programs" in sys.argv
            for i in range(PROGRAM_COUNT):
                base = i % 15
                if base > 2 and not tag:
                    continue
                text = source
                if base <= 2:
                    text = text.replace("z: SETGT_UINT R4.x, 0x10000, C0.x", "z: MOV R4.x, 0x0")
                if base == 1:
                    text = text.replace("SAMPLE          R9, R1.xy0x, t0, s0", "SAMPLE          R21, R1.xy0x, t0, s0")
                    text = text.replace("z: MOV R20.z, 1.0f", "z: MOV R20.z, 1.0f\n   99  x: MOV R9.x, 1.0f\n"
                                        "      y: MOV R9.y, 1.0f\n      z: MOV R9.z, 1.0f\n      w: MOV R9.w, 1.0f")
                    text = text.replace("NUM_GPRS = 21", "NUM_GPRS = 22")
                if tag:
                    text = text.replace("z: MOV R20.z, 1.0f", f"z: MOV R20.z, 1.0f\n      w: MOV R20.w, {i + 100}.0f")
                fixed = fix_clauses(assembler, tmp, HERE / "lyt.vsh", text)
                vsh = white_vsh if i >= 15 else HERE / ("lyt_frame.vsh" if i in FRAME_PROGRAMS else "lyt.vsh")
                programs[i] = assemble(assembler, tmp, fixed, vsh)[1]
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
