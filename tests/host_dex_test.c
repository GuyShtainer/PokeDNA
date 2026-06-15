/* Host test for gen3_dex: confirm the SaveBlock2 dex-flag offsets match what the
 * trainer card reports (cross-check), proving the layout is right.
 *   cc -std=c11 -I source tests/host_dex_test.c source/gen3_save.c source/gen3_dex.c source/gen3_trainer.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c source/data_tables.c -o /tmp/hx
 *   /tmp/hx tests/fixtures/POKEMON_EMER_BPEE00.sav   (expects seen=195 owned=108)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_dex.h"
#include "gen3_trainer.h"
static uint8_t save[1<<17], sb2[1<<13];
int main(int c, char** v){
  int fail=0;
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
