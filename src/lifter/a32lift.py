"""Prototype static recompiler: ARM A32 (Pikmin 3 Deluxe `main`, converted to ELF by nx2elf) -> C.

Same model as ssbu-recomp's AArch64 recomp.py: every guest function becomes `void f_<addr>(Ctx *c)` working on a
register context (src/lifter/a32rt.h), one label per instruction, conditional execution as `if (cond) { ... }`.
Decoding is done by hand (ARM ARM, A5); Capstone is only used for the disassembly comments.
Supported (prototype): data processing (all 16 ops, immediate / imm-shift / reg-shift operands, S forms), MOVW/MOVT,
MUL/MLA/[SU]MULL/[SU]MLAL, CLZ, [SU]XT[BH](A), [SU]BFX, BFI/BFC, LDR/STR/LDRB/STRB (all addressing modes but T forms),
LDRH/STRH/LDRSB/LDRSH/LDRD/STRD, LDM/STM (no user-mode forms), B inside the function, BX LR / MOV PC, LR / pops of PC
as returns, NOP. Anything else raises Unimpl.

Usage:
    python a32lift.py leaves <out.json> [count]     pick small leaf functions the prototype can lift
    python a32lift.py lift <out.c> <addr>[,<addr>...]
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
ELF = os.path.join(ROOT, 'work', 'elf', 'main.elf')
GHIDRA_BASE = 0x60000000          # notes/functions.tsv addresses are at Ghidra's image base

IMG = open(ELF, 'rb').read()


def segments():
    phoff, = struct.unpack_from('<I', IMG, 0x1c)
    phnum, = struct.unpack_from('<H', IMG, 0x2c)
    out = []
    for i in range(phnum):
        t, off, va, _pa, fs, ms, fl, _al = struct.unpack_from('<8I', IMG, phoff + i * 32)
        if t == 1:
            out.append((va, off, fs, ms, fl))
    return out


SEGS = segments()
TEXT_VA, TEXT_OFF, TEXT_SIZE = SEGS[0][0], SEGS[0][1], SEGS[0][2]


def _va2off(va):
    for sva, off, fs, _ms, _fl in SEGS:
        if sva <= va < sva + fs:
            return off + va - sva
    raise KeyError(hex(va))


def _dynamic():
    phoff, = struct.unpack_from('<I', IMG, 0x1c)
    phnum, = struct.unpack_from('<H', IMG, 0x2c)
    for i in range(phnum):
        t, off, _va, _pa, fs, _ms, _fl, _al = struct.unpack_from('<8I', IMG, phoff + i * 32)
        if t == 2:
            tags = {}
            for k in range(0, fs, 8):
                tag, val = struct.unpack_from('<iI', IMG, off + k)
                if tag == 0:
                    break
                tags.setdefault(tag, val)
            return tags
    return {}


def _plt():
    """PLT stub address -> (jump slot index, symbol name). Stub: add ip, pc, #a; add ip, ip, #b; ldr pc, [ip, #c]!"""
    tags = _dynamic()
    slot_names = {}
    for k in range(0, tags[2], 8):
        o, info = struct.unpack_from('<II', IMG, _va2off(tags[23]) + k)
        nameoff, = struct.unpack_from('<I', IMG, _va2off(tags[6]) + (info >> 8) * 16)
        st = _va2off(tags[5]) + nameoff
        slot_names[o] = (k // 8, IMG[st:IMG.index(b'\0', st)].decode('latin-1'))

    def rot(w):
        r, i = ((w >> 8) & 15) * 2, w & 0xff
        return ((i >> r) | (i << (32 - r))) & 0xffffffff if r else i
    out = {}
    for a in range(TEXT_VA, TEXT_VA + TEXT_SIZE - 8, 4):
        w0, w1, w2 = (struct.unpack_from('<I', IMG, TEXT_OFF + a + 4 * k - TEXT_VA)[0] for k in range(3))
        if (w0 & 0xfffff000) == 0xe28fc000 and (w1 & 0xfffff000) == 0xe28cc000 and (w2 & 0xfffff000) == 0xe5bcf000:
            slot = (a + 8 + rot(w0) + rot(w1) + (w2 & 0xfff)) & 0xffffffff
            if slot in slot_names:
                out[a] = slot_names[slot]
    return out


PLT = _plt()


def word(addr):
    return struct.unpack_from('<I', IMG, TEXT_OFF + addr - TEXT_VA)[0]


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


class Unimpl(Exception):
    pass


CONDS = ['C_EQ', 'C_NE', 'C_CS', 'C_CC', 'C_MI', 'C_PL', 'C_VS', 'C_VC', 'C_HI', 'C_LS', 'C_GE', 'C_LT', 'C_GT', 'C_LE']


class Fn:
    def __init__(self, entry, end):
        self.entry, self.end = entry, end
        self.callees = set()          # direct call / tail call targets (not PLT)
        self.imports = set()

    def call(self, target, pc_after):
        """C statement calling target (a lifted function or an import), with LR = return address"""
        if target in PLT:
            idx, name = PLT[target]
            self.imports.add(target)
            return 'c->r[14] = 0x%xu; imp_call(c, %d); /* %s */' % (pc_after, idx, name)
        self.callees.add(target)
        return 'c->r[14] = 0x%xu; f_%x(c);' % (pc_after, target)

    def tail(self, target):
        if target in PLT:
            idx, name = PLT[target]
            self.imports.add(target)
            return 'imp_call(c, %d); return; /* %s */' % (idx, name)
        self.callees.add(target)
        return 'f_%x(c); return;' % target

    def R(self, n, pc):
        """value of register n as an operand (PC reads as the instruction address + 8)"""
        return '0x%xu' % ((pc + 8) & 0xffffffff) if n == 15 else 'c->r[%d]' % n

    def ret_or_unimpl(self, what):
        raise Unimpl(what)

    # ---- operand 2 of data processing: returns (C expression setup, value name)
    def operand2(self, w, pc):
        if bits(w, 25, 25):
            rot, imm = bits(w, 11, 8) * 2, bits(w, 7, 0)
            v = ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff if rot else imm
            co = 'c->C' if rot == 0 else str(v >> 31)
            return 'uint32_t co = %s, op2 = 0x%xu;' % (co, v)
        rm, typ = bits(w, 3, 0), bits(w, 6, 5)
        if bits(w, 4, 4) == 0:
            return 'uint32_t co, op2 = sh_imm(c, %s, %d, %d, &co);' % (self.R(rm, pc), typ, bits(w, 11, 7))
        if bits(w, 7, 7):
            raise Unimpl('dp encoding')
        rs = bits(w, 11, 8)
        if 15 in (rm, rs, bits(w, 19, 16), bits(w, 15, 12)):
            raise Unimpl('pc in register-shifted operand')
        return 'uint32_t co, op2 = sh_reg(c, c->r[%d], %d, c->r[%d], &co);' % (rm, typ, rs)

    def dataproc(self, w, pc):
        op, s, rn, rd = bits(w, 24, 21), bits(w, 20, 20), bits(w, 19, 16), bits(w, 15, 12)
        if 8 <= op <= 11 and not s:
            raise Unimpl('misc (mrs/msr/...)')
        if rd == 15 and op not in (8, 9, 10, 11):
            if op == 13 and not s and not bits(w, 25, 25) and bits(w, 11, 0) == 14:     # mov pc, lr
                return 'return;'
            raise Unimpl('dp writing pc')
        setup = self.operand2(w, pc)
        a = self.R(rn, pc)
        if op in (0, 1, 8, 9, 12, 13, 14, 15):                       # logical
            e = {0: '%s & op2' % a, 1: '%s ^ op2' % a, 8: '%s & op2' % a, 9: '%s ^ op2' % a, 12: '%s | op2' % a,
                 13: 'op2', 14: '%s & ~op2' % a, 15: '~op2'}[op]
            if op in (8, 9):
                return '{ %s nz(c, %s, co); }' % (setup, e)
            if s:
                return '{ %s c->r[%d] = nz(c, %s, co); }' % (setup, rd, e)
            return '{ %s (void)co; c->r[%d] = %s; }' % (setup, rd, e)
        args = {2: (a, '~op2', '1'), 3: ('op2', '~%s' % a, '1'), 4: (a, 'op2', '0'), 5: (a, 'op2', 'c->C'),
                6: (a, '~op2', 'c->C'), 7: ('op2', '~%s' % a, 'c->C'), 10: (a, '~op2', '1'), 11: (a, 'op2', '0')}[op]
        call = 'awc(c, %s, %s, %s, %d)' % (args[0], args[1], args[2], 1 if s else 0)
        if op in (10, 11):
            return '{ %s (void)co; %s; }' % (setup, call)
        return '{ %s (void)co; c->r[%d] = %s; }' % (setup, rd, call)

    # ---- single data transfer (LDR/STR/LDRB/STRB)
    def ldst(self, w, pc):
        i, p, u, b, wb, l = (bits(w, k, k) for k in (25, 24, 23, 22, 21, 20))
        rn, rd = bits(w, 19, 16), bits(w, 15, 12)
        if not p and wb:
            raise Unimpl('ldrt/strt')
        if i:
            rm = bits(w, 3, 0)
            if rm == 15:
                raise Unimpl('pc offset register')
            off = 'sh_imm(c, c->r[%d], %d, %d, &co)' % (rm, bits(w, 6, 5), bits(w, 11, 7))
        else:
            off = '0x%xu' % bits(w, 11, 0)
        writeback = (not p) or wb
        if rn == 15 and writeback:
            raise Unimpl('pc writeback')
        base = self.R(rn, pc)
        oa = '%s %s %s' % (base, '+' if u else '-', off)
        addr = 'oa' if p else base
        out = ['{ uint32_t co; (void)co; uint32_t oa = %s, a = %s;' % (oa, addr)]
        if l:
            out.append('uint32_t v = %s(a);' % ('rd8' if b else 'rd32'))
            if writeback:
                out.append('c->r[%d] = oa;' % rn)
            if rd == 15:
                if rn == 13 and not p and u and not i and bits(w, 11, 0) == 4 and not b:     # pop {pc}
                    out.append('(void)v; return; }')
                    return ' '.join(out)
                raise Unimpl('load to pc')
            out.append('c->r[%d] = v; }' % rd)
        else:
            if rd == 15:
                raise Unimpl('store pc')
            out.append('%s(a, c->r[%d]);' % ('wr8' if b else 'wr32', rd))
            if writeback:
                out.append('c->r[%d] = oa;' % rn)
            out.append('}')
        return ' '.join(out)

    # ---- extra load/store (halfword, signed byte, doubleword)
    def ldst_extra(self, w, pc):
        p, u, imm, wb, l = (bits(w, k, k) for k in (24, 23, 22, 21, 20))
        rn, rt, op = bits(w, 19, 16), bits(w, 15, 12), bits(w, 6, 5)
        if not p and wb:
            raise Unimpl('ldrht etc.')
        off = '0x%xu' % ((bits(w, 11, 8) << 4) | bits(w, 3, 0)) if imm else 'c->r[%d]' % bits(w, 3, 0)
        if not imm and bits(w, 3, 0) == 15:
            raise Unimpl('pc offset register')
        writeback = (not p) or wb
        if rn == 15 and writeback:
            raise Unimpl('pc writeback')
        if rt == 15:
            raise Unimpl('pc transfer')
        oa = '%s %s %s' % (self.R(rn, pc), '+' if u else '-', off)
        out = ['{ uint32_t oa = %s, a = %s;' % (oa, 'oa' if p else self.R(rn, pc))]
        if op == 1 and l:
            out.append('uint32_t v = rd16(a);')
        elif op == 2 and l:
            out.append('uint32_t v = (uint32_t)(int32_t)(int8_t)rd8(a);')
        elif op == 3 and l:
            out.append('uint32_t v = (uint32_t)(int32_t)(int16_t)rd16(a);')
        elif op == 1:
            out.append('wr16(a, c->r[%d]);' % rt)
        elif op == 2:                                                 # LDRD
            if rt & 1 or rt == 14:
                raise Unimpl('ldrd odd register')
            out.append('uint32_t v = rd32(a), v2 = rd32(a + 4);')
        else:                                                         # STRD
            if rt & 1 or rt == 14:
                raise Unimpl('strd odd register')
            out.append('wr32(a, c->r[%d]); wr32(a + 4, c->r[%d]);' % (rt, rt + 1))
        if writeback:
            out.append('c->r[%d] = oa;' % rn)
        if l or op == 2:
            out.append('c->r[%d] = v;' % rt)
            if op == 2 and not l:
                out.append('c->r[%d] = v2;' % (rt + 1))
        out.append('}')
        return ' '.join(out)

    # ---- LDM / STM
    def ldm_stm(self, w, pc):
        p, u, s, wb, l, rn = bits(w, 24, 24), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21), bits(w, 20, 20), bits(w, 19, 16)
        regs = [k for k in range(16) if w >> k & 1]
        if s or rn == 15 or not regs:
            raise Unimpl('ldm/stm user / pc base')
        n = len(regs)
        start = {(0, 1): 'c->r[%d]' % rn, (1, 1): 'c->r[%d] + 4' % rn,
                 (0, 0): 'c->r[%d] - %d' % (rn, 4 * n - 4), (1, 0): 'c->r[%d] - %d' % (rn, 4 * n)}[(p, u)]
        nb = 'c->r[%d] %s %d' % (rn, '+' if u else '-', 4 * n)
        out = ['{ uint32_t a = %s, nb = %s;' % (start, nb)]
        if l:
            for k, r in enumerate(regs):
                out.append('uint32_t v%d = rd32(a + %d);' % (k, 4 * k))
            if wb:
                out.append('c->r[%d] = nb;' % rn)
            for k, r in enumerate(regs):
                if r != 15:
                    out.append('c->r[%d] = v%d;' % (r, k))
            if 15 in regs:
                out.append('(void)v%d; return;' % (n - 1))
        else:
            if 15 in regs:
                raise Unimpl('stm pc')
            for k, r in enumerate(regs):
                out.append('wr32(a + %d, c->r[%d]);' % (4 * k, r))
            if wb:
                out.append('c->r[%d] = nb;' % rn)
        out.append('(void)nb; }')
        return ' '.join(out)

    # ---- ARMv8 A32 load-acquire / store-release and (load / store) exclusive: LDA*, STL*, LDAEX*, STLEX*, LDREX*, STREX*
    def excl(self, w, pc):
        size, load, rn, rt, kind, rs = bits(w, 22, 21), bits(w, 20, 20), bits(w, 19, 16), bits(w, 15, 12), bits(w, 9, 8), bits(w, 3, 0)
        if kind == 1:
            raise Unimpl('excl kind 01')
        if (load and rs != 15) or (not load and kind == 0 and rt != 15):
            raise Unimpl('excl encoding')                  # loads have Rt2 = 1111, STL has Rd = 1111
        ex = kind != 0                                   # 00 LDA/STL, 10 LDAEX/STLEX, 11 LDREX/STREX (single core: same code)
        if 15 in (rn, rt if load else rs) or (not load and ex and rt == 15):
            raise Unimpl('excl pc')
        rd_, wr_ = {0: ('rd32', 'wr32'), 2: ('rd8', 'wr8'), 3: ('rd16', 'wr16'), 1: (None, None)}[size]
        if size == 1 and not ex:
            raise Unimpl('lda/stl doubleword')
        if size == 1 and ((rt if load else rs) & 1 or (rt if load else rs) == 14):
            raise Unimpl('exclusive doubleword register')
        a = 'c->r[%d]' % rn
        if load:
            if size == 1:
                out = '{ uint32_t a = %s, v = rd32(a), v2 = rd32(a + 4); c->r[%d] = v; c->r[%d] = v2;' % (a, rt, rt + 1)
                return out + (' c->exon = 1; c->exa = a; c->exv = (uint64_t)v2 << 32 | v; }' if ex else ' }')
            out = '{ uint32_t a = %s, v = %s(a); c->r[%d] = v;' % (a, rd_, rt)
            return out + (' c->exon = 1; c->exa = a; c->exv = v; }' if ex else ' }')
        if not ex:
            return '%s(%s, c->r[%d]);' % (wr_, a, rs)
        if size == 1:
            cur, store = '(uint64_t)rd32(a + 4) << 32 | rd32(a)', 'wr32(a, c->r[%d]); wr32(a + 4, c->r[%d]);' % (rs, rs + 1)
        else:
            cur, store = '(uint64_t)%s(a)' % rd_, '%s(a, c->r[%d]);' % (wr_, rs)
        return ('{ uint32_t a = %s; if (c->exon && c->exa == a && (%s) == c->exv) { %s c->r[%d] = 0; } else c->r[%d] = 1; c->exon = 0; }'
                % (a, cur, store, rt, rt))

    def media(self, w, pc):
        rd, rm = bits(w, 15, 12), bits(w, 3, 0)
        if 15 in (rd, rm):
            raise Unimpl('media pc')
        ext = {0x06af0070: ('(uint32_t)(int32_t)(int8_t)', 'sxtb'), 0x06bf0070: ('(uint32_t)(int32_t)(int16_t)', 'sxth'),
               0x06ef0070: ('(uint32_t)(uint8_t)', 'uxtb'), 0x06ff0070: ('(uint32_t)(uint16_t)', 'uxth')}
        key = (w & 0x0ff003f0) | 0x000f0000
        if key in ext:
            rot = bits(w, 11, 10) * 8
            v = 'c->r[%d]' % rm if rot == 0 else '((c->r[%d] >> %d) | (c->r[%d] << %d))' % (rm, rot, rm, 32 - rot)
            e = '%s(%s)' % (ext[key][0], v)
            rn = bits(w, 19, 16)
            return 'c->r[%d] = %s;' % (rd, e if rn == 15 else 'c->r[%d] + %s' % (rn, e))
        if (w & 0x0fa00070) == 0x07a00050:                              # SBFX / UBFX
            width, lsb = bits(w, 20, 16) + 1, bits(w, 11, 7)
            if lsb + width > 32:
                raise Unimpl('bfx range')
            if bits(w, 22, 22):
                return 'c->r[%d] = (c->r[%d] >> %d) & 0x%xu;' % (rd, rm, lsb, (1 << width) - 1)
            return 'c->r[%d] = (uint32_t)((int32_t)(c->r[%d] << %d) >> %d);' % (rd, rm, 32 - lsb - width, 32 - width)
        if (w & 0x0fe00070) == 0x07c00010:                              # BFI / BFC
            msb, lsb = bits(w, 20, 16), bits(w, 11, 7)
            if msb < lsb:
                raise Unimpl('bfi range')
            mask = ((1 << (msb - lsb + 1)) - 1) << lsb
            src = '0u' if rm == 15 else '(c->r[%d] << %d)' % (rm, lsb)
            return 'c->r[%d] = (c->r[%d] & 0x%xu) | (%s & 0x%xu);' % (rd, rd, ~mask & 0xffffffff, src, mask)
        raise Unimpl('media')

    def jump_table(self, w, pc):
        ts = jt_targets(w, pc)
        if any(not (self.entry <= t < self.end) for t in ts):
            raise Unimpl('jump table target outside the function')
        cases = ' '.join('case 0x%xu: goto L_%x;' % (t, t) for t in sorted(set(ts)))
        return ('{ switch (c->r[%d] + c->r[%d]) { %s default: guest_unimpl(c, 0x%xu, 0x%08xu); } }'
                % (bits(w, 19, 16), w & 15, cases, pc, w))

    def lift(self, w, pc):
        cond = w >> 28
        if cond == 15:
            if (w & 0xfffffff0) in (0xf57ff040, 0xf57ff050, 0xf57ff060):
                return ';   /* dsb / dmb / isb: single host thread per guest thread, nothing to order here */'
            if w == 0xf57ff01f:
                return 'c->exon = 0;'                                     # clrex
            if (w & 0xff70f000) == 0xf550f000 or (w & 0xff70f010) == 0xf750f000:
                return ';   /* pld */'
            raise Unimpl('unconditional space')
        stmt = self.lift_body(w, pc)
        if cond == 14:
            return stmt
        return 'if (%s) { %s }' % (CONDS[cond], stmt)

    def lift_body(self, w, pc):
        if (w & 0xfff0fff0) == 0xe080f000 and jt_targets(w, pc):        # add pc, rB, rO: clang jump table
            return self.jump_table(w, pc)
        if w & 0x0fffffff == 0x0320f000:
            return ';'                                                   # nop
        if (w & 0x0ffffff0) == 0x012fff10:                               # bx
            if bits(w, 3, 0) == 14:
                return 'return;'
            if bits(w, 3, 0) == 15:
                raise Unimpl('bx pc')
            return 'call_indirect(c, c->r[%d]); return;' % bits(w, 3, 0)
        if (w & 0x0ffffff0) == 0x012fff30:
            if bits(w, 3, 0) == 15:
                raise Unimpl('blx pc')
            return '{ uint32_t t = c->r[%d]; c->r[14] = 0x%xu; call_indirect(c, t); }' % (bits(w, 3, 0), pc + 4)
        if (w & 0x0fff0ff0) == 0x016f0f10:
            return 'c->r[%d] = clz32(c->r[%d]);' % (bits(w, 15, 12), bits(w, 3, 0))
        if (w & 0x0ff00000) == 0x03000000:
            return 'c->r[%d] = 0x%xu;' % (bits(w, 15, 12), (bits(w, 19, 16) << 12) | bits(w, 11, 0))
        if (w & 0x0ff00000) == 0x03400000:
            return 'c->r[%d] = (c->r[%d] & 0xffffu) | 0x%xu;' % (bits(w, 15, 12), bits(w, 15, 12), ((bits(w, 19, 16) << 12) | bits(w, 11, 0)) << 16)
        if (w & 0x0fc000f0) == 0x00000090:                               # MUL / MLA
            rd, ra, rs, rm, s, acc = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0), bits(w, 20, 20), bits(w, 21, 21)
            e = 'c->r[%d] * c->r[%d]%s' % (rm, rs, ' + c->r[%d]' % ra if acc else '')
            return ('c->r[%d] = %s;' % (rd, e)) + (' c->N = c->r[%d] >> 31; c->Z = c->r[%d] == 0;' % (rd, rd) if s else '')
        if (w & 0x0f8000f0) == 0x00800090:                               # [SU]MULL / [SU]MLAL
            hi, lo, rs, rm, sg, acc, s = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0), bits(w, 22, 22), bits(w, 21, 21), bits(w, 20, 20)
            prod = '(uint64_t)((int64_t)(int32_t)c->r[%d] * (int32_t)c->r[%d])' % (rm, rs) if sg else '((uint64_t)c->r[%d] * c->r[%d])' % (rm, rs)
            accv = ' + (((uint64_t)c->r[%d] << 32) | c->r[%d])' % (hi, lo) if acc else ''
            out = '{ uint64_t p = %s%s; c->r[%d] = (uint32_t)p; c->r[%d] = (uint32_t)(p >> 32);' % (prod, accv, lo, hi)
            if s:
                out += ' c->N = (uint32_t)(p >> 63); c->Z = p == 0;'
            return out + ' }'
        top = bits(w, 27, 25)
        if top in (0, 1):
            if top == 0 and bits(w, 7, 7) and bits(w, 4, 4):
                if bits(w, 6, 5) == 0:
                    if (w & 0x0f8000f0) == 0x01800090 and bits(w, 11, 10) == 3:
                        return self.excl(w, pc)
                    raise Unimpl('swp/...')
                return self.ldst_extra(w, pc)
            return self.dataproc(w, pc)
        if top in (2, 3):
            if top == 3 and bits(w, 4, 4):
                return self.media(w, pc)
            return self.ldst(w, pc)
        if top == 4:
            return self.ldm_stm(w, pc)
        if top == 5:
            target = branch_target(w, pc)
            if bits(w, 24, 24):
                return self.call(target, pc + 4)
            if self.entry <= target < self.end:
                return 'goto L_%x;' % target
            return self.tail(target)
        raise Unimpl('coprocessor / vfp / neon / svc')


def branch_target(w, pc):
    imm = bits(w, 23, 0)
    if imm & 0x800000:
        imm -= 1 << 24
    return (pc + 8 + imm * 4) & 0xffffffff


def _writes(p):
    """destination register of a plain data-processing / load instruction p (None if none or unknown)"""
    if (p >> 28) == 15:
        return None
    if bits(p, 27, 26) in (0, 1) and not (bits(p, 27, 26) == 0 and 8 <= bits(p, 24, 21) <= 11 and bits(p, 20, 20)):
        return bits(p, 15, 12)
    return None


def jt_targets(w, pc):
    """Targets of a clang jump table ending at `add pc, rB, rO` (pc), or None:
           cmp   rI, #K            (somewhere before; usually followed by bhi default)
           add   rB, pc, #imm      rB = table = pc + 4 (the words right after the add pc)
           ldr   rO, [rB, rI, lsl #2]
           add   pc, rB, rO        target = table + offset[rI]
           .word offset[0] ... offset[K]      (relative to the table, in the read-only code: known when lifting)
       The scheduler may interleave other instructions; rB / rO must not be rewritten in between. Number of entries:
       the table ends exactly where its first target begins (offsets are positive and the cases follow the table);
       when that does not hold, K + 1 from the `cmp rI, #K` found before the ldr."""
    if (w & 0xfff0fff0) != 0xe080f000:
        return None
    rb, ro = bits(w, 19, 16), bits(w, 3, 0)
    table = pc + 4
    ld_at = add_at = None
    for back in range(1, 8):
        a = pc - 4 * back
        p = word(a)
        if ld_at is None:
            if (p & 0xfff00ff0) == 0xe7900100 and bits(p, 19, 16) == rb and bits(p, 15, 12) == ro:
                ld_at = a
                continue
            if _writes(p) in (rb, ro):
                return None
        else:
            if (p & 0xffff0f00) == 0xe28f0000 and bits(p, 15, 12) == rb:
                rot, imm = bits(p, 11, 8) * 2, bits(p, 7, 0)
                v = ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff if rot else imm
                if a + 8 + v != table:
                    return None
                add_at = a
                break
            if _writes(p) == rb:
                return None
    if ld_at is None or add_at is None:
        return None
    ri = bits(word(ld_at), 3, 0)
    # 1. exact end: the first target starts right after the table
    n, first = 0, 1 << 32
    while 4 * n < first and n < 4096:
        off = word(table + 4 * n)
        if off % 4 or off < 4 * (n + 1) or off >= 0x40000:
            break
        first = min(first, off)
        n += 1
    if n and 4 * n == first:
        return [(table + word(table + 4 * i)) & 0xffffffff for i in range(n)]
    # 2. cmp rI, #K before the ldr (a conditional cmp is accepted; conditional writes of rI on the exit path too)
    for back in range(1, 14):
        p = word(ld_at - 4 * back)
        if (p & 0x0ff0f000) == 0x03500000 and bits(p, 19, 16) == ri and (p >> 28) != 15:
            rot, imm = bits(p, 11, 8) * 2, bits(p, 7, 0)
            k = ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff if rot else imm
            if k >= 4096:
                return None
            return [(table + word(table + 4 * i)) & 0xffffffff for i in range(k + 1)]
        if _writes(p) == ri and (p >> 28) == 14:
            return None
    return None


def flow(w, pc, entry, end):
    """(falls through, internal branch targets) of one instruction, for the reachability walk"""
    cond = w >> 28
    always = cond == 14
    ends = False
    targets = []
    if jt_targets(w, pc):                                     # jump table
        targets.extend(t for t in jt_targets(w, pc) if entry <= t < end)
        ends = True
    elif (w & 0x0ffffff0) == 0x012fff10:                       # bx (return or indirect tail jump)
        ends = True
    elif bits(w, 27, 25) == 5 and not bits(w, 24, 24) and cond != 15:
        t = branch_target(w, pc)
        if entry <= t < end:
            targets.append(t)
        ends = True
    elif bits(w, 27, 25) == 4 and bits(w, 20, 20) and w >> 15 & 1:   # ldm with pc
        ends = True
    elif bits(w, 27, 26) == 1 and bits(w, 20, 20) and bits(w, 15, 12) == 15:   # ldr pc
        ends = True
    elif (bits(w, 27, 26) == 0 and bits(w, 15, 12) == 15 and not (8 <= bits(w, 24, 21) <= 11)
          and not (bits(w, 7, 7) and bits(w, 4, 4) and not bits(w, 25, 25))):
        ends = True                                             # data processing writing pc (mov pc, lr ...)
    return (not ends) or not always, targets


def reachable(entry, end):
    seen, todo = set(), [entry]
    while todo:
        pc = todo.pop()
        if pc in seen or not (entry <= pc < end):
            continue
        seen.add(pc)
        ft, targets = flow(word(pc), pc, entry, end)
        todo.extend(targets)
        if ft:
            todo.append(pc + 4)
    return sorted(seen)


def disasm(words, addr):
    try:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
        return {i.address: '%s %s' % (i.mnemonic, i.op_str) for i in md.disasm(b''.join(struct.pack('<I', x) for x in words), addr)}
    except ImportError:
        return {}


def lift_function(entry, end, strict=True):
    """C text of one function, lifting only the instructions reachable from entry inside [entry, end).
    Returns (text, Fn, list of (pc, reason) not lifted). strict: raise Unimpl on the first instruction not lifted."""
    fn = Fn(entry, end)
    pcs = reachable(entry, end)
    words = [word(a) for a in range(entry, (pcs[-1] if pcs else entry) + 4, 4)]
    dis = disasm(words, entry)
    lines, bad = ['void f_%x(Ctx *c) {' % entry], []
    for pc in pcs:
        w = word(pc)
        try:
            stmt = fn.lift(w, pc)
        except Unimpl as e:
            if strict:
                raise
            bad.append((pc, str(e)))
            stmt = 'guest_unimpl(c, 0x%xu, 0x%08xu);' % (pc, w)
        lines.append('L_%x: %s   /* %08x  %s */' % (pc, stmt, w, dis.get(pc, '?')))
        if pc + 4 == end and flow(w, pc, entry, end)[0]:
            lines.append('  ' + fn.tail(end))                   # falls into the next function
    lines.append('  return;')
    lines.append('}')
    return '\n'.join(lines), fn, bad


def ghidra_functions():
    out = []
    with open(os.path.join(ROOT, 'notes', 'functions.tsv')) as f:
        next(f)
        for line in f:
            a, size, name, thunk = line.rstrip('\n').split('\t')
            out.append((int(a, 16) - GHIDRA_BASE, int(size), name))
    return out


def cmd_leaves(out_path, count):
    """Small leaf functions (no calls) that the prototype lifts completely; varied instruction mixes first."""
    import collections
    picked, seen_kinds = [], collections.Counter()
    for addr, size, name in ghidra_functions():
        if not (12 <= size <= 160) or addr % 4 or addr + size > TEXT_SIZE:
            continue
        fn = Fn(addr, addr + size)
        try:
            kinds = set()
            for a in range(addr, addr + size, 4):
                w = word(a)
                fn.lift(w, a)
                kinds.add('cond' if w >> 28 != 14 else 'al')
                kinds.add(('dp', 'dp', 'ldst', 'ldst', 'ldm', 'b')[min(bits(w, 27, 25), 5)])
                if (w & 0x0fc000f0) == 0x90 or (w & 0x0f8000f0) == 0x00800090:
                    kinds.add('mul')
        except Unimpl:
            continue
        if word(addr + size - 4) & 0x0fffffff not in (0x012fff1e, 0x08bd8000) and (word(addr + size - 4) & 0x0fff8000) != 0x08bd8000:
            continue                                                     # must end in a return
        key = tuple(sorted(kinds))
        if seen_kinds[key] >= 2:
            continue
        seen_kinds[key] += 1
        picked.append({'addr': addr, 'size': size, 'name': name, 'kinds': sorted(kinds)})
        if len(picked) >= count:
            break
    json.dump(picked, open(out_path, 'w'), indent=1)
    for p in picked:
        print('0x%x %3d %s %s' % (p['addr'], p['size'], p['name'], ','.join(p['kinds'])))


_STARTS = None


def starts():
    """sorted function entries: Ghidra's list plus every BL target (PLT stubs excluded)"""
    global _STARTS
    if _STARTS is None:
        st = {a for a, _s, _n in ghidra_functions() if a % 4 == 0 and a < TEXT_SIZE}
        for a in range(TEXT_VA, TEXT_VA + TEXT_SIZE, 4):
            w = word(a)
            if (w & 0x0f000000) == 0x0b000000 and w >> 28 != 15:
                t = branch_target(w, a)
                if t < TEXT_SIZE:
                    st.add(t)
        _STARTS = sorted(x for x in st if x not in PLT)
    return _STARTS


def extent(entry):
    import bisect
    st = starts()
    i = bisect.bisect_right(st, entry)
    end = st[i] if i < len(st) else TEXT_VA + TEXT_SIZE
    return min(end, min((p for p in PLT if p > entry), default=end))


def closure(roots, limit=40):
    """roots and every function they call directly (recursively); None if one of them cannot be lifted or too many"""
    done, todo = {}, list(roots)
    while todo:
        a = todo.pop()
        if a in done:
            continue
        if len(done) >= limit:
            return None
        try:
            _text, fn, _bad = lift_function(a, extent(a))
        except Unimpl:
            return None
        done[a] = fn
        todo.extend(fn.callees)
    return done


def cmd_lift(out_path, addrs, sizes=None):
    texts = [lift_function(a, extent(a))[0] for a in addrs]
    with open(out_path, 'w') as f:
        f.write('/* generated by src/lifter/a32lift.py from work/elf/main.elf: do not edit */\n#include "a32rt.h"\n\n')
        f.writelines('void f_%x(Ctx *c);\n' % a for a in addrs)
        f.write('\n')
        for t in texts:
            f.write(t + '\n\n')


def cmd_nonleaves(out_path, count, max_bytes=400, limit=30):
    """Small functions with calls whose whole direct-call closure lifts; varied features first (imports, indirect
    calls, tail calls, recursion depth)."""
    picked, seen = [], set()
    for a in starts():
        if a in seen or extent(a) - a > max_bytes:
            continue
        try:
            _t, fn, _b = lift_function(a, extent(a))
        except Unimpl:
            continue
        text = _t
        if not (fn.callees or fn.imports):
            continue
        cl = closure([a], limit)
        if not cl:
            continue
        feats = []
        if fn.imports or any(f.imports for f in cl.values()):
            feats.append('import')
        if 'call_indirect' in text:
            feats.append('indirect')
        if any('return; /*' not in l and ' f_' in l and 'return;' in l.split(' f_', 1)[1] and 'c->r[14]' not in l for l in text.splitlines()):
            feats.append('tail')
        if len(cl) > 2:
            feats.append('deep')
        key = tuple(feats)
        if sum(1 for p in picked if tuple(p['kinds']) == key) >= 3:
            continue
        seen.update(cl)
        picked.append({'addr': a, 'size': extent(a) - a, 'name': 'f_%x' % a, 'kinds': feats, 'closure': sorted(cl)})
        if len(picked) >= count:
            break
    json.dump(picked, open(out_path, 'w'), indent=1)
    for p in picked:
        print('0x%x %3d bytes, closure %2d fns  %s' % (p['addr'], p['size'], len(p['closure']), ','.join(p['kinds']) or '-'))


def feature_of(w, pc):
    """'acq' (LDA*), 'stl' (STL*), 'ldaex' (LDAEX*/STLEX*), 'excl' (LDREX*/STREX*), 'jt' (jump table) or None"""
    if (w & 0x0f8000f0) == 0x01800090 and bits(w, 11, 10) == 3 and w >> 28 != 15:
        kind = bits(w, 9, 8)
        if kind == 0:
            return 'acq' if bits(w, 20, 20) else 'stl'
        return 'ldaex' if kind == 2 else 'excl'
    if jt_targets(w, pc):
        return 'jt'
    return None


def jt_argument(entry, pcs):
    """(register, K) when a jump table of the function is indexed by an argument register r0-r3 compared with
    `cmp rI, #K` in the first 8 instructions, the register not being written before: the test can then drive it"""
    for pc in pcs:
        if pc - entry >= 32:
            break
        p = word(pc)
        if (p & 0xfff0f000) == 0xe3500000 and bits(p, 19, 16) < 4:
            ri = bits(p, 19, 16)
            if any(_writes(word(q)) == ri for q in range(entry, pc, 4)):
                continue
            rot, imm = bits(p, 11, 8) * 2, bits(p, 7, 0)
            k = ((imm >> rot) | (imm << (32 - rot))) & 0xffffffff if rot else imm
            for q in pcs:
                t = jt_targets(word(q), q)
                if t and len(t) == k + 1 and q - pc < 64:
                    return ri, k
    return None


def cmd_select(out_path, feature, count, max_bytes=2000, limit=30):
    """functions using `feature` whose whole direct-call closure lifts"""
    picked = []
    for a in starts():
        end = extent(a)
        if end - a > max_bytes:
            continue
        pcs = reachable(a, end)
        if not any(feature_of(word(pc), pc) == feature for pc in pcs):
            continue
        arg = jt_argument(a, pcs) if feature == 'jt' else None
        if feature == 'jt' and not arg:
            continue
        cl = closure([a], limit)
        if not cl:
            continue
        picked.append({'addr': a, 'size': end - a, 'name': 'f_%x' % a, 'kinds': [feature], 'closure': sorted(cl)})
        if arg:                                           # the test drives the switch index over 0..K+2
            picked[-1]['fix'] = {'reg': arg[0], 'values': list(range(arg[1] + 3))}
        if len(picked) >= count:
            break
    json.dump(picked, open(out_path, 'w'), indent=1)
    for p in picked:
        print('0x%x %4d bytes, closure %2d fns  %s' % (p['addr'], p['size'], len(p['closure']), feature))


if __name__ == '__main__':
    if sys.argv[1] == 'select':
        cmd_select(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 6)
    elif sys.argv[1] == 'nonleaves':
        cmd_nonleaves(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 10)
    elif sys.argv[1] == 'leaves':
        cmd_leaves(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 10)
    elif sys.argv[1] == 'lift':
        sel = json.load(open(sys.argv[3]))
        cmd_lift(sys.argv[2], [p['addr'] for p in sel], [p['size'] for p in sel])
