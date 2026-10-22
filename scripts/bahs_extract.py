#!/usr/bin/env python3
"""Extractor for Nintendo AGL 'BAHS' shader archives (.sharcb, Switch/NVN).

Layout (derived from agl_common.sharcb, see notes/EMULATION_SURFACE.md §8-§10):
  0x00 'BAHS', version, file_size, n, 0, 0x1000, code_size(@0x18), code_off(@0x1c)
  0x150.. string table (archive name, UBO, attribute / uniform names)
  descriptors area: one per program, containing a GLSLC header (magic 0x98761234); the dword
      at (magic-96) is the program's offset *relative to code_off* of its first code blob.
  code area [code_off, code_off+code_size): sequence of 0x100-aligned blobs, each starting with
      0x12345678 (NVN shader-data header, 0x30 bytes) followed by Maxwell SASS (a program has
      one blob per stage: vertex / fragment...). The 4-byte tag at blob+0x30 differs per stage.
Usage: python -I bahs_extract.py file.sharcb outdir
"""
import re, struct, sys, os, json

def cstr_at(d, off, lo=0x150, hi=0x1000):
    if not (lo <= off < hi) or d[off - 1] != 0:      # must be the start of a string
        return None
    j = d.find(b'\0', off)
    s = d[off:j]
    return s.decode() if 0 < len(s) < 80 and all(32 <= c < 127 for c in s) else None

def main(path, outdir):
    d = open(path, 'rb').read()
    assert d[:4] == b'BAHS', 'not a BAHS file'
    ver = struct.unpack('<I', d[4:8])[0]
    code_size, code_off = struct.unpack('<II', d[0x18:0x20])
    code = d[code_off:code_off + code_size]
    blob_offs = [m.start() for m in re.finditer(b'\x78\x56\x34\x12', code) if m.start() % 0x100 == 0]
    blob_ends = blob_offs[1:] + [len(code)]
    magics = [m.start() for m in re.finditer(b'\x34\x12\x76\x98', d[:code_off])]
    poffs = [struct.unpack('<I', d[m - 96:m - 92])[0] for m in magics]
    assert poffs == sorted(poffs) and poffs[-1] < code_size, 'heuristic failed'
    os.makedirs(outdir, exist_ok=True)
    progs = []
    for i, (m, a) in enumerate(zip(magics, poffs)):
        b = poffs[i + 1] if i + 1 < len(poffs) else code_size
        blobs = [k for k, o in enumerate(blob_offs) if a <= o < b]
        names = [n for (v,) in struct.iter_unpack('<I', d[m - 128:m]) if (n := cstr_at(d, v))]
        files = []
        for k in blobs:
            fn = f'prog_{i:03d}_stage{blobs.index(k)}.bin'
            open(os.path.join(outdir, fn), 'wb').write(code[blob_offs[k]:blob_ends[k]])
            files.append({'file': fn, 'size': blob_ends[k] - blob_offs[k], 'tag': code[blob_offs[k] + 0x30:blob_offs[k] + 0x34].hex()})
        progs.append({'index': i, 'code_off': a, 'blobs': files, 'names': names})
    json.dump({'source': os.path.basename(path), 'version': ver, 'code_off': code_off, 'blobs_total': len(blob_offs), 'programs': progs},
              open(os.path.join(outdir, 'index.json'), 'w'), indent=1)
    print(f'{len(progs)} programs, {len(blob_offs)} code blobs, code area 0x{code_off:x}+0x{code_size:x}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
