"""Régénère le code de lyt.psh (combineur de textures de nw::lyt) ; l'en-tête du fichier est gardé.

    python gen_lyt_psh.py <latte_assembler.exe>

Les adresses et tailles de clauses (ADDR/CNT) sont recalculées d'après l'assembleur.
"""
import sys
import tempfile
from pathlib import Path

from make_lyt_gsh import HERE, fix_clauses

groups = []  # liste de clauses : ("ALU", [groupes]) / ("TEX", [lignes])
def alu(*gs): groups.append(["ALU", list(gs)])
def tex(*ls): groups.append(["TEX", list(ls)])
X = "xyz"
def vec(op, dst, *srcs, chans=X):
    out = []
    for c in chans:
        args = []
        for s in srcs:
            neg = s.startswith("-")
            b = s[1:] if neg else s
            args.append(("-" if neg else "") + (b + "." + c if (b.startswith("R") or b.startswith("C")) and "." not in b else b))
        out.append("%s: %s %s.%s, %s" % (c, op, dst, c, ", ".join(args)))
    return out

# --- A : décodage de uTextureMode (C0.x, entier) ---
alu(
 ["x: AND_INT R4.y, C0.x, 0x7F", "y: LSHR_INT R4.w, C0.x, 0x8", "z: SETGT_UINT R4.x, 0x10000, C0.x", "w: AND_INT R12.x, C0.x, 0x80"],
 ["x: AND_INT R4.z, R4.w, 0x7F", "y: AND_INT R12.y, R4.w, 0x80"],
 ["x: AND_INT R4.y, R4.y, R4.x", "y: AND_INT R4.z, R4.z, R4.x", "z: AND_INT R12.x, R12.x, R4.x", "w: AND_INT R12.y, R12.y, R4.x"],
 ["x: SETGE_INT R5.z, R4.y, 0xE", "y: SETGE_INT R5.w, R4.z, 0xE"],
 ["x: MOV R20.x, 0.5f", "y: MOV R20.y, 2.0f", "z: MOV R20.z, 1.0f"],
 # nw::lyt donne v croissant vers le haut de l'écran (v = 1 - t par uTexMtx), les textures sont
 # rangées ligne 0 en haut : v est retourné pour les trois jeux, avant le décalage indirect.
 # (l'unité ALU suit le canal écrit : deux .y au plus par groupe, le second en transcendant)
 ["y: ADD R1.y, -R1.y, 1.0f", "t: ADD R2.y, -R2.y, 1.0f"],
 ["y: ADD R3.y, -R3.y, 1.0f"],
)
tex("SAMPLE          R6, R2.xy0x, t1, s1", "SAMPLE          R7, R3.xy0x, t2, s2")
# --- B : indirection (étage 0 : texture 1, étage 1 : texture 2), décalage de uv0 ---
alu(
 vec("CNDE_INT", "R8", "R5.z", "R7", "R6") + ["w: MOV R8.w, 1.0f"],
 ["x: DOT4 R11.x, R8.x, C3.x", "y: DOT4 ____, R8.y, C3.y", "z: DOT4 ____, R8.z, C3.z", "w: DOT4 ____, R8.w, C3.w"],
 ["x: DOT4 ____, R8.x, C4.x", "y: DOT4 R11.y, R8.y, C4.y", "z: DOT4 ____, R8.z, C4.z", "w: DOT4 ____, R8.w, C4.w"],
 ["z: OR_INT R11.z, R5.z, R5.w"],
 ["x: CNDE_INT R11.x, R11.z, 0.0f, R11.x", "y: CNDE_INT R11.y, R11.z, 0.0f, R11.y"],
 ["x: ADD R1.x, R1.x, R11.x", "y: ADD R1.y, R1.y, R11.y"],
)
tex("SAMPLE          R9, R1.xy0x, t0, s0")
# --- C : combinaison, étage 0 (R6, code R4.y, alpha R12.x) puis étage 1 (R7, R4.z, R12.y) ---
def stage(T, m, af):
    gs = []
    gs.append(["x: SETE_INT R10.x, %s, 0x3" % m, "y: SETE_INT R10.y, %s, 0x4" % m, "z: SETE_INT R10.z, %s, 0x5" % m, "w: SETE_INT R10.w, %s, 0x6" % m])
    gs.append(vec("MUL", "R13", "R9", T) + ["w: MUL R13.w, R9.w, %s.w" % T])     # Modulate
    gs.append(vec("ADD", "R14", "R9", T) + ["w: SETE_INT R11.x, %s, 0x9" % m])  # Add
    gs.append(vec("ADD", "R15", "R9", "-" + T))                                 # Subtract
    gs.append(vec("ADD", "R16", "R14", "-R20.x"))                               # AddSigned
    gs.append(vec("ADD", "R17", "R14", "-R13"))                                 # Overlay : 2(P+T-PT)-1 si P>=0,5
    gs.append(vec("MULADD", "R17", "R17", "R20.y", "-R20.z"))
    gs.append(vec("ADD", "R18", "R13", "R13"))                                  #           2PT sinon
    gs.append(vec("ADD", "R19", "R9", "-R20.x"))
    gs.append(vec("CNDGE", "R17", "R19", "R17", "R18"))
    gs.append(vec("CNDE_INT", "R9", "R10.x", "R9", "R13") + ["w: CNDE_INT R9.w, %s, R9.w, R13.w" % af])
    gs.append(vec("CNDE_INT", "R9", "R10.y", "R9", "R14"))
    gs.append(vec("CNDE_INT", "R9", "R10.z", "R9", "R15"))
    gs.append(vec("CNDE_INT", "R9", "R10.w", "R9", "R16"))
    gs.append(vec("CNDE_INT", "R9", "R11.x", "R9", "R17"))
    return gs
alu(*stage("R6", "R4.y", "R12.x"))
alu(*stage("R7", "R4.z", "R12.y"),
 vec("MULADD", "R9", "R9", "C1", "C2", chans="xyzw"),
 vec("MUL", "R0", "R0", "R9", chans="xyzw"))

n = 0; cf = 0; lines = []
for kind, body in groups:
    lines.append("%02d %s: ADDR(32) CNT(1)" % (cf, kind)); cf += 1
    for g in body:
        if kind == "ALU":
            lines.append("  %2d  %s" % (n, g[0]))
            for x in g[1:]:
                lines.append("      %s" % x)
        else:
            lines.append("  %2d  %s" % (n, g))
        n += 1
lines.append("%02d EXP_DONE: PIX0, R0.xyzw" % cf)
lines.append("END_OF_PROGRAM")

psh = HERE / "lyt.psh"
text = psh.read_text(encoding="utf-8")
text = text[:text.index("00 ALU:")] + "\n".join(lines) + "\n"
with tempfile.TemporaryDirectory() as tmp:
    fixed = fix_clauses(str(Path(sys.argv[1]).resolve()), tmp, HERE / "lyt.vsh", text)
    psh.write_text(fixed.read_text(encoding="utf-8"), encoding="utf-8")
print(psh)
