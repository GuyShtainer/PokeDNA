#!/usr/bin/env python3
"""Render-verify the Ruby/Sapphire sprite pins (BACKLOG #293 step 1): an independent
pure-Python LZ10 decode of every front/back sheet + normal/shiny palette, located by
tools/rs_locate.py's shape scan, must equal what source/rom_sprite.c serves, byte for
byte, for ALL 440 rows (Castform's 4 formes and Deoxys' single frame included).
usage: rs_sprite_verify.py <roms dir>"""
import os, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rs_locate as L
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include "rom_map.h"
#include "rom_sprite.h"
static FILE* f;
static bool rd(void* c, uint32_t off, void* d, uint32_t n){ (void)c; if(fseek(f,(long)off,SEEK_SET))return false; return fread(d,1,n,f)==n; }
static uint8_t buf[ROM_SPRITE_BUF_BYTES];
int main(int argc,char**argv){
  f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long sz=ftell(f);
  RomCtx rc; if(!rom_open(&rc,rd,0,(uint32_t)sz)){puts("ROMOPEN FAIL");return 1;}
  RomSprite rs; if(!rom_sprite_open(&rs,&rc)){puts("SPRITEOPEN FAIL");return 1;}
  for(int ts=0;ts<440;ts++){
    uint16_t sp=(uint16_t)ts; uint8_t form=0; if(ts>=413){sp=201;form=(uint8_t)(ts-413+1);}
    for(int side=0;side<2;side++){
      RomSpritePic pi; memset(buf,0,sizeof buf);
      if(!rom_sprite_pic(&rs,(RomSpriteSide)side,sp,form,buf,sizeof buf,&pi)){printf("S%d %d FAIL\n",side,ts);continue;}
      printf("S%d %d %u ",side,ts,pi.bytes); for(uint32_t i=0;i<pi.bytes;i++)printf("%02x",buf[i]); puts("");
    }
    for(int sh=0;sh<2;sh++){ uint16_t p[16]; if(!rom_sprite_pal(&rs,sp,form,sh,p)){printf("P%d %d FAIL\n",sh,ts);continue;}
      printf("P%d %d ",sh,ts); for(int i=0;i<16;i++)printf("%04x",p[i]); puts(""); }
  }
  return 0;
}
'''
def lz(d, a):
    o = a - L.BASE
    assert d[o] == 0x10
    n = d[o+1] | d[o+2] << 8 | d[o+3] << 16
    out = bytearray(); p = o + 4
    while len(out) < n:
        fl = d[p]; p += 1
        for b in range(8):
            if len(out) >= n: break
            if fl & (0x80 >> b):
                x = d[p] << 8 | d[p+1]; p += 2
                ln = (x >> 12) + 3; disp = (x & 0xFFF) + 1
                for _ in range(ln): out.append(out[-disp])
            else:
                out.append(d[p]); p += 1
    return bytes(out)
def main():
    roms = sys.argv[1]
    for name in ('Ruby', 'Sapphire'):
        path = os.path.join(roms, name + '.gba'); d = open(path, 'rb').read()
        s = L.locate_sprites(d)
        assert len(s['sheets']) == 2 and len(s['pals']) == 1 and len(s['spals']) == 1, s
        front, back = s['sheets']; npal, spal = s['pals'][0], s['spals'][0]
        with tempfile.TemporaryDirectory() as td:
            open(td + '/h.c', 'w').write(HARNESS)
            subprocess.check_call(['cc', '-std=c11', '-I', ROOT + '/source', td + '/h.c', ROOT + '/source/rom_sprite.c',
                                   ROOT + '/source/map_render.c', ROOT + '/source/rom_map.c', '-o', td + '/h'])
            out = subprocess.check_output([td + '/h', path]).decode().splitlines()
        bad = n = 0
        for line in out:
            t = line.split(); n += 1
            if 'FAIL' in line: bad += 1; print('FAIL line', line); continue
            kind, row = t[0], int(t[1])
            if kind[0] == 'S':
                base = front if kind == 'S0' else back
                ptr = L.u32(d, base - L.BASE + 8*row)
                want = lz(d, ptr)
                if bytes.fromhex(t[3]) != want: bad += 1; print('SPRITE MISMATCH', line[:20])
            else:
                base = npal if kind == 'P0' else spal
                ptr = L.u32(d, base - L.BASE + 8*row)
                want = lz(d, ptr)[:32]
                got = struct.pack('<16H', *[int(t[2][4*i:4*i+4], 16) for i in range(16)])
                if got != want: bad += 1; print('PAL MISMATCH', line[:20])
        print(f'{name}: module vs independent LZ10 decode: {n} outputs (440 rows x front/back + normal/shiny pal), {bad} mismatches')
if __name__ == '__main__':
    main()
