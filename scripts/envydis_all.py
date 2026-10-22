#!/usr/bin/env python3
"""Run inside the envydis Docker image: disassemble every BAHS code blob (SASS at +0x80) with
envydis -m gm107 and write <blob>.envy next to it. Reports blobs with undecoded ('???') opcodes.
Usage (host): docker run --rm -v <work>:/w -v <scripts>:/s --entrypoint python3 envydis:f102b823 /s/envydis_all.py /w/shaders
"""
import glob, os, subprocess, sys, collections

root = sys.argv[1]
ops, n, unk_blobs = collections.Counter(), 0, 0
for f in sorted(glob.glob(os.path.join(root, '**', 'prog_*_stage*.bin'), recursive=True)):
    code = open(f, 'rb').read()[0x80:]
    r = subprocess.run(['envydis', '-m', 'gm107', '-i', '-n'], input=code, capture_output=True)
    out = r.stdout.decode('utf-8', 'replace')
    open(f[:-4] + '.envy', 'w').write(out)
    n += 1
    if '???' in out or r.returncode:
        unk_blobs += 1
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and not parts[3].startswith('sched'):
            ops[parts[3].split()[0]] += 1
print(f'{n} blobs, {unk_blobs} with undecoded opcodes')
print('top ops:', ops.most_common(40))
