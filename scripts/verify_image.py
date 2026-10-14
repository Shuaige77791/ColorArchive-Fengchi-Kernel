#!/usr/bin/env python3
"""Decisive check: image_size vs file size.  Working kernels show a tiny
overhang (~84 B); a broken layout shows hundreds of KB.  Also prints code1."""
import struct, sys, os
p = sys.argv[1]
d = open(p, 'rb').read()
magic, = struct.unpack_from('<I', d, 56)
c0, c1 = struct.unpack_from('<II', d, 0)
isz, = struct.unpack_from('<Q', d, 16)
over = len(d) - isz
print("file        = %d" % len(d))
print("magic@56    = 0x%08x %s" % (magic, 'OK(ARMd)' if magic == 0x644d5241 else 'BAD'))
print("code0/code1 = 0x%08x / 0x%08x" % (c0, c1))
print("image_size  = %d" % isz)
print("overhang    = %d bytes" % over)
if magic != 0x644d5241:
    print("FAIL: not an arm64 Image"); sys.exit(1)
if over > 200000:
    print("FAIL: %d bytes past _end -- CONFIG_DEBUG_INFO is probably on" % over)
    sys.exit(1)
print("OK: layout looks sane")
