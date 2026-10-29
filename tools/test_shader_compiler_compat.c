// Compare old/new plugin outputs, not just normalized IR. Run on host or Android.
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/nvn/vulkan/shader_prepare.h"
#include "../runtime/nvn/vulkan/shader_cache.h"
typedef struct {void* library;void* context;ShaderPrepareDestroy destroy;ShaderPrepareCompileContext compile;ShaderPrepareDerive derive;} Plugin;
static Plugin open_plugin(const char* path) {
    Plugin p={0};p.library=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!p.library){fprintf(stderr,"%s\n",dlerror());exit(2);}
    ShaderPrepareCreate create=(ShaderPrepareCreate)dlsym(p.library,"shader_prepare_create");
    p.destroy=(ShaderPrepareDestroy)dlsym(p.library,"shader_prepare_destroy");
    p.compile=(ShaderPrepareCompileContext)dlsym(p.library,"shader_prepare_compile_context");
    p.derive=(ShaderPrepareDerive)dlsym(p.library,"shader_prepare_derive");assert(create && p.destroy && p.compile && p.derive);
    p.context=create();assert(p.context);return p;
}
static unsigned direct,derived;
// Only floating locations can be retyped by the preparation interface. Input
// samples may mix numeric types; always compare their direct compiled bytes.
static int floating_location(const uint32_t* spv,size_t words,int stage) {
    assert(vk_shader_cache_framed(spv,words));
    uint32_t bound=spv[3];
    uint32_t *type=calloc(bound,sizeof *type),*location=malloc(bound*sizeof *location);
    assert(type && location);for(uint32_t i=0;i<bound;++i)location[i]=UINT32_MAX;
    for(size_t at=5;at<words;at+=spv[at]>>16) {
        const uint32_t *w=spv+at;unsigned n=w[0]>>16,op=w[0]&65535;
        if(op==22 && n==3 && w[1]<bound)type[w[1]]=w[2]==32;
        if(op==23 && n==4 && w[1]<bound && w[2]<bound)type[w[1]]=type[w[2]];
        if(op==32 && n==4 && w[1]<bound && w[3]<bound)type[w[1]]=type[w[3]];
        if(op==71 && n==4 && w[2]==30 && w[1]<bound)location[w[1]]=w[3];
    }
    int found=-1;
    for(size_t at=5;at<words;at+=spv[at]>>16) {
        const uint32_t *w=spv+at;unsigned n=w[0]>>16,op=w[0]&65535;
        if(op==59 && n>=4 && w[1]<bound && w[2]<bound && type[w[1]] &&
            w[3]==(stage==0?1u:3u) && location[w[2]]<(stage==0?32u:8u)){found=(int)location[w[2]];break;}
    }
    free(type);free(location);return found;
}
static void check(Plugin* a,Plugin* b,const char* source,int stage,unsigned api,bool optimized) {
    char error[8192];size_t na=0,nb=0;
    uint32_t *wa=a->compile(a->context,source,stage,api,optimized,&na,error,sizeof error);
    if(!wa){fprintf(stderr,"old compiler: %s\n",error);exit(1);}
    uint32_t *wb=b->compile(b->context,source,stage,api,optimized,&nb,error,sizeof error);
    if(!wb){fprintf(stderr,"new compiler: %s\n",error);exit(1);}
    assert(na==nb && !memcmp(wa,wb,na*4));++direct;
    int location=(stage==0 || stage==1)?floating_location(wa,na,stage):-1;
    if(location>=0)for(unsigned type=1;type<=2;++type) {
        uint8_t kinds[32]={0};kinds[location]=type;size_t xa=0,xb=0;
        uint32_t *va=a->derive(wa,na,stage,kinds,api,&xa,error,sizeof error);
        if(!va){fprintf(stderr,"old derive: %s\n",error);exit(1);}
        uint32_t *vb=b->derive(wb,nb,stage,kinds,api,&xb,error,sizeof error);
        if(!vb){fprintf(stderr,"new derive: %s\n",error);exit(1);}
        assert(va && vb && xa==xb && !memcmp(va,vb,xa*4));free(va);free(vb);++derived;
    }
    free(wa);free(wb);
}
int main(int argc,char** argv) {
    if(argc<3){fprintf(stderr,"usage: test_shader_compiler_compat OLD.so NEW.so [real.vert|frag|comp ...]\n");return 2;}
    Plugin a=open_plugin(argv[1]),b=open_plugin(argv[2]);
    const char* source[]={
        "#version 450\nlayout(location=0) in vec4 a;void main(){gl_Position=a*1.125+vec4(0.001);}\n",
        "#version 450\nlayout(location=0) out vec4 c;void main(){c=vec4(0.5);gl_FragDepth=gl_FragCoord.z*0.91+0.017;}\n",
        "#version 450\nlayout(location=0) out vec4 c;void main(){c=vec4(gl_FragCoord.x*0.7+0.2);}\n",
        "#version 450\nlayout(local_size_x=1) in;layout(r32i,set=0,binding=0) uniform iimage2D img;void main(){imageAtomicAdd(img,ivec2(0),3);}\n"};
    const int stages[]={0,1,1,5};
    for(unsigned minor=1;minor<=3;minor+=2)for(unsigned opt=0;opt<2;++opt)for(unsigned i=0;i<4;++i)
        check(&a,&b,source[i],stages[i],(1u<<22)|(minor<<12),opt!=0);
    for(int i=3;i<argc;++i) {
        const char* ext=strrchr(argv[i],'.');int stage=ext&&!strcmp(ext,".vert")?0:ext&&!strcmp(ext,".frag")?1:ext&&!strcmp(ext,".comp")?5:-1;assert(stage>=0);
        FILE* f=fopen(argv[i],"rb");assert(f && !fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0 && n<=16*1024*1024 && !fseek(f,0,SEEK_SET));
        char* text=calloc((size_t)n+1,1);assert(text && fread(text,1,n,f)==(size_t)n);fclose(f);
        check(&a,&b,text,stage,(1u<<22)|(3u<<12),false);free(text);
        printf("checked module %s\n",argv[i]);fflush(stdout);
    }
    a.destroy(a.context);b.destroy(b.context);
    printf("PASS: %u direct and %u derived old/new SPIR-V modules are byte-identical\n",direct,derived);
}
