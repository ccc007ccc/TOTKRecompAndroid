#include "pipeline_seed.h"
#include "shader_prepare.h"
#include "../../../third_party/d2r-digest/include/CommonCrypto/CommonDigest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifndef SHADER_TRANSLATION_ID
#define SHADER_TRANSLATION_ID "development"
#endif
#ifndef PIPELINE_RECIPE_ID
#define PIPELINE_RECIPE_ID "development"
#endif
#define SEED_MAX 4096
#define SEED_BYTES (4u*1024*1024)
static pthread_mutex_t writer=PTHREAD_MUTEX_INITIALIZER;
typedef struct { unsigned char *data;size_t size,at;bool reading,ok; } Codec;
static void bytes(Codec *c,void *value,size_t n) {
    if(n>c->size-c->at){c->ok=false;return;}
    if(c->reading)memcpy(value,c->data+c->at,n);else memcpy(c->data+c->at,value,n);c->at+=n;
}
static void integer(Codec *c,uint32_t *value) {
    unsigned char b[4];if(!c->reading)for(unsigned i=0;i<4;++i)b[i]=*value>>(i*8);
    bytes(c,b,4);if(c->reading && c->ok){*value=0;for(unsigned i=0;i<4;++i)*value|=(uint32_t)b[i]<<(i*8);}
}
static void key(Codec *c,uint64_t *value) {
    uint32_t lo=*value,hi=*value>>32;integer(c,&lo);integer(c,&hi);if(c->reading)*value=(uint64_t)lo|((uint64_t)hi<<32);
}
static void recipe(Codec *c,VkPipelineRecipe *r) {
    for(unsigned i=0;i<2;++i)key(c,&r->shader[i]);
    for(unsigned i=0;i<3;++i)integer(c,&r->local[i]);
    VkPipelineState *s=&r->state;
#define U32(field) do {uint32_t value=s->field;integer(c,&value);if(c->reading)s->field=value;} while(0)
#define BYTE(field) bytes(c,&s->field,sizeof s->field)
    for(unsigned i=0;i<8;++i)U32(color[i]);U32(depth);U32(stencil);U32(ncolor);U32(samples);
    for(unsigned i=0;i<32;++i){U32(attr_fmt[i]);U32(attr_off[i]);U32(attr_bind[i]);}U32(attr_mask);
    for(unsigned i=0;i<17;++i){U32(stride[i]);U32(divisor[i]);}U32(used_streams);
    BYTE(blend);BYTE(blend_on);BYTE(a2c);BYTE(topo_class);BYTE(poly_fill);BYTE(depth_clamp);BYTE(prim_restart);
    for(unsigned i=0;i<8;++i)U32(write_mask[i]);
    BYTE(fx.on);BYTE(fx.cull);BYTE(fx.ff);BYTE(fx.topo);BYTE(fx.dt);BYTE(fx.dw);BYTE(fx.dop);BYTE(fx.sten);BYTE(fx.sop);
#undef U32
#undef BYTE
}
static void header(Codec *c,VkPipelineSeedIdentity *id,uint32_t *count) {
    unsigned char magic[8]={'T','K','P','S','E','E','D','1'};bytes(c,magic,8);
    if(c->reading && memcmp(magic,"TKPSEED1",8))c->ok=false;
    uint32_t version=1;integer(c,&version);if(version!=1)c->ok=false;integer(c,count);
    integer(c,&id->game);integer(c,&id->features);
    bytes(c,id->translation,65);bytes(c,id->recipe,65);bytes(c,id->compiler,65);
}
VkPipelineSeedIdentity vk_pipeline_seed_identity(unsigned game) {
    VkPipelineSeedIdentity id={0};id.game=game;
    id.features=g_vk.dyn_render|(g_vk.eds<<1)|(g_vk.eds2<<2)|(g_vk.divisor<<3)|((getenv("LARECOMP_NO_DYN_UBO")!=NULL)<<4);
    snprintf(id.translation,65,"%s",SHADER_TRANSLATION_ID);snprintf(id.recipe,65,"%s",PIPELINE_RECIPE_ID);
    snprintf(id.compiler,65,"%s",vk_shader_cache_namespace());return id;
}
static bool valid(VkPipelineRecipe *r) {
    VkPipelineRecipe canonical;
    if(!r->shader[0])return false;
    if(r->shader[1]){if(!vk_pipeline_state_valid(&r->state))return false;vk_pipeline_recipe_make(&canonical,r->shader[0],r->shader[1],&r->state);}
    else {if(!vk_pipeline_local_size_valid(r->local))return false;vk_pipeline_recipe_compute(&canonical,r->shader[0],r->local);}
    return !memcmp(&canonical,r,sizeof *r);
}
bool vk_pipeline_seed_save(const char *path,const VkPipelineSeedIdentity *identity,const VkPipelineRecipe *recipes,unsigned count) {
    if(!path || !identity || !recipes || !count || count>SEED_MAX)return false;
    unsigned char *data=malloc(SEED_BYTES);if(!data)return false;
    Codec c={data,SEED_BYTES-32,0,false,true};VkPipelineSeedIdentity id=*identity;uint32_t n=count;header(&c,&id,&n);
    for(unsigned i=0;i<count && c.ok;++i){VkPipelineRecipe r=recipes[i];if(!valid(&r)){c.ok=false;break;}recipe(&c,&r);}
    if(c.ok){CC_SHA256(data,c.at,data+c.at);c.at+=32;}
    bool ok=false;size_t length=strlen(path);char *part=malloc(length+6);
    if(c.ok && part){memcpy(part,path,length);memcpy(part+length,".part",6);pthread_mutex_lock(&writer);
        FILE *f=fopen(part,"wb");if(f){ok=fwrite(data,1,c.at,f)==c.at;if(fclose(f))ok=false;}
        if(ok)ok=rename(part,path)==0;if(!ok && f)remove(part);pthread_mutex_unlock(&writer);}
    free(part);free(data);return ok;
}
unsigned vk_pipeline_seed_load(const char *path,const VkPipelineSeedIdentity *identity,VkPipelineRecipe *recipes,unsigned capacity) {
    if(!path || !identity || !recipes || !capacity)return 0;
    unsigned char *data=malloc(SEED_BYTES+1);if(!data)return 0;
    FILE *f=fopen(path,"rb");if(!f){free(data);return 0;}
    size_t size=fread(data,1,SEED_BYTES+1,f);bool ok=!ferror(f);fclose(f);
    if(size<251 || size>SEED_BYTES)ok=false;
    unsigned char digest[32];if(ok){CC_SHA256(data,size-32,digest);ok=!memcmp(digest,data+size-32,32);}
    Codec c={data,ok?size-32:0,0,true,ok};VkPipelineSeedIdentity id={0};uint32_t count=0;
    if(ok)header(&c,&id,&count);
    ok=c.ok && count && count<=SEED_MAX && count<=capacity && id.game==identity->game && id.features==identity->features &&
       !memcmp(id.translation,identity->translation,65) && !memcmp(id.recipe,identity->recipe,65) && !memcmp(id.compiler,identity->compiler,65);
    if(ok)for(unsigned i=0;i<count;++i){memset(&recipes[i],0,sizeof recipes[i]);recipe(&c,&recipes[i]);if(!c.ok || !valid(&recipes[i])){ok=false;break;}}
    ok=ok && c.at==c.size;free(data);return ok?count:0;
}
