#include "../runtime/nvn/vulkan/shader_warm_requests.hpp"
#include <cassert>
#include <iostream>
using shader_warm::RequestCache;
using shader_warm::RawKey;
static void write_word(shader_archive::Bytes& data,size_t at,uint32_t value) {
    for(unsigned i=0;i<4;++i)data.at(at+i)=value>>(8*i);
}
int main(int argc,char** argv) {
    assert(argc==2);std::string root=argv[1];std::filesystem::create_directories(root);
    RawKey a{},b{},c{};a[0]=1;b[0]=2;c[0]=3;RequestCache::Result result;
    std::string path;
    {
        RequestCache cache(root,"translator/planner/profile-A");path=cache.filename();
        assert(cache.size()==0);cache.put(a,{11,5,0});cache.put(a,{11,5,0});cache.put(b,{22,7,1});
        shader_archive::Shader shader;shader.stage=0;shader.control.resize(0x880);shader.binary.resize(8);
        write_word(shader.control,0x20,0x880);write_word(shader.control,0x704,8);
        ShaderKey state{};state.glsl=1;auto key=cache.key(shader,state);
        auto changed=state;changed.attr_kind[31]=1;assert(cache.key(shader,changed)!=key);
        changed=state;changed.rt_kind[7]=1;assert(cache.key(shader,changed)!=key);
        changed=state;changed.image_kind[7]=2;assert(cache.key(shader,changed)!=key);
        changed=state;changed.vs_out_mask=1;assert(cache.key(shader,changed)!=key);
        changed=state;changed.gather_bias_bits=8;assert(cache.key(shader,changed)!=key);
        changed=state;changed.vertex_snap=true;assert(cache.key(shader,changed)!=key);
        changed=state;changed.gmem_linear_lookup=true;assert(cache.key(shader,changed)!=key);
        changed=state;changed.flip_x=true;assert(cache.key(shader,changed)!=key);
        changed=state;changed.flip_y=true;assert(cache.key(shader,changed)!=key);
        changed=state;changed.depth_minus_one=true;assert(cache.key(shader,changed)!=key);
        changed=state;changed.no_ds_write=true;assert(cache.key(shader,changed)!=key);
        auto other=shader;other.binary[0]=1;assert(cache.key(other,state)!=key);
    }
    assert(std::filesystem::file_size(path)==RequestCache::HeaderBytes+2*RequestCache::EntryBytes);
    {
        RequestCache cache(root,"translator/planner/profile-A");assert(cache.size()==2);
        assert(cache.get(a,result) && result.source==11 && result.outputs==5 && result.stage==0);
        RequestCache other(root,"translator/planner/profile-B");assert(!other.get(a,result));
        assert(other.filename()!=path);
    }
    // Crash during an append: retain the earlier entry and make later appends readable.
    std::filesystem::resize_file(path,std::filesystem::file_size(path)-17);
    {RequestCache cache(root,"translator/planner/profile-A");assert(cache.get(a,result) && !cache.get(b,result));cache.put(c,{33,9,5});}
    {RequestCache cache(root,"translator/planner/profile-A");assert(cache.get(a,result) && cache.get(c,result) && result.source==33);}
    // A damaged complete row cannot supply a forged source key or varying mask.
    {std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);file.seekp(RequestCache::HeaderBytes+RequestCache::EntryBytes+40);file.put('\x7f');}
    {RequestCache cache(root,"translator/planner/profile-A");assert(cache.get(a,result) && !cache.get(c,result));cache.put(b,{44,3,1});}
    {RequestCache cache(root,"translator/planner/profile-A");assert(cache.get(b,result) && result.source==44);}
    // Metadata failure never prevents otherwise useful preparation in this process.
    {std::ofstream blocked(root+"/blocked");blocked<<"file";}
    {RequestCache cache(root+"/blocked","identity");cache.put(a,{55,1,0});assert(cache.get(a,result) && result.source==55);}
    {
        RequestCache cache(root+"/blocked","bounded");
        for(size_t i=0;i<RequestCache::MaxEntries+1;++i) {
            RawKey key{};for(unsigned j=0;j<8;++j)key[j]=uint64_t(i)>>(8*j);cache.put(key,{i+1,0,0});
        }
        assert(cache.size()==RequestCache::MaxEntries);
    }
    std::cout<<"PASS: independent requests, profile isolation, full state identity, bounded history, interrupted/corrupt append recovery, unwritable metadata\n";
}
