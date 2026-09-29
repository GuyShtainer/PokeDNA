#!/usr/bin/env python3
"""Extract ONE file from a tools/vsd_img.c-built FAT16 image (8.3 names, upper-case path
components): vsd_fatget.py IMG POKEDNA/BANK/BOX00.BOX > out.bin. Read-only; used by the
#270 y7 chain to byte-compare the Bank box the harness-hosted SD received."""
import sys, struct
img = open(sys.argv[1], 'rb').read(); want = sys.argv[2].upper().split('/')
b = img[:512]
# optional MBR partition
if b[510:512] == b'\x55\xaa' and b[0] not in (0xEB, 0xE9) :
    off = struct.unpack('<I', img[454:458])[0]*512
else: off = 0
bs = img[off:off+512]
bps, spc, rsv, nfat, rootent, tot16, _, fatsz16 = struct.unpack('<HBHBHHBH', bs[11:24])
tot = tot16 or struct.unpack('<I', bs[32:36])[0]
root = off + (rsv + nfat*fatsz16)*bps
rootsz = rootent*32
data = root + rootsz
fat = img[off+rsv*bps: off+rsv*bps+fatsz16*bps]
def clus(c): return data + (c-2)*spc*bps
def chain(c):
    out=[]
    while 2 <= c < 0xFFF8:
        out.append(c); c = struct.unpack('<H', fat[c*2:c*2+2])[0]
    return out
def entries(raw):
    for i in range(0, len(raw), 32):
        e = raw[i:i+32]
        if e[0]==0: break
        if e[0]==0xE5 or e[11]==0x0F: continue
        name = (e[:8].decode('latin1').strip()+('.'+e[8:11].decode('latin1').strip() if e[8:11].strip() else '')).upper()
        yield name, e[11], struct.unpack('<H', e[26:28])[0], struct.unpack('<I', e[28:32])[0]
raw = img[root:root+rootsz]
for k, part in enumerate(want):
    found = None
    for name, attr, cl, sz in entries(raw):
        if name == part: found = (attr, cl, sz); break
    if not found: sys.exit("not found: "+part)
    attr, cl, sz = found
    content = b''.join(img[clus(c):clus(c)+spc*bps] for c in chain(cl))
    if k == len(want)-1:
        sys.stdout.buffer.write(content[:sz])
    else: raw = content
