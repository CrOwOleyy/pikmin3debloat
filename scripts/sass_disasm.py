#!/usr/bin/env python3
"""Disassemble every extracted BAHS code blob (see bahs_extract.py) with nvdisasm (SM53 = Tegra X1).
Blob layout: +0x00 NVN header (0x30), +0x30 Maxwell SPH (0x50, low 5 bits of word0: 1=vertex, 2=fragment),
+0x80 SASS. Writes <blob>.sass next to each blob and a summary line per archive.
Usage: python -I sass_disasm.py <nvdisasm.exe> <dir containing *_out folders or one *_out folder>
"""
import glob, json, os, subprocess, sys, tempfile, collections

STAGES = {1: 'vertex', 2: 'fragment', 3: 'tess_ctrl?', 4: 'tess_eval?', 5: 'geometry?'}

def main(nvdisasm, root):
    outs = [root] if os.path.exists(os.path.join(root, 'index.json')) else sorted(glob.glob(os.path.join(root, '*_out')))
    for od in outs:
        idx = json.load(open(os.path.join(od, 'index.json')))
        ops, fail, n = collections.Counter(), 0, 0
        for p in idx['programs']:
            for b in p['blobs']:
                blob = open(os.path.join(od, b['file']), 'rb').read()
                b['stage'] = STAGES.get(blob[0x30] & 0x1f, f'type{blob[0x30] & 0x1f}')
                with tempfile.NamedTemporaryFile(delete=False, suffix='.code') as t:
                    t.write(blob[0x80:])
                r = subprocess.run([nvdisasm, '-b', 'SM53', t.name], capture_output=True, text=True)
                os.unlink(t.name)
                n += 1
                if r.returncode:
                    fail += 1; b['disasm_error'] = r.stderr.strip()[:200]; continue
                open(os.path.join(od, b['file'][:-4] + '.sass'), 'w').write(r.stdout)
                for line in r.stdout.splitlines():
                    if line.strip().startswith('/*') and '*/' in line:
                        ins = line.split('*/', 1)[1].strip().lstrip('@!P0123456789T ').split(' ')[0]
                        if ins: ops[ins.split('.')[0]] += 1
        json.dump(idx, open(os.path.join(od, 'index.json'), 'w'), indent=1)
        print(f'{os.path.basename(od)}: {n} blobs, {fail} errors, top ops: {ops.most_common(12)}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
