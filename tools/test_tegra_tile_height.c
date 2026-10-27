// Fixed Tegra addresses for the NVN 40-block-row boundary. All synthetic data.
#include "../runtime/nvn/tegra.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const unsigned rows[] = {1,8,16,20,21,22,32,40,41,42,43,64,80,84,85,86,128,256};
    const unsigned expected[] = {0,0,1,1,1,2,2,2,2,2,3,3,3,3,3,4,4,4};
    for(unsigned i=0;i<sizeof rows/sizeof *rows;i++)
        assert(tegra_default_block_height_log2(rows[i])==expected[i]);

    const FormatInfo bc3={16,4,4,FMT_COMPRESSED,"BC3"};
    TexLayout t={.width=160,.height=160,.depth=1,.layers=1,.levels=8,.fmt=&bc3};
    tegra_layout_init(&t);
    const unsigned tiles[]={2,2,1,0,0,0,0,0};
    const uint64_t offsets[]={0,40960,51200,54272,55296,55808,56320,56832};
    for(unsigned i=0;i<8;i++) {
        LevelLayout l=tegra_level(&t,i);
        assert(l.block_height_log2==tiles[i] && l.offset==offsets[i]);
    }
    assert(tegra_total_size(&t)==57344);
    unsigned char *tiled=calloc(1,57344), *linear=calloc(1,25600);
    assert(tiled&&linear);
    // Four GOBs high. Each GOB is 64 bytes x 8 rows with 16-byte sectors.
    // Generate independent fixed-layout input, including unique markers in the
    // region the oversized eight-GOB layout incorrectly reads as zero padding.
    for(unsigned y=0;y<40;y++)for(unsigned x=0;x<40;x++) {
        unsigned xb=x*16;
        size_t at=(y/32)*20480+(xb/64)*2048+((y%32)/8)*512+
                  ((xb%64)/32)*256+((y%8)/2)*64+((xb%32)/16)*32+(y%2)*16;
        unsigned char block[16];for(unsigned k=0;k<16;k++)block[k]=(x+3*y+7*k)%251+1;
        memcpy(tiled+at,block,16);
    }
    LevelLayout l=tegra_level(&t,0);
    tegra_deswizzle(tiled,linear,40,40,1,16,l.block_height_log2,0);
    for(unsigned y=0;y<40;y++)for(unsigned x=0;x<40;x++)for(unsigned k=0;k<16;k++)
        assert(linear[(y*40+x)*16+k]==(x+3*y+7*k)%251+1);
    // Explicit packaged layouts and 3D depth tiling still override 2D defaults.
    t.block_height_log2=3;assert(tegra_level(&t,0).block_height_log2==3);
    t.depth=16;tegra_layout_init(&t);assert(t.block_height_log2==0&&t.block_depth_log2==4);
    free(tiled);free(linear);
    puts("PASS: NVN tile-height boundaries, fixed GOB addresses, mip offsets and explicit/3D layouts");
}
