"""Differential test of src/lifter/a32lift.py against Unicorn.

    python difftest.py <leaves.json> [cases_per_function] [seed]

1. lifts the functions of leaves.json to work/lifter/lifted.c and builds harness.exe (MSVC);
2. generates random cases (registers: pointers into a scratch area or interesting / random values; random NZCV);
3. runs every case in Unicorn first; each case starts from the image's own data (writable segments restored, scratch
   refilled, stack zeroed, as in the harness); cases that fault or do not return are dropped;
4. runs the same cases with the lifted C and compares r0-r14, NZCV and the hashes of the scratch and stack areas.
"""
import json
import os
import random
import struct
import subprocess
import sys

import unicorn as uc
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_CPSR

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import a32lift  # noqa: E402

ROOT = a32lift.ROOT
WORK = os.path.join(ROOT, 'work', 'lifter')
SCRATCH, STACK, RSIZE, SENTINEL = 0x20000000, 0x30000000, 0x10000, 0xDEAD0000
FN_ADDRS = [0]                      # sorted addresses of the lifted functions (the harness's fns[]), set by build()
REGS = [UC_ARM_REG_R0 + k for k in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]
VCVARS = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'


def fnv(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xffffffff
    return h


def scratch_bytes(seed):
    """the harness's scratch fill; seed bit 1 selects the pointer-heavy profile"""
    x, out = seed | 1, bytearray()
    heavy = seed >> 1 & 1
    ptr, zero = (10, 14) if heavy else (6, 9)
    for _ in range(RSIZE // 4):
        x ^= (x << 13) & 0xffffffff
        x ^= x >> 17
        x ^= (x << 5) & 0xffffffff
        t = x >> 28
        v = SCRATCH + (((x >> 4) % (RSIZE - 0x100)) & ~3) if t < ptr else 0 if t < zero else x
        if heavy and t == 14:                               # heavy: 6.25 % addresses of lifted functions (indirect calls)
            v = FN_ADDRS[(x >> 4) % len(FN_ADDRS)]
        out += struct.pack('<I', v)
    return bytes(out)


def build(sel):
    """lifts the selected functions and everything they call directly (closure); the table is sorted by address"""
    cl = a32lift.closure([p['addr'] for p in sel], limit=100000)
    if cl is None:
        sys.exit('a selected function (or one it calls) cannot be lifted')
    addrs = sorted(cl)
    FN_ADDRS[:] = addrs
    a32lift.cmd_lift(os.path.join(WORK, 'lifted.c'), addrs)
    with open(os.path.join(WORK, 'lifted_table.h'), 'w') as f:
        f.write('static const struct { uint32_t addr; void (*fn)(Ctx *); } fns[] = {\n')
        f.writelines('    {0x%xu, f_%x},\n' % (a, a) for a in addrs)
        f.write('};\n')
    print('lifted %d functions (closure of %d roots)' % (len(addrs), len(sel)))
    bat = os.path.join(WORK, 'build.bat')
    with open(bat, 'w') as f:
        f.write('@echo off\ncall "%s" >nul 2>&1\ncl /nologo /O1 /I"%s" /I"%s" "%s" /Fe:harness.exe\n'
                % (VCVARS, HERE, WORK, os.path.join(HERE, 'harness.c')))
    r = subprocess.run(['cmd', '/c', bat], cwd=WORK, capture_output=True, text=True)
    if r.returncode or not os.path.exists(os.path.join(WORK, 'harness.exe')):
        print(r.stdout[-3000:], r.stderr[-2000:])
        sys.exit('build failed')


def gen_regs(rng, heavy=False):
    regs = []
    for k in range(13):
        t = rng.random()
        if t < (0.95 if heavy else 0.7):
            regs.append(SCRATCH + rng.randrange(0, RSIZE - 0x1000) & ~3)
        elif t < 0.8:
            regs.append(rng.choice([0, 1, 2, 31, 32, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0x100, 0xff]))
        else:
            regs.append(rng.getrandbits(32))
    return regs + [STACK + RSIZE - 0x800, SENTINEL, 0]


def make_unicorn():
    mu = uc.Uc(uc.UC_ARCH_ARM, uc.UC_MODE_ARM)
    mu.ctl_set_cpu_model(uc.arm_const.UC_CPU_ARM_MAX)      # ARMv8 in AArch32 state, as the Switch (LDA*, STL*, VSEL...)
    for va, off, fs, ms, _fl in a32lift.SEGS:
        lo, hi = va & ~0xfff, (va + ms + 0xfff) & ~0xfff
        mu.mem_map(lo, hi - lo, uc.UC_PROT_ALL if _fl & 2 else uc.UC_PROT_READ | uc.UC_PROT_EXEC)
        mu.mem_write(va, a32lift.IMG[off:off + fs])
    mu.mem_map(SCRATCH, RSIZE)
    mu.mem_map(STACK, RSIZE)
    mu.rw_init = [(va, a32lift.IMG[off:off + fs] + bytes(ms - fs)) for va, off, fs, ms, fl in a32lift.SEGS if fl & 2]
    mu.mem_map(SENTINEL, 0x1000)
    lo, hi = min(a32lift.PLT), max(a32lift.PLT) + 16

    def plt_hook(mu, addr, size, _data):                # imports: the harness's test semantics, then return to LR
        if addr in a32lift.PLT:
            idx = a32lift.PLT[addr][0]
            r = [mu.reg_read(UC_ARM_REG_R0 + k) for k in range(4)]
            m = 0xffffffff
            v = ((r[0] * 0x9E3779B1) & m) ^ ((r[1] * 0x85EBCA77) & m) ^ ((r[2] * 0xC2B2AE3D) & m) ^ r[3] ^ ((idx * 0x27D4EB2F) & m)
            mu.reg_write(UC_ARM_REG_R0, v)
            mu.reg_write(UC_ARM_REG_R0 + 1, r[0] ^ idx)
            mu.reg_write(uc.arm_const.UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
    mu.hook_add(uc.UC_HOOK_CODE, plt_hook, begin=lo, end=hi)
    return mu


def main():
    sel = json.load(open(sys.argv[1]))
    per = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    os.makedirs(WORK, exist_ok=True)
    build(sel)
    mu = make_unicorn()
    cases, expect = [], []
    dropped = 0
    for p in sel:
        for _ in range(per):
            seed = rng.getrandbits(32)
            regs, nzcv = gen_regs(rng, heavy=bool(seed >> 1 & 1)), rng.getrandbits(4) << 28
            if 'fix' in p:                                  # a register the selection wants driven (switch index)
                regs[p['fix']['reg']] = rng.choice(p['fix']['values'])
            mu.mem_write(SCRATCH, scratch_bytes(seed))
            mu.mem_write(STACK, bytes(RSIZE))
            for va, data in mu.rw_init:                     # every case starts from the image's own data
                mu.mem_write(va, data)
            for r, v in zip(REGS, regs):
                mu.reg_write(r, v)
            mu.reg_write(UC_ARM_REG_CPSR, 0x10 | nzcv)                 # user mode, ARM state
            try:
                mu.emu_start(p['addr'], SENTINEL, count=200000)
            except uc.UcError:
                dropped += 1
                continue
            if mu.reg_read(uc.arm_const.UC_ARM_REG_PC) != SENTINEL:
                dropped += 1
                continue
            cases.append(struct.pack('<19I', p['addr'], seed, *regs, nzcv))
            expect.append((p, [mu.reg_read(r) for r in REGS], mu.reg_read(UC_ARM_REG_CPSR) & 0xf0000000,
                           fnv(mu.mem_read(SCRATCH, RSIZE)), fnv(mu.mem_read(STACK, RSIZE))))
    with open(os.path.join(WORK, 'cases.bin'), 'wb') as f:
        f.writelines(cases)
    subprocess.run([os.path.join(WORK, 'harness.exe'), a32lift.ELF, os.path.join(WORK, 'cases.bin'), os.path.join(WORK, 'results.bin')], check=True)
    res = open(os.path.join(WORK, 'results.bin'), 'rb').read()
    per_fn, first_bad, nofn = {}, {}, 0
    for k, (p, regs, nzcv, hs, hk) in enumerate(expect):
        got = struct.unpack_from('<21I', res, k * 84)
        if got[0] == 3:                                   # indirect call to a function outside the lifted set
            nofn += 1
            continue
        ok = got[0] == 0 and list(got[1:16]) == regs and got[17] == nzcv and got[18] == hs and got[19] == hk
        st = per_fn.setdefault(p['addr'], [0, 0])
        st[0 if ok else 1] += 1
        if not ok and p['addr'] not in first_bad:
            diffs = ['r%d %08x/%08x' % (i, got[1 + i], regs[i]) for i in range(15) if got[1 + i] != regs[i]]
            if got[17] != nzcv:
                diffs.append('nzcv %x/%x' % (got[17] >> 28, nzcv >> 28))
            if got[18] != hs:
                diffs.append('scratch memory')
            if got[19] != hk:
                diffs.append('stack memory')
            if got[0]:
                diffs.append({1: 'C fault', 4: 'instruction not lifted'}.get(got[0], 'status %d' % got[0]))
            first_bad[p['addr']] = ', '.join(diffs) + '  (lifted/unicorn)'
    total_ok = sum(v[0] for v in per_fn.values())
    total = sum(v[0] + v[1] for v in per_fn.values())
    for p in sel:
        ok, bad = per_fn.get(p['addr'], [0, 0])
        print('0x%-7x %-28s %4d ok %4d bad  %s' % (p['addr'], p['name'], ok, bad, first_bad.get(p['addr'], '')))
    print('total: %d / %d cases identical (%d dropped: fault or no return in Unicorn; %d skipped: indirect call to a '
          'function not lifted)' % (total_ok, total, dropped, nofn))


if __name__ == '__main__':
    main()
