/* Host test for gen3_dex: confirm the SaveBlock2 dex-flag offsets match what the
 * trainer card reports (cross-check), proving the layout is right; plus a
 * self-contained per-game round-trip of the National-Dex unlock (magic+var+flag).
 *   cc -std=c11 -I source tests/host_dex_test.c source/gen3_save.c source/gen3_dex.c source/gen3_flags.c source/gen3_trainer.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c source/data_tables.c -o /tmp/hx
 *   /tmp/hx tests/fixtures/POKEMON_EMER_BPEE00.sav   (expects seen=195 owned=108)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_dex.h"
#include "gen3_flags.h"
#include "gen3_trainer.h"
static uint8_t save[1<<17], sb2[1<<13];

/* National-Dex unlock: write all three values, confirm pk_dex_national_on sees them at
 * exactly the per-game offsets/values verified against the decomps, then clear + recheck. */
static uint8_t nsb1[1<<16], nsb2[1<<13];
static int natl_case(const char* tag, PkGame g, int magic_abs, uint8_t magic,
                     int var_abs, uint16_t var_v, int flag){
  int fail=0;
  memset(nsb1,0,sizeof nsb1); memset(nsb2,0,sizeof nsb2);
  if (pk_dex_national_on(nsb1,nsb2,g)){ printf("  %s: blank reads as national!\n",tag); fail++; }
  pk_dex_set_national(nsb1,nsb2,g,true);
  uint16_t vv=(uint16_t)(nsb1[var_abs]|(nsb1[var_abs+1]<<8));
  int ok = nsb2[magic_abs]==magic && vv==var_v && pk_flag_get(nsb1,g,flag) && pk_dex_national_on(nsb1,nsb2,g);
  printf("  %s ON : magic[%#x]=%#x(exp %#x) var[%#x]=%#x(exp %#x) flag %#x=%d -> %s\n",
         tag, magic_abs, nsb2[magic_abs], magic, var_abs, vv, var_v, flag, pk_flag_get(nsb1,g,flag), ok?"OK":"BAD");
  if(!ok) fail++;
  pk_dex_set_national(nsb1,nsb2,g,false);
  int off = !pk_dex_national_on(nsb1,nsb2,g) && nsb2[magic_abs]==0 && !pk_flag_get(nsb1,g,flag);
  printf("  %s OFF: %s\n", tag, off?"OK":"BAD");
  if(!off) fail++;
  return fail;
}
static int natl_roundtrip(void){
  printf("National-Dex unlock round-trip:\n");
  int fail=0;
  /* RS:  magic sb2[0x1A]=0xDA, var sb1[0x13CC]=0x302, flag 0x836 */
  fail += natl_case("RS  ", PK_RS,      0x1A, 0xDA, 0x1340+0x46*2, 0x302,  0x836);
  /* EM:  magic sb2[0x1A]=0xDA, var sb1[0x1428]=0x302, flag 0x896 */
  fail += natl_case("EM  ", PK_EMERALD, 0x1A, 0xDA, 0x139C+0x46*2, 0x302,  0x896);
  /* FRLG:magic sb2[0x1B]=0xB9, var sb1[0x109C]=0x6258, flag 0x840 */
  fail += natl_case("FRLG", PK_FRLG,    0x1B, 0xB9, 0x1000+0x4E*2, 0x6258, 0x840);
  printf("%s\n\n", fail?"national: FAIL":"national: OK");
  return fail;
}

/* BACKLOG #370: Emerald dex VIEW byte (SB2 pokedex+0x01) / ORDER byte (+0x00). */
#define VIEW_OFF (0x18 + 0x01)
#define ORDER_OFF (0x18 + 0x00)
static int natl_view_case(void){
  int fail=0;
  /* A: National ALREADY on, Hoenn view, alphabetical order -> Catch ALL's set_national(true) must not touch either byte */
  memset(nsb1,0,sizeof nsb1); memset(nsb2,0,sizeof nsb2);
  pk_dex_set_national(nsb1,nsb2,PK_EMERALD,true);
  nsb2[VIEW_OFF]=0; nsb2[ORDER_OFF]=1;
  pk_dex_set_national(nsb1,nsb2,PK_EMERALD,true);
  if (nsb2[VIEW_OFF]!=0 || nsb2[ORDER_OFF]!=1){ printf("  370A: already-on Catch ALL moved view/order (%d/%d)\n",nsb2[VIEW_OFF],nsb2[ORDER_OFF]); fail++; }
  /* B: locked -> unlocked still flips the view to National and the order to 0 (documented behaviour) */
  memset(nsb1,0,sizeof nsb1); memset(nsb2,0,sizeof nsb2); nsb2[ORDER_OFF]=1;
  pk_dex_set_national(nsb1,nsb2,PK_EMERALD,true);
  if (nsb2[VIEW_OFF]!=1 || nsb2[ORDER_OFF]!=0){ printf("  370B: unlock transition did not set the National view (%d/%d)\n",nsb2[VIEW_OFF],nsb2[ORDER_OFF]); fail++; }
  /* C: lock again (Undo of a Catch ALL that auto-unlocked) -> view back to Hoenn */
  pk_dex_set_national(nsb1,nsb2,PK_EMERALD,false);
  if (nsb2[VIEW_OFF]!=0){ printf("  370C: locking National left the National view on (%d)\n",nsb2[VIEW_OFF]); fail++; }
  /* D: other games never get the Emerald-only bytes written */
  memset(nsb1,0,sizeof nsb1); memset(nsb2,0,sizeof nsb2); nsb2[VIEW_OFF]=7; nsb2[ORDER_OFF]=9;
  pk_dex_set_national(nsb1,nsb2,PK_FRLG,true);  pk_dex_set_national(nsb1,nsb2,PK_FRLG,false);
  pk_dex_set_national(nsb1,nsb2,PK_RS,true);    pk_dex_set_national(nsb1,nsb2,PK_RS,false);
  if (nsb2[VIEW_OFF]!=7 || nsb2[ORDER_OFF]!=9){ printf("  370D: a non-Emerald game had its view/order bytes written\n"); fail++; }
  printf("%s\n\n", fail?"national view/order (#370): FAIL":"national view/order (#370): OK");
  return fail;
}

int main(int c, char** v){
  int fail=0;
  fail += natl_roundtrip();
  fail += natl_view_case();
  for (int a=1;a<c;a++){
    FILE* f=fopen(v[a],"rb"); if(!f)continue; size_t n=fread(save,1,sizeof save,f); fclose(f);
    Gen3SaveInfo info; if(!gen3_parse(save,(uint32_t)n,&info)){printf("%s parse fail\n",v[a]);fail++;continue;}
    int s0=gen3_find_section(save,info.slot,0);
    memcpy(sb2, save+(uint32_t)info.slot*G3_SLOT_BYTES+(uint32_t)s0*G3_SECTOR_SIZE, G3_SECTOR_DATA_SIZE);
    int seen,caught; pk_pokedex(sb2,&seen,&caught,0);
    int ds=pk_dex_count(sb2,false), dc=pk_dex_count(sb2,true);
    printf("%s: trainer seen=%d caught=%d | gen3_dex seen=%d owned=%d %s\n",
           v[a], seen, caught, ds, dc, (seen==ds&&caught==dc)?"OK":"MISMATCH");
    if (seen!=ds || caught!=dc) fail++;
  }
  printf("\n%s\n", fail?"FAIL":"OK");
  return fail?1:0;
}
