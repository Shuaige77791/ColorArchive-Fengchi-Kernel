#!/usr/bin/env python3
"""Pack an arm64 Image into an Android boot v4 image using the ramdisks that
ship in this repo (ramdisk/ramdisk_{ksu,noksu}.lz4).

usage: pack_boot.py <Image> <repo_root> <outdir>
"""
import os, sys, gzip, struct, hashlib, json
img_path, root, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(outdir, exist_ok=True)
PS, FMT = 4096, '<8s4I16sI1536s'

meta = {}
mi = os.path.join(root, 'ramdisk', 'HEADER_INFO.txt')
if os.path.exists(mi):
    for line in open(mi):
        if '=' in line:
            k, v = line.strip().split('=', 1)
            meta[k] = v
osver = int(meta.get('os_version', 402653604))
hs = int(meta.get('header_size', 1584))
hv = int(meta.get('header_version', 4))
cl = (meta.get('cmdline', '') or '').encode()[:1535]

img = open(img_path, 'rb').read()
gz = gzip.compress(img, 9, mtime=0)
pad = lambda x: x + b'\0'*(((len(x)+PS-1)//PS)*PS - len(x))

made = []
for tag in ('ksu', 'noksu'):
    rp = os.path.join(root, 'ramdisk', 'ramdisk_%s.lz4' % tag)
    if not os.path.exists(rp):
        print('missing %s' % rp); continue
    rd = open(rp, 'rb').read()
    hdr = struct.pack(FMT, b'ANDROID!', len(gz), len(rd), osver, hs, 0, hv, cl)
    out = pad(hdr) + pad(gz) + pad(rd)
    dst = os.path.join(outdir, 'boot_FENGCHI_%s.img' % tag)
    open(dst, 'wb').write(out)
    h = hashlib.sha256(out).hexdigest()
    print('%-34s %12d B  sha256 %s' % (os.path.basename(dst), len(out), h))
    made.append((os.path.basename(dst), h))

with open(os.path.join(outdir, 'SHA256SUMS.txt'), 'w') as f:
    for n, h in made:
        f.write('%s  %s\n' % (h, n))
    f.write('%s  Image\n' % hashlib.sha256(img).hexdigest())
print('Image %d B sha256 %s' % (len(img), hashlib.sha256(img).hexdigest()))
