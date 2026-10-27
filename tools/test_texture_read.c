// Exercise the NVN entry points with independently placed GOB bytes, rather
// than reading back data laid out by the same region-copy implementation.
#define _GNU_SOURCE
#define __ANDROID__ 1
#include <assert.h>
#include <stdarg.h>
#include "../runtime/nvn/objects.c"

void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *format, ...) {
    va_list args;
    va_start(args,format); vfprintf(stderr,format,args); va_end(args);
    abort();
}
static unsigned writes;
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; writes++; }

static void read_texels(NvnTexture *texture, const NvnTextureView *view, const int32_t region[6],
                        uint8_t *out, uint64_t row, uint64_t image, bool strided) {
    uint64_t object[4] = {(uintptr_t)texture};
    Cpu cpu = {0};
    cpu.x[0] = (uintptr_t)object; cpu.x[1] = (uintptr_t)view;
    cpu.x[2] = (uintptr_t)region; cpu.x[3] = (uintptr_t)out;
    cpu.x[4] = row; cpu.x[5] = image;
    if (strided) hle_nvnTextureReadTexelsStrided(&cpu);
    else hle_nvnTextureReadTexels(&cpu);
}

static void bc4_regions(void) {
    NvnTexture t = {.width=64, .height=48, .format=0x49, .target=4,
        .layout={.width=64,.height=48,.depth=1,.layers=2,.levels=3,.fmt=nvn_format_info(0x49)}};
    tegra_layout_init(&t.layout);
    assert(tegra_layer_size(&t.layout) == 3072 && tegra_total_size(&t.layout) == 6144);
    uint8_t storage[6144] = {0}, before[6144], out[144], expected[144];
    t.addr = (uintptr_t)storage;
    // BC4 blocks (x=7..9, y=7..9): cross both x and y GOB boundaries.
    const unsigned offsets[] = {504,1232,1240,808,1536,1544,824,1552,1560};
    memset(out,0xa5,sizeof out); memset(expected,0xa5,sizeof expected);
    for (unsigned i=0;i<9;i++) for (unsigned b=0;b<8;b++) {
        uint8_t value=(uint8_t)(i*19+b+1);
        storage[3072+offsets[i]+b]=value;
        expected[4+(i/3)*40+(i%3)*8+b]=value;
    }
    memcpy(before,storage,sizeof storage);
    NvnTextureView view={.setmask=3,.min_layer=1,.num_layers=1,.num_levels=1};
    int32_t region[6]={28,28,0,12,12,1};
    read_texels(&t,&view,region,out+4,40,120,true);
    assert(!memcmp(out,expected,sizeof out));
    assert(!memcmp(storage,before,sizeof storage));

    // Mip 1 starts 2048 bytes into each layer; these are its four 4x4 blocks.
    const unsigned mip_offsets[]={24,48,72,96};
    for (unsigned i=0;i<4;i++) for (unsigned b=0;b<8;b++) storage[3072+2048+mip_offsets[i]+b]=(uint8_t)(i*23+b+3);
    view.base_level=1;
    int32_t mip[6]={4,4,0,8,8,1};
    memset(out,0xa5,sizeof out); memset(expected,0xa5,sizeof expected);
    for (unsigned i=0;i<32;i++) expected[4+i]=(uint8_t)((i/8)*23+i%8+3);
    read_texels(&t,&view,mip,out+4,0,0,false);
    assert(!memcmp(out,expected,sizeof out));

    // Rejected reads must leave the caller's buffer alone.
    memcpy(expected,out,sizeof out);
    assert(!tegra_read_region(&t.layout,3,0,region,storage,0,out,0,0));
    assert(!tegra_read_region(&t.layout,0,2,region,storage,0,out,0,0));
    assert(!tegra_read_region(&t.layout,0,0,region,storage,0,out,23,0));
    assert(!tegra_read_region(&t.layout,0,0,region,storage,0,out,24,71));
    assert(!tegra_read_region(&t.layout,0,0,region,storage,0,out,UINT64_MAX,0));
    region[0]=29; assert(!tegra_read_region(&t.layout,0,0,region,storage,0,out,0,0));
    region[0]=60; assert(!tegra_read_region(&t.layout,0,0,region,storage,0,out,0,0));
    region[3]=0; assert(tegra_read_region(&t.layout,0,0,region,storage,0,out,0,0));
    assert(!memcmp(out,expected,sizeof out));
}

static void volume_and_linear(void) {
    NvnTexture t={.width=4,.height=4,.format=1,.target=2,
        .layout={.width=4,.height=4,.depth=4,.layers=1,.levels=1,.fmt=nvn_format_info(1),.block_depth_log2=2}};
    uint8_t storage[2048]={0}, out[40], expected[40];
    t.addr=(uintptr_t)storage;
    const unsigned offsets[]={529,530,577,578,1041,1042,1089,1090};
    memset(out,0xa5,sizeof out); memset(expected,0xa5,sizeof expected);
    for (unsigned i=0;i<8;i++) {
        storage[offsets[i]]=(uint8_t)(20+i);
        expected[3+(i/4)*12+((i%4)/2)*4+i%2]=(uint8_t)(20+i);
    }
    int32_t region[6]={1,1,1,2,2,2};
    read_texels(&t,NULL,region,out+3,4,12,true);
    assert(!memcmp(out,expected,sizeof out));

    t=(NvnTexture){.width=7,.height=3,.format=1,.target=1,.flags=0x10,.stride=16,
        .layout={.width=7,.height=3,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(1)},.addr=(uintptr_t)storage};
    for (unsigned i=0;i<48;i++) storage[i]=(uint8_t)(40+i);
    int32_t rect[6]={2,1,0,3,2,1};
    memset(out,0xa5,sizeof out); memset(expected,0xa5,sizeof expected);
    for (unsigned y=0;y<2;y++) for (unsigned x=0;x<3;x++) expected[3+y*8+x]=storage[18+y*16+x];
    read_texels(&t,NULL,rect,out+3,8,0,true);
    assert(!memcmp(out,expected,sizeof out));
}

static void linear_render_targets(void) {
    // Independently fill declared rows, including the non-power-of-two height
    // and mask shapes used by TOTK. Padding must never become texels.
    const unsigned widths[] = {260,68,68,260};
    const unsigned formats[] = {0x6,0x2a,0x25,0x25};
    const unsigned pitches[] = {640,640,384,1152};
    for (unsigned shape=0;shape<4;shape++) for (unsigned explicit_stride=0;explicit_stride<2;explicit_stride++) {
        unsigned w=widths[shape], h=w, bpp=nvn_format_info(formats[shape])->bpb;
        unsigned row=w*bpp, pitch=pitches[shape]+(explicit_stride?128:0);
        NvnTexture t={.width=w,.height=h,.format=formats[shape],.target=1,.flags=0x40,
            .stride=explicit_stride?pitch:0,
            .layout={.width=w,.height=h,.depth=1,.layers=1,.levels=1,.fmt=nvn_format_info(formats[shape])}};
        uint8_t *mem=malloc((size_t)pitch*h), *expected=malloc((size_t)row*h), *got=malloc((size_t)row*h);
        assert(mem && expected && got);
        memset(mem,0xa5,(size_t)pitch*h);
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<row;x++)
            mem[(size_t)y*pitch+x]=expected[(size_t)y*row+x]=(uint8_t)(y*37+x*11+x/7);
        t.addr=(uintptr_t)mem;
        assert(nvn_texture_row_stride(&t,row)==pitch);
        int32_t full[6]={0,0,0,(int32_t)w,(int32_t)h,1};
        read_texels(&t,NULL,full,got,0,0,false);
        assert(!memcmp(got,expected,(size_t)row*h));
        // An update near the last row must preserve both padding and neighbors.
        uint8_t update[48]; memset(update,0xe7,sizeof update);
        int32_t rect[6]={(int32_t)w-3,(int32_t)h-2,0,3,2,1};
        uint64_t object[4]={(uintptr_t)&t}; Cpu c={0};
        c.x[0]=(uintptr_t)object;c.x[2]=(uintptr_t)rect;c.x[3]=(uintptr_t)update;
        unsigned before=writes;hle_nvnTextureWriteTexels(&c);assert(writes==before+1);
        for (unsigned y=h-2;y<h;y++) memset(expected+(size_t)y*row+(w-3)*bpp,0xe7,3*bpp);
        read_texels(&t,NULL,full,got,0,0,false);
        assert(!memcmp(got,expected,(size_t)row*h));
        for (unsigned y=0;y<h;y++) for (unsigned x=row;x<pitch;x++) assert(mem[(size_t)y*pitch+x]==0xa5);
        free(mem);free(expected);free(got);
    }
}

static void array_1d(void) {
    NvnTexture t={.width=8,.height=1,.format=1,.target=3,
        .layout={.width=8,.height=1,.depth=1,.layers=4,.levels=1,.fmt=nvn_format_info(1)}};
    uint8_t storage[2048]={0}, out[24], expected[24];
    t.addr=(uintptr_t)storage;
    tegra_layout_init(&t.layout);
    for (unsigned z=0;z<4;z++) for (unsigned x=0;x<8;x++) storage[z*512+x]=(uint8_t)(z*30+x);
    NvnTextureView view={.setmask=2,.min_layer=1,.num_layers=3};
    int32_t region[6]={2,1,0,3,2,1}; // y selects array layers, relative to the view.
    memset(out,0xa5,sizeof out); memset(expected,0xa5,sizeof expected);
    for (unsigned z=0;z<2;z++) for (unsigned x=0;x<3;x++) expected[3+z*8+x]=(uint8_t)((z+2)*30+x+2);
    read_texels(&t,&view,region,out+3,8,0,true);
    assert(!memcmp(out,expected,sizeof out));
    assert(region[1]==1 && region[4]==2 && region[5]==1);
}

static void partial_block_atlas(void) {
    // The next story area uses a 1024x1020 source. Its mip 1 ends with 126
    // logical rows at y=384; the atlas transfer still contains 32 BC4 rows.
    NvnTexture src={.width=1024,.height=1020,.format=0x49,.target=1,
        .layout={.width=1024,.height=1020,.depth=1,.layers=1,.levels=7,.fmt=nvn_format_info(0x49)}};
    NvnTexture dst={.width=5120,.height=5120,.format=0x49,.target=1,
        .layout={.width=5120,.height=5120,.depth=1,.layers=1,.levels=7,.fmt=nvn_format_info(0x49)}};
    tegra_layout_init(&src.layout); tegra_layout_init(&dst.layout);
    uint8_t *source=malloc(tegra_total_size(&src.layout)), *atlas=malloc(tegra_total_size(&dst.layout));
    uint8_t *linear=malloc(5120u*5120u/2), transfer[32768];
    assert(source && atlas && linear);
    src.addr=(uintptr_t)source; dst.addr=(uintptr_t)atlas;
    memset(atlas,0xcc,tegra_total_size(&dst.layout));
    uint64_t object[4]={(uintptr_t)&dst};
    unsigned start_writes=writes;
    for (unsigned mip=0;mip<7;mip++) {
        LevelLayout s=tegra_level(&src.layout,mip), d=tegra_level(&dst.layout,mip);
        uint64_t end=mip+1<7 ? tegra_level(&src.layout,mip+1).offset : tegra_total_size(&src.layout);
        uint8_t value=(uint8_t)(0x21+13*mip);
        memset(source+s.offset,value,end-s.offset);
        NvnTextureView view={.setmask=1,.base_level=mip,.num_levels=1};
        unsigned step=(sizeof transfer/(s.wb*8))*4;
        for (unsigned y=0;y<s.h;y+=step) {
            int32_t region[6]={0,y,0,s.w,s.h-y<step?s.h-y:step,1};
            read_texels(&src,&view,region,transfer,0,0,false);
            Cpu cpu={0};
            cpu.x[0]=(uintptr_t)object; cpu.x[1]=(uintptr_t)&view;
            cpu.x[2]=(uintptr_t)region; cpu.x[3]=(uintptr_t)transfer;
            hle_nvnTextureWriteTexels(&cpu);
        }
        tegra_deswizzle(atlas+d.offset,linear,d.wb,d.hb,1,8,d.block_height_log2,0);
        for (unsigned y=0;y<d.hb;y++) for (unsigned x=0;x<d.wb*8;x++)
            assert(linear[(uint64_t)y*d.wb*8+x] == (y<s.hb && x<s.wb*8 ? value : 0xcc));
    }
    assert(writes-start_writes==25);
    free(linear); free(atlas); free(source);
}

int main(void) {
    bc4_regions();
    volume_and_linear();
    array_1d();
    assert(writes==0); // Reading must not publish CPU writes or invalidate uploads.
    linear_render_targets();
    partial_block_atlas();
    puts("texture read tests passed: NVN ABI, BC4 blocks, mips, arrays, 3D GOBs, linear storage, strides and bounds");
}
