#!/usr/bin/env python3
"""Render-verify the Ruby/Sapphire icon tables (BACKLOG #293 step 2), three ways:

 1. INDEPENDENT decode: follow the by-shape-located tables (tools/rs_locate.py) in pure
    Python and render RGB15 for every row.
 2. THE MODULE: a tiny host harness links source/rom_mon.c and dumps rom_mon_icon() +
    rom_mon_icon_pal() for every row; it must equal (1) byte for byte.
 3. THE NORMAL BUILD: compare (1) against the compiled fan-art icons (mon_icons.bin,
    LANCZOS-downscaled 64->32, so NOT expected to be identical) with an Emerald control
    run through the same metric, to show R/S lands in the same class as Emerald.

usage: rs_icon_verify.py <roms dir> <mon_icons.bin> <mon_icons.c>
"""
import os, re, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rs_locate as L

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rom_map.h"
#include "rom_mon.h"
static FILE* f;
static bool rd(void* c, uint32_t off, void* d, uint32_t n){ (void)c; if(fseek(f,(long)off,SEEK_SET))return false; return fread(d,1,n,f)==n; }
int main(int argc,char**argv){
  f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f);
  RomCtx rc; if(!rom_open(&rc,rd,0,(uint32_t)sz)){puts("ROMOPEN FAIL");return 1;}
  RomMon rm; if(!rom_mon_open(&rm,&rc)){puts("MONOPEN FAIL");return 1;}
  for(int p=0;p<3;p++){uint16_t c[16]; if(!rom_mon_icon_pal(&rm,p,c)){puts("PAL FAIL");return 1;}
    printf("P%d",p); for(int i=0;i<16;i++)printf(" %04x",c[i]); puts("");}
  for(int ts=0;ts<440;ts++){
    uint16_t sp=(uint16_t)ts; uint8_t form=0; if(ts>=413){sp=201;form=(uint8_t)(ts-413+1);}
    uint8_t fr[2][512]; uint8_t pal=0xFF;
    if(!rom_mon_icon(&rm,sp,form,0,fr[0],&pal)||!rom_mon_icon(&rm,sp,form,1,fr[1],0)){printf("R%d FAIL\n",ts);continue;}
    printf("R%d %d ",ts,pal); for(int k=0;k<2;k++)for(int i=0;i<512;i++)printf("%02x",fr[k][i]); puts("");
  }
  return 0;
}
'''

def decode_tiles(b):
    """512 B 4bpp, 16 tiles in 1D order (4 per row) -> 32x32 index grid."""
    px = [[0]*32 for _ in range(32)]
    for t in range(16):
        tx, ty = t % 4, t // 4
        for y in range(8):
            for xb in range(4):
                v = b[t*32 + y*4 + xb]
                px[ty*8+y][tx*8+xb*2]   = v & 15
                px[ty*8+y][tx*8+xb*2+1] = v >> 4
    return px

def pal_rgb(d, addr):
    o = addr - L.BASE
    return [struct.unpack_from('<H', d, o + 2*i)[0] & 0x7FFF for i in range(16)]

def indep_tables(d):
    r = [x for x in L.locate_icons(d) if x['shape_ok']]
    assert len(r) == 1, r
    r = r[0]
    icons = [L.u32(d, r['icons'] - L.BASE + 4*i) for i in range(440)]
    ids = list(d[r['ids'] - L.BASE: r['ids'] - L.BASE + 440])
    pals = [pal_rgb(d, L.u32(d, r['pals'] - L.BASE + 8*k)) for k in range(3)]
    return icons, ids, pals

def emerald_tables(d):
    h = 0x100
    ic = L.u32(d, h + 0x38) - L.BASE; idx = L.u32(d, h + 0x3C) - L.BASE; pl = L.u32(d, h + 0x40) - L.BASE
    return ([L.u32(d, ic + 4*i) for i in range(440)], list(d[idx:idx+440]),
            [pal_rgb(d, L.u32(d, pl + 8*k)) for k in range(3)])

def render(d, tabs, row, frame):
    icons, ids, pals = tabs
    o = icons[row] - L.BASE + frame*512
    px = decode_tiles(d[o:o+512])
    p = pals[ids[row]]
    return [[(0x8000 | p[v]) if v else 0 for v in r] for r in px]

def normal_blob(blob_path, c_path):
    blob = open(blob_path, 'rb').read()
    src = open(c_path).read()
    off = [int(x, 16) for x in re.search(r'off_tbl\[412\] = \{([^}]*)\}', src).group(1).replace('\n','').split(',') if x.strip()]
    uf = [int(x, 16) for x in re.search(r'uform_tbl\[28\] = \{([^}]*)\}', src).group(1).split(',')]
    def get(row, frame):
        o = off[row] if row < 412 else (uf[row - 413 + 1] if row >= 413 else None)
        if o is None or o == 0xFFFFFFFF: return None
        o += frame * 2048
        return [[struct.unpack_from('<H', blob, o + 2*(y*32+x))[0] for x in range(32)] for y in range(32)]
    return get

def metric(a, b):
    """(mask mismatches, mean per-channel 5-bit error over mutually opaque px, #both opaque)"""
    mm = 0; err = 0; n = 0
    for y in range(32):
        for x in range(32):
            pa, pb = a[y][x], b[y][x]
            if bool(pa & 0x8000) != bool(pb & 0x8000): mm += 1
            elif pa & 0x8000:
                n += 1
                for s in (0, 5, 10): err += abs(((pa >> s) & 31) - ((pb >> s) & 31))
    return mm, (err / (3*n) if n else 0.0), n

def main():
    roms, blob_path, c_path = sys.argv[1:4]
    get_norm = normal_blob(blob_path, c_path)
    summary = {}
    for name in ('Ruby', 'Sapphire', 'Emerald'):
        path = os.path.join(roms, name + '.gba'); d = open(path, 'rb').read()
        tabs = emerald_tables(d) if name == 'Emerald' else indep_tables(d)
        if name != 'Emerald':
            # (2) module == independent decode, byte for byte
            with tempfile.TemporaryDirectory() as td:
                open(td + '/h.c', 'w').write(HARNESS)
                subprocess.check_call(['cc', '-std=c11', '-I', ROOT + '/source', td + '/h.c', ROOT + '/source/rom_mon.c',
                                       ROOT + '/source/rom_map.c', '-o', td + '/h'])
                out = subprocess.check_output([td + '/h', path]).decode().splitlines()
            icons, ids, pals = tabs
            bad = 0
            for line in out:
                t = line.split()
                if t[0].startswith('P'):
                    k = int(t[0][1:]); got = [int(x, 16) for x in t[1:]]
                    if got != [v & 0x7FFF for v in pals[k]]: bad += 1; print('PAL MISMATCH', k)
                elif t[0].startswith('R') and len(t) > 2:
                    row = int(t[0][1:]); want_pal = ids[row]
                    raw = bytes.fromhex(t[2])
                    o = icons[row] - L.BASE
                    if int(t[1]) != want_pal or raw != d[o:o+1024]: bad += 1; print('ROW MISMATCH', row)
                else:
                    bad += 1; print('LINE', line)
            print(f'{name}: module vs independent decode: {len(out)} lines, {bad} mismatches (rows 0..439 both frames + 3 palettes)')
        # (3) vs the normal build's compiled icons
        rows = [r for r in list(range(1, 252)) + list(range(277, 412)) + [412] + list(range(413, 440))]
        tot_mm = tot_n = 0; tot_err = 0.0; cmp = 0; worst = []
        sel = {}
        for r in rows:
            a = get_norm(r if r < 413 else 201, 0) if False else None
        # normal blob rows: species axis 0..411, Egg 412 has no entry (off_tbl has 412), Unown via uform
        for r in rows:
            nr = get_norm(r, 0) if r < 412 else (None if r == 412 else get_norm(r, 0))
            if nr is None: continue
            rr = render(d, tabs, r, 0)
            mm, e, n = metric(rr, nr)
            tot_mm += mm; tot_err += e * n; tot_n += n; cmp += 1
            sel[r] = (mm, round(e, 2))
        print(f'{name}: vs normal compiled icons, {cmp} rows compared: mask mismatch/row {tot_mm/cmp:.1f} px, '
              f'mean 5-bit channel err on shared-opaque px {tot_err/max(tot_n,1):.2f}')
        for r, lab in ((1, 'Bulbasaur'), (412, 'Egg'), (414, 'Unown C'), (413, 'Unown B')):
            if r in sel: print(f'   row {r} {lab}: mask mismatch {sel[r][0]} px, chan err {sel[r][1]}')
        summary[name] = (tot_mm/cmp, tot_err/max(tot_n,1))

if __name__ == '__main__':
    main()
