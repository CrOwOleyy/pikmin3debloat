/* Differential-test harness for src/lifter/a32lift.py: runs lifted functions on test cases written by difftest.py.
 *   harness <main.elf> <cases.bin> <results.bin>
 * Guest memory: 4 GB reservation; committed: the ELF's PT_LOAD segments, scratch 0x20000000 (64 KB, refilled for each
 * case from the case's seed with xorshift32; the writable segments are restored for every case; 37.5 % of its words point into it, 18.75 % are 0), stack 0x30000000 (64 KB, zeroed for each case).
 * Case: fn, seed, r[16], nzcv (u32 each). Result: status (0 ok, 1 fault, 3 indirect call to a function not lifted,
 * 4 instruction not lifted), r[16], nzcv, fnv(scratch), fnv(stack). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "a32rt.h"

uint8_t *MB;
#include "lifted.c"
#include "lifted_table.h"     /* static const struct { uint32_t addr; void (*fn)(Ctx *); } fns[]; sorted by addr */

#define SCRATCH 0x20000000u
#define STACK 0x30000000u
#define RSIZE 0x10000u

static void commit(uint32_t a, uint32_t size) {
    uint32_t lo = a & ~0xfffu, hi = (a + size + 0xfffu) & ~0xfffu;
    if (!VirtualAlloc(MB + lo, hi - lo, MEM_COMMIT, PAGE_READWRITE)) { fprintf(stderr, "commit %x failed\n", lo); exit(1); }
}
static uint32_t fnv(const uint8_t *p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }

#define EX_NOFN 0xE0000001u      /* call_indirect to an address that was not lifted */
#define EX_UNIMPL 0xE0000002u

/* fns[] is sorted by address (difftest.py writes it so) */
void call_indirect(Ctx *c, uint32_t addr) {
    size_t lo = 0, hi = sizeof fns / sizeof fns[0];
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (fns[m].addr < addr) lo = m + 1; else hi = m;
    }
    if (lo < sizeof fns / sizeof fns[0] && fns[lo].addr == addr) { fns[lo].fn(c); return; }
    RaiseException(EX_NOFN, 0, 0, NULL);
}
/* test semantics of every import (the same in difftest.py's Unicorn hook) */
void imp_call(Ctx *c, int idx) {
    uint32_t r0 = c->r[0];
    c->r[0] = r0 * 0x9E3779B1u ^ c->r[1] * 0x85EBCA77u ^ c->r[2] * 0xC2B2AE3Du ^ c->r[3] ^ (uint32_t)idx * 0x27D4EB2Fu;
    c->r[1] = r0 ^ (uint32_t)idx;
}
void guest_unimpl(Ctx *c, uint32_t pc, uint32_t insn) { (void)c; (void)pc; (void)insn; RaiseException(EX_UNIMPL, 0, 0, NULL); }

static int filt(DWORD code) { return code == EX_NOFN ? 3 : code == EX_UNIMPL ? 4 : 1; }
static int run(void (*fn)(Ctx *), Ctx *c) {
    int st = 0;
    __try { fn(c); } __except (st = filt(GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) { return st; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    MB = VirtualAlloc(NULL, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *elf = malloc(n); fread(elf, 1, n, f); fclose(f);
    uint32_t phoff = *(uint32_t *)(elf + 0x1c); uint16_t phnum = *(uint16_t *)(elf + 0x2c);
    uint32_t *rw[8]; int nrw = 0;
    for (int i = 0; i < phnum; i++) {
        uint32_t *ph = (uint32_t *)(elf + phoff + i * 32);
        if (ph[0] != 1) continue;
        commit(ph[2], ph[5]);
        memcpy(MB + ph[2], elf + ph[1], ph[4]);
        if (ph[6] & 2 && nrw < 8) rw[nrw++] = ph;          /* writable segments are restored before every case */
    }
    for (int i = 0; i < phnum; i++) {                     /* the others are read-only, as on the console */
        uint32_t *ph = (uint32_t *)(elf + phoff + i * 32);
        DWORD old;
        if (ph[0] == 1 && !(ph[6] & 2)) VirtualProtect(MB + (ph[2] & ~0xfffu), ((ph[2] + ph[5] + 0xfffu) & ~0xfffu) - (ph[2] & ~0xfffu), PAGE_READONLY, &old);
    }
    commit(SCRATCH, RSIZE); commit(STACK, RSIZE);
    FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    uint32_t cs[19];
    while (fread(cs, 4, 19, in) == 19) {
        uint32_t x = cs[1] | 1, heavy = cs[1] >> 1 & 1;   /* seed bit 1: pointer-heavy profile */
        for (uint32_t i = 0; i < RSIZE / 4; i++) {          /* 37.5 % (heavy: 62.5 %) pointers into the scratch area, 18.75 % (25 %) zeros */
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            uint32_t t = x >> 28;
            uint32_t v = t < (heavy ? 10u : 6u) ? SCRATCH + (((x >> 4) % (RSIZE - 0x100)) & ~3u) : t < (heavy ? 14u : 9u) ? 0 : x;
            if (heavy && t == 14) v = fns[(x >> 4) % (sizeof fns / sizeof fns[0])].addr;   /* heavy: 6.25 % lifted function addresses */
            wr32(SCRATCH + 4 * i, v);
        }
        memset(MB + STACK, 0, RSIZE);
        for (int i = 0; i < nrw; i++) { memcpy(MB + rw[i][2], elf + rw[i][1], rw[i][4]); memset(MB + rw[i][2] + rw[i][4], 0, rw[i][5] - rw[i][4]); }
        Ctx c; memset(&c, 0, sizeof c);
        memcpy(c.r, cs + 2, 64);
        c.N = cs[18] >> 31 & 1; c.Z = cs[18] >> 30 & 1; c.C = cs[18] >> 29 & 1; c.V = cs[18] >> 28 & 1;
        void (*fn)(Ctx *) = NULL;
        for (size_t k = 0; k < sizeof fns / sizeof fns[0]; k++) if (fns[k].addr == cs[0]) fn = fns[k].fn;
        uint32_t res[21];
        res[0] = fn ? run(fn, &c) : 2;
        memcpy(res + 1, c.r, 64);
        res[17] = c.N << 31 | c.Z << 30 | c.C << 29 | c.V << 28;
        res[18] = fnv(MB + SCRATCH, RSIZE); res[19] = fnv(MB + STACK, RSIZE); res[20] = 0;
        fwrite(res, 4, 21, out);
    }
    fclose(out);
    return 0;
}
