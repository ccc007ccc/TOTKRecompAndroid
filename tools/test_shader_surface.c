// Synthetic surface instructions, with no game data. The GPU runner consumes
// these exact production-emitter fragments, including register conversions.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"
int main(int argc,char**argv){
    assert(argc==2);unsigned count=0;
    for(unsigned dim=0;dim<2;++dim)for(unsigned op=0;op<5;++op){
        // op 0 raw B32 load, 1 signed atomic add, 2..4 formatted float/sint/uint writes.
        ShaderInfo info={0};ShaderKey key={.glsl=1};key.image_kind[0]=op>=2?op-2:0;
        uint64_t code[]={0,0x4c98000000000000ull|2ull<<34|0x48ull<<20|7ull<<16|20};
        Ctx ctx={.info=&info,.key=&key,.glsl=true,.stage=STAGE_COMPUTE,.code=(const uint8_t*)code};
        uint64_t common=20ull<<39|(dim?3ull:0)<<33|7ull<<16|1ull<<8;
        if(op==0)emit_insn(&ctx,0xeb10000000000000ull|common|4ull<<20,16);
        else if(op==1)emit_insn(&ctx,0xea70000000000000ull|common|1ull<<36|8ull<<20,16);
        else emit_insn(&ctx,0xeb20000000000000ull|common|15ull<<20|8,16);
        assert(!ctx.fail && info.ntex==1 && info.tex[0].is_image);
        assert(info.tex[0].handle_offset==0x120 && info.tex[0].image_raw==(op<2));
        assert(info.tex[0].image_write==(op!=0) && info.image_format_mask==(op>=2?1:0));
        Str body={0};glsl_rewrite(&ctx,&body,ctx.body.buf);
        char path[1024];snprintf(path,sizeof path,"%s/surface-%u-%u.comp",argv[1],dim,op);FILE*f=fopen(path,"w");assert(f);
        fprintf(f,"#version 450\nlayout(local_size_x=1,local_size_y=1) in;\nlayout(%sset=0,binding=32) uniform %s%simage%s t0;\n",
            op==0?"r32ui, ":op==1?"r32i, ":"",op==0?"readonly ":op>=2?"writeonly ":"",op==0 || op==4?"u":op==1 || op==3?"i":"",dim?"2D":"1D");
        fputs("layout(std430,set=0,binding=0) buffer Output {uint values[];};\n"
              "void main(){uint r0=0u,r1=gl_GlobalInvocationID.x,r2=gl_GlobalInvocationID.y;\n",f);
        fputs(op==2?"uint r8=floatBitsToUint(0.25),r9=floatBitsToUint(0.5),r10=floatBitsToUint(0.75),r11=floatBitsToUint(1.0);\n":
                    op==1?"uint r8=3u;\n":"uint r8=0xfffffffcu,r9=2u,r10=4u,r11=8u;\n",f);
        fputs(body.buf,f);fputs("values[r1+4u*r2]=r0;}\n",f);assert(!fclose(f));free(body.buf);free(ctx.body.buf);++count;
    }
    // Unresolved/unaligned handles must fail instead of reading the wrong table.
    for(unsigned off=0;off<0x180;off+=4){
        ShaderInfo info={0};ShaderKey key={.glsl=1};uint64_t code[]={0,0x4c98000000000000ull|2ull<<34|(uint64_t)(off/4)<<20|7ull<<16|20};
        Ctx ctx={.info=&info,.key=&key,.glsl=true,.stage=STAGE_COMPUTE,.code=(const uint8_t*)code};
        emit_surface(&ctx,20ull<<39|4ull<<20,false,16);
        assert(ctx.fail==(shader_bound_image_slot(off)<0));free(ctx.body.buf);
    }
    printf("PASS: generated %u surface GPU programs and checked handle bounds\n",count);
}
