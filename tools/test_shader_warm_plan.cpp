#include "../runtime/nvn/vulkan/shader_warm_plan.hpp"
#include <cassert>
#include <iostream>

int main() {
    shader_archive::Bytes bytes(16+shader_warm::RecordBytes);
    memcpy(bytes.data(),"TKSWARM1",8);bytes[8]=shader_warm::RecordBytes;bytes[12]=1;
    bytes[16]=shader_warm::Pair;bytes[17]=1;bytes[20]=1;bytes[52]=2;
    auto parsed=shader_warm::parse(bytes);assert(parsed.size()==1 && parsed[0].title);
    auto rejected=[](const shader_archive::Bytes& b){try{shader_warm::parse(b);return false;}catch(const std::runtime_error&){return true;}};
    for(auto at:{0u,8u,12u,16u,17u,18u,84u}){auto broken=bytes;broken[at]=254;assert(rejected(broken));}
    auto truncated=bytes;truncated.pop_back();assert(rejected(truncated));
    auto zero=bytes;zero[20]=0;assert(rejected(zero));

    shader_catalog::Catalog catalog;catalog.records.resize(4);
    catalog.records[0].shader.stage=0;catalog.records[1].shader.stage=1;
    catalog.records[2].shader.stage=5;catalog.records[3].shader.stage=1;
    shader_catalog::Program pair;pair.stages[0]=0;pair.stages[1]=1;
    shader_catalog::NamedProgram group;group.variations.push_back(pair);catalog.named_programs.push_back(group);
    shader_warm::Selection selection;shader_warm::Request request;
    request.stage=shader_warm::Pair;request.attributes[2]=2;request.outputs[0]=1;
    selection.requests.push_back({request,0,1});selection.requests.push_back({request,0,1});
    request.mirror=true;selection.requests.push_back({request,0,1});
    request={};request.stage=5;request.images[1]=1;selection.requests.push_back({request,2});
    request={};request.stage=1;selection.requests.push_back({request,3});
    size_t emitted=0;std::set<unsigned> clips;
    auto stats=shader_warm::plan(catalog,selection,{8,true,true},[](const shader_archive::Shader& s,const ShaderKey& key){
        assert(key.glsl && key.gather_bias_bits==8);
        auto p=static_cast<ShaderInfo*>(calloc(1,sizeof(ShaderInfo)));p->stage=s.stage;p->msl=strdup("test");p->out_mask=s.stage==0?5:1;
        return shader_warm::Info(p,maxwell_free);
    },[&](unsigned record,const ShaderKey& key,const ShaderInfo&){
        ++emitted;
        if(record==0){assert(key.attr_kind[2]==2 && key.vertex_snap);clips.insert(key.flip_y+2*key.depth_minus_one+4*key.flip_x);}
        if(record==1)assert(key.vs_out_mask==5 && key.rt_kind[0]==1);
        if(record==2)assert(key.gmem_linear_lookup && key.image_kind[1]==1);
    });
    assert(emitted==11 && stats.sources==11 && stats.unpaired_fragments==1 && clips.size()==8);
    assert(stats.stages[0]==8 && stats.stages[1]==2 && stats.stages[5]==1);
    catalog.records[2].shader.control.resize(0x880);
    catalog.records[2].shader.control[0x20]=0x80;catalog.records[2].shader.control[0x21]=8;
    auto defaults=shader_warm::default_compute_requests(catalog);
    assert(defaults.size()==1 && defaults[0].stage==5 && defaults[0].first!=shader_warm::RawKey{});
    assert((defaults[0].images==std::array<uint8_t,8>{}));
    std::cout<<"PASS: manifest bounds, title flags, numeric bindings, linked varyings, dedup, clip states, compute profile\n";
}
