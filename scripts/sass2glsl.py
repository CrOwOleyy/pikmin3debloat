#!/usr/bin/env python3
"""Prototype: translate envydis (gm107) output of a straight-line Maxwell graphics shader to GLSL 450
(Vulkan). Covers only what agl_common prog 000/001 use; anything else raises NotImplementedError.

Conventions (Maxwell attribute space):
  a[0x70..0x7c]            position xyzw (vertex output -> gl_Position; fragment a[0x7c] = w)
  a[0x80 + 16*k + 4*c]     generic attribute k, component c (vertex in/out, fragment in)
  cN[off]                  constant buffer N, byte offset -> UBO 'cbN' as vec4 array (std140)
  fragment outputs         color0 = r0..r3 at EXIT (assumption, should be read from the SPH omap)
Usage: python -I sass2glsl.py <blob.envy> <vertex|fragment> > out.glsl
"""
import re, struct, sys

def fimm(tok):
    """Maxwell 20-bit float immediates hold the upper bits of an IEEE float."""
    v = int(tok, 16)
    bits = v << 12 if v < (1 << 20) else v
    f = struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]
    return repr(f) if f == f else '0.0'

class Translator:
    def __init__(self, stage):
        self.stage = stage
        self.regs, self.ins, self.outs, self.cbufs, self.body = set(), set(), set(), set(), []

    def operand(self, toks, i):
        """Parse one source operand starting at toks[i]; returns (glsl, next_i)."""
        neg = toks[i] == 'neg'
        if neg:
            i += 1
        t = toks[i]
        if t in ('$r255', '$rz'):
            e = '0.0'
        elif t.startswith('$r'):
            n = int(t[2:]); self.regs.add(n); e = f'r{n}'
        elif m := re.fullmatch(r'c(\d+)\[(0x[0-9a-f]+)\]', t):
            b, off = int(m[1]), int(m[2], 16); self.cbufs.add(b)
            e = f'cb{b}.d[{off // 16}].{"xyzw"[(off % 16) // 4]}'
        elif t.startswith('0x'):
            e = fimm(t)
        else:
            raise NotImplementedError(f'operand {t}')
        return (f'(-{e})' if neg else e), i + 1

    def attr(self, off, direction):
        if 0x70 <= off < 0x80:
            c = 'xyzw'[(off - 0x70) // 4]
            if direction == 'out':
                return f'gl_Position.{c}'
            return f'gl_FragCoord.{c}' if self.stage == 'fragment' else f'vin_pos.{c}'
        k, c = (off - 0x80) // 16, 'xyzw'[((off - 0x80) % 16) // 4]
        (self.outs if direction == 'out' else self.ins).add(k)
        return f'{"vout" if direction == "out" else "vin"}{k}.{c}'

    def dst(self, t):
        n = int(t[2:]); self.regs.add(n); return f'r{n}'

    def line(self, ins):
        toks = ins.replace(',', ' ').split()
        op, mods = toks[0], set()
        i = 1
        while i < len(toks) and toks[i] in ('ftz', 'sat', 'b32', 'pass', 'rcp', 'rsq', 'ex2', 'lg2', 'sin', 'cos'):
            mods.add(toks[i]); i += 1
        sat = (lambda e: f'clamp({e}, 0.0, 1.0)') if 'sat' in mods else (lambda e: e)
        if op == 'exit':
            if self.stage == 'fragment':
                self.regs.update(range(4)); self.body.append('    color0 = vec4(r0, r1, r2, r3);')
            self.body.append('    return;'); return False
        if op == 'ld':                       # ld b32 $rD a[off] 0x0
            d = self.dst(toks[i]); off = int(re.search(r'a\[(0x[0-9a-f]+)\]', toks[i + 1])[1], 16)
            self.body.append(f'    {d} = {self.attr(off, "in")};')
        elif op == 'st':                     # st b32 a[off] $rS 0x0
            off = int(re.search(r'a\[(0x[0-9a-f]+)\]', toks[i])[1], 16)
            s, _ = self.operand(toks, i + 1)
            self.body.append(f'    {self.attr(off, "out")} = {s};')
        elif op == 'ipa':                    # ipa [pass] $rD a[off] $rM ...
            d = self.dst(toks[i]); off = int(re.search(r'a\[(0x[0-9a-f]+)\]', toks[i + 1])[1], 16)
            if 'pass' in mods and off == 0x7c:
                self.body.append(f'    {d} = 1.0 / gl_FragCoord.w;  // ipa pass a[0x7c]: clip-space w')
            else:                            # perspective-corrected by hardware: multiplier dropped
                self.body.append(f'    {d} = {self.attr(off, "in")};')
        elif op == 'mufu':
            d = self.dst(toks[i]); s, _ = self.operand(toks, i + 1)
            fn = {'rcp': f'1.0 / {s}', 'rsq': f'inversesqrt({s})', 'ex2': f'exp2({s})', 'lg2': f'log2({s})',
                  'sin': f'sin({s})', 'cos': f'cos({s})'}
            self.body.append(f'    {d} = {fn[(mods & set(fn)).pop()]};')
        elif op in ('fmul', 'fadd', 'ffma', 'mov'):
            d = self.dst(toks[i]); i += 1
            srcs = []
            for _ in range({'fmul': 2, 'fadd': 2, 'ffma': 3, 'mov': 1}[op]):
                e, i = self.operand(toks, i); srcs.append(e)
            e = {'fmul': lambda a, b: f'{a} * {b}', 'fadd': lambda a, b: f'{a} + {b}',
                 'ffma': lambda a, b, c: f'fma({a}, {b}, {c})', 'mov': lambda a: a}[op](*srcs)
            self.body.append(f'    {d} = {sat(e)};')
        else:
            raise NotImplementedError(f'opcode {op}: {ins}')
        return True

    def run(self, text):
        for l in text.splitlines():
            p = l.split(None, 3)
            if len(p) < 4 or p[3].startswith(('sched', 'nop', 'C ')):
                continue
            if not self.line(p[3].strip()):
                break
        head = ['#version 450', f'// translated from Maxwell SASS ({self.stage}) by scripts/sass2glsl.py']
        if self.stage == 'vertex' and any('vin_pos' in b for b in self.body):
            head.append('layout(location = 15) in vec4 vin_pos;')
        for k in sorted(self.ins):
            head.append(f'layout(location = {k}) in vec4 vin{k};')
        for k in sorted(self.outs):
            head.append(f'layout(location = {k}) out vec4 vout{k};')
        if self.stage == 'fragment':
            head.append('layout(location = 0) out vec4 color0;')
        for b in sorted(self.cbufs):
            head.append(f'layout(std140, set = 0, binding = {b}) uniform CB{b} {{ vec4 d[4096]; }} cb{b};')
        decl = '    float ' + ', '.join(f'r{n} = 0.0' for n in sorted(self.regs)) + ';' if self.regs else ''
        return '\n'.join(head + ['void main() {', decl] + self.body + ['}', ''])

if __name__ == '__main__':
    sys.stdout.write(Translator(sys.argv[2]).run(open(sys.argv[1]).read()))
