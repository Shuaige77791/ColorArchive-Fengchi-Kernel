#!/usr/bin/env python3
"""Pack an arm64 Image into an Android boot v4 image.
usage: pack_boot.py <Image> <ramdisk_dir_with_donor_imgs> <outdir>"""
import os, sys, gzip, struct, hashlib, glob
img_path, donor_dir, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(outdir, exist_ok=True)
PS, FMT = 4096, '<8s4I16sI1536s'

def donor_ramdisk(path):
    d = open(path, 'rb').read()
    m, ks, rs, ov, hs, rv, hv, cl = struct.unpack_from(FMT, d, 0)
    assert m == b'ANDROID!', path
    kp = ((max(hs, struct.calcsize(FMT)) + PS - 1)//PS)*PS
    rp = kp + ((ks + PS - 1)//PS)*PS
    return d[rp:rp+rs], (m, ks, rs, ov, hs, rv, hv, cl)

img = open(img_path, 'rb').read()
gz = gzip.compress(img, 9, mtime=0)
pad = lambda x: x + b'\0'*(((len(x)+PS-1)//PS)*PS - len(x))

cands = sorted(glob.glob(os.path.join(donor_dir, '*.img')))
if not cands:
    print("no donor boot images in %s -- copy EEVDF boot images there" % donor_dir)
    sys.exit(1)
made = []
for c in cands:
    tag = 'ksu' if 'ksu' in os.path.basename(c).lower() else 'noksu'
    rd, (m, ks, rs, ov, hs, rv, hv, cl) = donor_ramdisk(c)
    hdr = struct.pack(FMT, b'ANDROID!', len(gz), len(rd), ov, hs, rv, hv, cl)
    out = pad(hdr) + pad(gz) + pad(rd)
    dst = os.path.join(outdir, 'boot_FENGCHI_%s.img' % tag)
    open(dst, 'wb').write(out)
    h = hashlib.sha256(out).hexdigest()
    print("%-34s %12d B  sha256 %s" % (os.path.basename(dst), len(out), h))
    made.append((os.path.basename(dst), h))
with open(os.path.join(outdir, 'SHA256SUMS.txt'), 'w') as f:
    for n, h in made:
        f.write('%s  %s\n' % (h, n))
print("Image %d B sha256 %s" % (len(img), hashlib.sha256(img).hexdigest()))
