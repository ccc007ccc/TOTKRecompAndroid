#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/tegra.h"

int main(void) {
    TexLayout t = {.width=16,.height=16,.depth=1,.layers=3,.levels=3,.fmt=nvn_format_info(0x25)};
    tegra_layout_init(&t);
    uint64_t size = tegra_total_size(&t), layer = tegra_layer_size(&t);
    uint8_t *storage = malloc(size), *before = malloc(size), source[128], linear[8*8*4];
    assert(storage && before); memset(storage, 0xcc, size); memcpy(before, storage, size);
    for (unsigned i=0;i<sizeof source;i++) source[i]=(uint8_t)i;
    int32_t region[6]={1,2,0,3,2,2};
    assert(tegra_write_region(&t,1,1,region,storage,0,source,20,64));
    LevelLayout l=tegra_level(&t,1);
    assert(!memcmp(storage,before,layer)); // the first array layer is untouched
    for(unsigned z=0;z<2;z++) {
        tegra_deswizzle(storage+(z+1)*layer+l.offset,linear,l.wb,l.hb,1,4,l.block_height_log2,0);
        for(unsigned y=0;y<8;y++) for(unsigned x=0;x<8;x++) for(unsigned b=0;b<4;b++) {
            uint8_t want=(y>=2&&y<4&&x>=1&&x<4) ? source[z*64+(y-2)*20+(x-1)*4+b] : 0xcc;
            assert(linear[(y*8+x)*4+b]==want);
        }
        assert(!memcmp(storage+(z+1)*layer,before+(z+1)*layer,l.offset)); // mip 0 is untouched
    }
    memcpy(before,storage,size); region[0]=7;
    assert(!tegra_write_region(&t,1,1,region,storage,0,source,20,64));
    assert(!memcmp(before,storage,size));
    free(storage); free(before);

    t=(TexLayout){.width=16,.height=16,.depth=1,.layers=3,.levels=2,.fmt=nvn_format_info(0x80)};
    tegra_layout_init(&t); size=tegra_total_size(&t); layer=tegra_layer_size(&t);
    storage=malloc(size); memset(storage,0xcc,size);
    int32_t compressed[6]={0,0,0,8,8,1}; l=tegra_level(&t,1);
    assert(tegra_write_region(&t,1,2,compressed,storage,0,source,0,0));
    for(uint64_t i=0;i<size;i++) assert(storage[i]==(i>=2*layer+l.offset&&i<2*layer+l.offset+16 ? source[i-2*layer-l.offset] : 0xcc));
    compressed[0]=1; assert(!tegra_write_region(&t,1,2,compressed,storage,0,source,0,0)); free(storage);

    t=(TexLayout){.width=4,.height=4,.depth=4,.layers=1,.levels=1,.fmt=nvn_format_info(1),.block_depth_log2=2};
    size=tegra_total_size(&t); storage=malloc(size); memset(storage,0xcc,size);
    int32_t volume[6]={1,1,1,2,2,2};
    assert(tegra_write_region(&t,0,0,volume,storage,0,source,2,4));
    const uint64_t offsets[]={529,530,577,578,1041,1042,1089,1090};
    for(uint64_t i=0;i<size;i++) {
        uint8_t want=0xcc; for(unsigned j=0;j<8;j++) if(i==offsets[j])want=source[j];
        assert(storage[i]==want);
    }
    free(storage);
    t=(TexLayout){.width=7,.height=3,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(1)};
    uint8_t pitched[48]; memset(pitched,0xcc,sizeof pitched);
    int32_t rect[6]={2,1,0,3,2,1};
    assert(tegra_write_region(&t,0,0,rect,pitched,16,source,8,0));
    for(unsigned i=0;i<48;i++) {
        uint8_t want=i>=18&&i<21?source[i-18]:i>=34&&i<37?source[8+i-34]:0xcc;
        assert(pitched[i]==want);
    }
    puts("texture region tests passed: mip/array placement, strides, compressed blocks, 3D GOB addresses and rejected bounds");
}
