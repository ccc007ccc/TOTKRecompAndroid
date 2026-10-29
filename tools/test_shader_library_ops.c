// Regression cases for library opcodes: address arithmetic, flag transfer,
// derivative sampling and integer texel loads. No game data is embedded.
#include <assert.h>
#include "../runtime/nvn/maxwell/translate.c"
static void shader(const char *dir, unsigned index, uint64_t word, int stage) {
    unsigned char data[0xc0]={0};uint64_t code[]={0,word,0xe30000000007000full,0xe2400fffff87000full};
    size_t prefix=stage==STAGE_COMPUTE?0:0x80;
    uint32_t mask=15;memcpy(data+0x78,&mask,4);memcpy(data+prefix,code,sizeof code);
    ShaderKey key={.glsl=1,.vs_out_mask=~0u};
    ShaderInfo *s=maxwell_translate(data,prefix+sizeof code,stage,&key);
    if(!s->msl){fprintf(stderr,"%016llx: %s\n",(unsigned long long)word,s->error);abort();}
    char path[1024];snprintf(path,sizeof path,"%s/library-%u.%s",dir,index,stage==STAGE_VERTEX?"vert":stage==STAGE_FRAGMENT?"frag":"comp");
    FILE *f=fopen(path,"w");assert(f);fputs(s->msl,f);assert(!fclose(f));maxwell_free(s);
}
int main(int argc,char **argv) {
    assert(argc==2);char path[1024];snprintf(path,sizeof path,"%s/arithmetic.cpp",argv[1]);FILE *f=fopen(path,"w");assert(f);
    fputs("#include <cstdint>\n#include <cstdio>\n#include <array>\nusing uint=uint32_t;\n",f);
    unsigned count=0,shaders=0;
    maxwell_no_int64=true;maxwell_gmem_base=0x340000000ull;maxwell_gmem_size=0xd0000000ull;
    for(unsigned form=0;form<3;form++)for(unsigned neg=0;neg<2;neg++)for(unsigned shift=0;shift<32;shift++) {
        uint64_t w=(form==0?0x5bd0000000000000ull:form==1?0x36d0000000000000ull:0x4bd0000000000000ull)|
            7ull<<48|(uint64_t)neg<<45|(uint64_t)shift<<39|7ull<<16|1ull<<8;
        if(form==0)w|=2ull<<20;else if(form==1)w|=1ull<<56|0x54321ull<<20;else w|=3ull<<34|5ull<<20;
        ShaderInfo info={0};ShaderKey key={.glsl=1};Ctx ctx={.info=&info,.key=&key,.glsl=true,.stage=STAGE_COMPUTE};
        emit_insn(&ctx,w,8);assert(!ctx.fail);
        fprintf(f,"uint lea%u(uint a,uint b){uint r0=0,r1=a,r2=b;std::array<uint,4> cb3[16]{};cb3[1][1]=b;\n%sreturn r0;}\n",count++,ctx.body.buf);
        free(ctx.body.buf);shader(argv[1],shaders++,w,STAGE_COMPUTE);
    }
    for(unsigned byte=0;byte<4;byte++) {
        ShaderInfo info={0};ShaderKey key={.glsl=1};Ctx ctx={.info=&info,.key=&key,.glsl=true,.stage=STAGE_COMPUTE};
        uint64_t common=7ull<<16|1ull<<40|(uint64_t)byte<<41|2ull<<20;
        emit_insn(&ctx,0x5cf0000000000000ull|common|1ull<<8,8);
        emit_insn(&ctx,0x5ce8000000000000ull|common|3ull<<8,16);
        assert(!ctx.fail && ctx.uses_cco);
        fprintf(f,"uint flags%u(uint r1,uint r2,uint r3,uint initial){uint r0=0,cc=(initial>>2)&1;bool ccz=initial&1,ccs=initial&2,cco=initial&8;\n%sreturn r0;}\n",byte,ctx.body.buf);
        free(ctx.body.buf);
        shader(argv[1],shaders++,0x5cf0000000000000ull|common|1ull<<8,STAGE_FRAGMENT);
        shader(argv[1],shaders++,0x5ce8000000000000ull|common|3ull<<8,STAGE_VERTEX);
    }
    fputs("using Lea=uint(*)(uint,uint);Lea lea[]={",f);for(unsigned i=0;i<count;i++)fprintf(f,"lea%u,",i);fputs("};\n",f);
    fputs("using Flags=uint(*)(uint,uint,uint,uint);Flags flags[]={flags0,flags1,flags2,flags3};\n"
          "uint state=0x52676487;uint next(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}\n"
          "int main(){unsigned long long tests=0;for(unsigned k=0;k<192;k++)for(unsigned i=0;i<4096;i++){uint a=next(),b=next();"
          "uint expected=uint(uint64_t(a)<<(k%32));if((k/32)%2)expected=0u-expected;expected+=(k/64==1)?0xfffd4321u:b;"
          "if(lea[k](a,b)!=expected)return 1;tests++;}\n"
          "for(unsigned byte=0;byte<4;byte++)for(uint initial=0;initial<16;initial++)for(uint mask=0;mask<16;mask++)for(uint v=0;v<256;v++){"
          "uint input=(next()&~(255u<<(byte*8)))|(v<<(byte*8)),base=next();uint bits=(initial&~mask)|(v&mask);uint m=mask<<(byte*8);"
          "uint expected=(base&~m)|((bits<<(byte*8))&m);if(flags[byte](input,mask,base,initial)!=expected)return 2;tests++;}"
          "std::printf(\"%llu LEA/CC vectors passed\\n\",tests);}\n",f);
    assert(!fclose(f));
    const unsigned dims[]={0,1,2,3,4,6,7};
    for(unsigned i=0;i<7;i++)for(unsigned mask=1;mask<16;mask++) {
        uint64_t w=0xde00000000000000ull|7ull<<51|8ull<<36|(uint64_t)dims[i]<<28|(uint64_t)mask<<31|7ull<<16|12ull<<20|4ull<<8;
        shader(argv[1],shaders++,w,STAGE_FRAGMENT);
    }
    for(unsigned dim=0;dim<5;dim++)for(unsigned offset=0;offset<2;offset++)for(unsigned lod=0;lod<2;lod++) {
        uint64_t w=0xdc00000000000000ull|7ull<<51|(uint64_t)lod<<55|8ull<<36|(uint64_t)offset<<35|
            (uint64_t)dim<<28|15ull<<31|7ull<<16|12ull<<20|4ull<<8;
        shader(argv[1],shaders++,w,STAGE_FRAGMENT);
    }
    // Output words beyond R254 must be discarded, including all results at RZ.
    for(unsigned d=254;d<=255;d++) {
        uint64_t w=0xde00000000000000ull|7ull<<51|8ull<<36|2ull<<28|15ull<<31|7ull<<16|12ull<<20|4ull<<8|d;
        shader(argv[1],shaders++,w,STAGE_FRAGMENT);
    }
    printf("Generated %u shader cases and arithmetic/flag-transfer checks\n",shaders);
}
