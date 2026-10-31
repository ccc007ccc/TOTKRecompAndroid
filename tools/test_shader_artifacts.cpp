// Independent metadata values, complete identities, strict module loading,
// malformed files and concurrent publication. No GPU or phone needed.
#include "../runtime/nvn/vulkan/shader_artifact.h"
#include "../runtime/nvn/vulkan/shader_artifact_storage.hpp"
#include "../runtime/nvn/vulkan/shader_prepare.h"
#include "../runtime/nvn/vulkan/shader_warm_manifest.hpp"
#include "../runtime/diagnostics_build.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
extern "C" {
#include "../runtime/nvn/vulkan/shaders.h"
VkG g_vk; void rt_fatal_trace(void) { abort(); }
}
using Bytes=std::vector<uint8_t>;
static const char *source="#version 450\nlayout(set=1,binding=3) uniform CB3{vec4 value;};\n"
    "layout(set=1,binding=32) uniform sampler2D t0;\nlayout(location=0) out vec4 c;"
    "void main(){c=texture(t0,vec2(.5))*value;if(c.a<.1)discard;gl_FragDepth=.25;}\n";
static void put(Bytes& b,size_t at,uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(8*i);}
static Bytes read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path& p,const Bytes& b){std::ofstream f(p,std::ios::binary);f.write((const char*)b.data(),b.size());assert(f.good());}
static void resign(Bytes& b){CC_SHA256(b.data(),b.size()-32,b.data()+b.size()-32);}
static void equal(const VkShaderArtifact& a){
    assert(a.info && !a.info->msl && a.spirv && a.words>5 && !a.vertex_index);
    const auto& i=*a.info;
    assert(i.stage==1 && i.writes_depth && i.uses_discard && !i.writes_point_size);
    assert(i.cbuf_mask==8 && !i.cbuf_dynamic && i.cbuf_size[3]==16);
    assert(i.attr_mask==0x13 && i.out_mask==15 && i.local_mem==32 && i.num_insts==71);
    assert(!i.uses_gmem && !i.uses_gmem_map && !i.uses_gmem_store && i.uses_rscale && i.uses_tint);
    assert(i.image_format_mask==5 && i.ntex==3);
    assert(i.tex[0].type==TEXTYPE_2D && i.tex[0].handle_offset==0x168 && !i.tex[0].shadow && !i.tex[0].is_image);
    assert(i.tex[1].type==TEXTYPE_2D_ARRAY && i.tex[1].is_array && i.tex[1].bindless &&
        i.tex[1].bindless_cbuf==2 && i.tex[1].bindless_offset==0x120 && i.tex[1].image_kind==1);
    assert(i.tex[2].type==TEXTYPE_3D && i.tex[2].is_image && i.tex[2].image_raw && i.tex[2].image_write && i.tex[2].image_kind==2);
    assert(a.source_key==vk_shader_prepare_key(source,STAGE_FRAGMENT));
}
int main(int argc,char** argv){
    assert(argc==2);std::filesystem::create_directories(argv[1]);assert(!chdir(argv[1]));
    unsetenv("LARECOMP_SHADER_ARTIFACTS");unsetenv("LARECOMP_VK_OPT");
    Bytes control(0x880),prefix(0x100),data(0x100);
    put(control,0,0x98761234);put(control,0x20,0x880);put(control,0x704,prefix.size()+data.size());
    shader_archive::Shader shader{5,0x80,data,control,prefix};uint8_t raw[32];
    assert(vk_shader_artifact_input_key(control.data(),control.size(),prefix.data(),prefix.size(),data.data(),data.size(),raw));
    auto expected=shader_warm::raw_key(shader);assert(!memcmp(raw,expected.data(),32));
    auto malformed=control;put(malformed,0x704,0xffffffff);
    assert(!vk_shader_artifact_input_key(malformed.data(),malformed.size(),prefix.data(),prefix.size(),data.data(),data.size(),raw));
    assert(!vk_shader_artifact_input_key(control.data(),0x40,prefix.data(),prefix.size(),data.data(),data.size(),raw));
    assert(!vk_shader_artifact_input_key(control.data(),control.size(),nullptr,prefix.size(),data.data(),data.size(),raw));
    memcpy(raw,expected.data(),32);
    ShaderKey key{};key.glsl=1;key.gather_bias_bits=8;key.vs_out_mask=0x13;
    ShaderInfo info{};info.stage=1;info.msl=const_cast<char*>(source);info.cbuf_mask=8;info.cbuf_size[3]=16;
    info.attr_mask=0x13;info.out_mask=15;info.writes_depth=info.uses_discard=info.uses_rscale=info.uses_tint=true;
    info.local_mem=32;info.num_insts=71;info.image_format_mask=5;info.ntex=3;
    info.tex[0].type=TEXTYPE_2D;info.tex[0].handle_offset=0x168;
    info.tex[1].type=TEXTYPE_2D_ARRAY;info.tex[1].is_array=info.tex[1].bindless=true;
    info.tex[1].bindless_cbuf=2;info.tex[1].bindless_offset=0x120;info.tex[1].image_kind=1;
    info.tex[2].type=TEXTYPE_3D;info.tex[2].is_image=info.tex[2].image_raw=info.tex[2].image_write=true;info.tex[2].image_kind=2;
    auto profile=vk_shader_artifact_profile(0x12345678);VkShaderArtifact a{};
    assert(!vk_shader_artifact_load(raw,&key,profile,1,&a) && !a.info && !a.spirv);
    assert(vk_shader_artifact_store(raw,&key,profile,&info));
    // Metadata alone never grants readiness.
    assert(!vk_shader_artifact_load(raw,&key,profile,1,&a) && !a.info && !a.spirv);
    char why[4096];assert(vk_shader_prepare_begin(why,sizeof why));
    assert(vk_shader_prepare_source(source,1,"artifact-test",why,sizeof why));vk_shader_prepare_end();
    setenv("LARECOMP_NATIVE_LIB_DIR","/compiler-unavailable-for-artifact-load",1);
    assert(vk_shader_artifact_load(raw,&key,profile,1,&a));equal(a);vk_shader_artifact_free(&a);vk_shader_artifact_free(&a);
    assert(!vk_shader_artifact_load(raw,&key,profile,0,&a));
    assert(!vk_shader_artifact_load(raw,&key,profile+1,1,&a));
    auto changed=key;changed.image_kind[7]=2;assert(!vk_shader_artifact_load(raw,&changed,profile,1,&a));
    changed=key;changed.attr_kind[31]=1;assert(!vk_shader_artifact_load(raw,&changed,profile,1,&a));
    changed=key;changed.flip_x=true;assert(!vk_shader_artifact_load(raw,&changed,profile,1,&a));
    changed=key;changed.no_ds_write=true;assert(!vk_shader_artifact_load(raw,&changed,profile,1,&a));
    auto raw_changed=expected;raw_changed[31]^=1;assert(!vk_shader_artifact_load(raw_changed.data(),&key,profile,1,&a));
    prefix[0]^=1;uint8_t other[32];assert(vk_shader_artifact_input_key(control.data(),control.size(),prefix.data(),prefix.size(),data.data(),data.size(),other));
    assert(memcmp(raw,other,32) && !vk_shader_artifact_load(other,&key,profile,1,&a));
    bool old=maxwell_gmem_map_all;maxwell_gmem_map_all=!old;
    assert(vk_shader_artifact_profile(0x12345678)!=profile);maxwell_gmem_map_all=old;
    setenv("LARECOMP_SHADER_ARTIFACTS","0",1);assert(!vk_shader_artifact_load(raw,&key,profile,1,&a));unsetenv("LARECOMP_SHADER_ARTIFACTS");
    setenv("LARECOMP_SHADER_SUBST","x",1);
#if TOTK_DIAGNOSTICS
    assert(!vk_shader_artifact_load(raw,&key,profile,1,&a));
#else
    assert(vk_shader_artifact_load(raw,&key,profile,1,&a));vk_shader_artifact_free(&a);
#endif
    unsetenv("LARECOMP_SHADER_SUBST");
    std::filesystem::path path;
    for(const auto& p:std::filesystem::recursive_directory_iterator("cache/shader-artifacts"))if(p.path().extension()==".bin")path=p.path();
    assert(!path.empty());auto original=read(path);assert(original.size()==192+3*24);
    // Known little-endian payload offsets; native structs/pointers aren't serialized.
    assert(original[40]==1 && original[136]==0x13 && original[156]==3 && original[160]==0x68 && original[161]==1);
    for(unsigned mode=0;mode<8;++mode){
        auto corrupt=original;
        if(mode==0)corrupt[100]^=1;
        if(mode==1){put(corrupt,156,65);resign(corrupt);}
        if(mode==2){put(corrupt,164,256);resign(corrupt);}
        if(mode==3){put(corrupt,52,0x80000000);resign(corrupt);}
        if(mode==4)corrupt.resize(corrupt.size()-1);
        if(mode==5)corrupt.push_back(0);
        if(mode==6){put(corrupt,160,0xffffffff);resign(corrupt);}
        if(mode==7){put(corrupt,200,0xffffffff);resign(corrupt);}
        write(path,corrupt);assert(!vk_shader_artifact_load(raw,&key,profile,1,&a) && !a.info && !a.spirv);
    }
    write(path,original);
    auto spvkey=vk_shader_prepare_key(source,1);size_t words;
    auto spv=vk_shader_cache_load_key(spvkey,false,&words);assert(spv);
    auto spvpath=std::filesystem::path("cache/shaders")/vk_shader_cache_namespace();
    char filename[32];snprintf(filename,sizeof filename,"%016llx.bin",(unsigned long long)spvkey);spvpath/=filename;
    auto packed=read(spvpath);write(spvpath,Bytes{1,2,3});
    assert(!vk_shader_artifact_load(raw,&key,profile,1,&a));write(spvpath,packed);
    assert(vk_shader_artifact_load(raw,&key,profile,1,&a) && a.words==words && !memcmp(spv,a.spirv,words*4));
    free(spv);vk_shader_artifact_free(&a);
    std::vector<std::thread> threads;
    for(unsigned t=0;t<8;++t)threads.emplace_back([&]{
        for(unsigned i=0;i<24;++i){assert(vk_shader_artifact_store(raw,&key,profile,&info));VkShaderArtifact item{};
            assert(vk_shader_artifact_load(raw,&key,profile,1,&item));equal(item);vk_shader_artifact_free(&item);}
    });
    for(auto& t:threads)t.join();
    auto tmp=path;tmp+=".tmp";std::filesystem::create_directory(tmp);
    auto failures=vk_shader_artifact_stats().failed;
    assert(vk_shader_artifact_store(raw,&key,profile,&info));vk_shader_artifact_flush();
    assert(vk_shader_artifact_stats().failed==failures+1);std::filesystem::remove(tmp);
    assert(read(path)==original);
    assert(vk_shader_artifact_load(raw,&key,profile,1,&a));vk_shader_artifact_free(&a);
    auto stats=vk_shader_artifact_stats();assert(stats.hits>=194 && stats.misses>=18 && stats.stored && stats.stored<=193);
    // This unit is compiled with a small retention limit. Independently stored
    // identities exercise eviction without creating tens of thousands of files.
    std::array<uint8_t,32> first_raw=expected,last_raw=expected;
    for(unsigned i=0;i<17;++i){auto distinct=expected;distinct[0]=uint8_t(i+1);distinct[1]^=0x80;
        if(!i)first_raw=distinct;last_raw=distinct;
        assert(vk_shader_artifact_store(distinct.data(),&key,profile,&info));vk_shader_artifact_flush();}
    assert(!vk_shader_artifact_load(first_raw.data(),&key,profile,1,&a));
    assert(vk_shader_artifact_load(last_raw.data(),&key,profile,1,&a));vk_shader_artifact_free(&a);
    unsigned files=0;for(const auto& entry:std::filesystem::directory_iterator(path.parent_path()))if(entry.path().extension()==".bin")++files;
    assert(files==16);
    assert(shader_artifact_storage::at_capacity());
    std::map<std::filesystem::path,Bytes> retained;
    for(const auto& entry:std::filesystem::directory_iterator(path.parent_path()))retained.emplace(entry.path(),read(entry.path()));
    auto deferred=shader_artifact_storage::stats().deferred;
    {
        shader_artifact_storage::BackgroundScope background(true);
        for(unsigned i=0;i<32;++i) {
            auto cold=expected;cold[0]=uint8_t(i+40);cold[1]^=0x40;
            assert(vk_shader_artifact_store(cold.data(),&key,profile,&info));vk_shader_artifact_flush();
            assert(!vk_shader_artifact_load(cold.data(),&key,profile,1,&a));
        }
        assert(shader_artifact_storage::stats().deferred==deferred+32);
        for(const auto& entry:retained)assert(read(entry.first)==entry.second);
        // Existing records can still be repaired while full.
        info.num_insts=72;
        assert(vk_shader_artifact_store(last_raw.data(),&key,profile,&info));vk_shader_artifact_flush();
        assert(vk_shader_artifact_load(last_raw.data(),&key,profile,1,&a) && a.info->num_insts==72);vk_shader_artifact_free(&a);
        auto priority=expected;priority[0]=90;
        try {
            shader_artifact_storage::BackgroundScope foreground(false);
            assert(vk_shader_artifact_store(priority.data(),&key,profile,&info));vk_shader_artifact_flush();
            throw 7;
        }catch(int){}
        assert(vk_shader_artifact_load(priority.data(),&key,profile,1,&a));vk_shader_artifact_free(&a);
        auto cold=expected;cold[0]=91;
        assert(vk_shader_artifact_store(cold.data(),&key,profile,&info));vk_shader_artifact_flush();
        assert(!vk_shader_artifact_load(cold.data(),&key,profile,1,&a));
        // The producer role is TLS, not an accidental global mode.
        std::thread foreground([&]{assert(vk_shader_artifact_store(cold.data(),&key,profile,&info));});foreground.join();vk_shader_artifact_flush();
        assert(vk_shader_artifact_load(cold.data(),&key,profile,1,&a));vk_shader_artifact_free(&a);
    }
    assert(read(spvpath)==packed);
    files=0;for(const auto& entry:std::filesystem::directory_iterator(path.parent_path()))if(entry.path().extension()==".bin")++files;
    assert(files==16);
    std::cout<<"PASS: full metadata cache retains exact bytes during background scans; foreground repair, nested cancellation and TLS retain priority without removing SPIR-V\n";
    std::cout<<"PASS: artifact fields, input/state/profile, strict modules, malformed records, diagnostics, atomic publication and failure cleanup\n";
}
