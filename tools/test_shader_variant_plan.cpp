#include "../runtime/nvn/vulkan/shader_variant_plan.hpp"
extern "C" {
#include "../runtime/nvn/maxwell/shader.h"
}
#include <cassert>
#include <iostream>
using namespace shader_variant_plan;
int main() {
    // Model positions are float, bone indices uint or the unbound float default;
    // an absent/custom input remains all three types. Unread fields do not multiply work.
    auto p=asset_profile({{"position","_p0",0,0x518},{"indices","_i0",2,0x309},{"unused","_u0",8,0x112}},7);
    assert(p[0]==1 && p[1]==7 && p[2]==5 && p[8]==1);
    std::vector<Kinds> profiles;merge(profiles,p);auto narrower=p;narrower[1]=1;merge(profiles,narrower);assert(profiles.size()==1);
    auto types=expand(profiles);assert(types.size()==6);
    std::vector<uint64_t> packed;merge(packed,compact(p));merge(packed,compact(narrower));assert(packed.size()==1 && unpack(packed[0])==p);
    for(const auto& k:types)assert(k[0]==0 && k[1]<3 && (k[2]==0 || k[2]==2) && k[8]==0);
    auto unknown=asset_profile({{"custom","x",0,0xffff}},1);assert(expand({unknown}).size()==3);
    auto upper=open_profile(1u<<20);assert(expand({upper}).size()==1);
    // The shader interface and pipeline agree even if an integer format is
    // present in an attribute state whose stream is outside the 16 bindings.
    for(unsigned format=0;format<256;++format)for(unsigned stream=0;stream<32;++stream) {
        auto kind=nvn_vertex_state_kind(format|(stream<<8));
        assert(kind==(!format || stream>=16?NVN_VERTEX_FLOAT:nvn_vertex_format_kind(format)));
    }
    assert(nvn_vertex_state_kind(0x03)==NVN_VERTEX_UINT && nvn_vertex_state_kind(0x04)==NVN_VERTEX_SINT);
    auto particle=effect_profile({{"sysLocalPosAttr",0,0},{"sysEmitterPluginAttr5",1,1},{"sysPosAttr",2,2},{"custom",3,3}},31);
    assert(particle[0]==1 && particle[1]==1 && particle[2]==7 && particle[3]==7 && particle[4]==7);
    assert(!effect_float_attribute("sysEmitterPluginAttr6") && !effect_float_attribute("sysEmitterPluginAttr05"));
    auto conflict=effect_profile({{"sysLocalPosAttr",0,0},{"custom",1,0}},1);assert(conflict[0]==7);
    Entry e;e.stage=0;e.vertex_types=types;e.varying_masks={0};e.usage.inputs=7;
    assert(variant_count(e)==24);e.usage.gmem=true;assert(variant_count(e)==72);
    ShaderKey k{};key(e,5,3,0,k);assert(k.depth_minus_one && k.flip_y && !k.flip_x && k.attr_kind[2]==2);
    e={};e.stage=1;e.usage.outputs=0x0f00f;e.varying_masks={3,7};assert(type_count(e)==9 && variant_count(e)==18);
    e.usage.gmem=true;e.usage.discard=true;assert(state_count(e,0)==1 && state_count(e,1)==1 && state_count(e,2)==2);
    k={};key(e,5,1,3,k);assert(k.rt_kind[0]==2 && k.rt_kind[3]==1 && k.no_ds_write && !k.flip_x && k.vs_out_mask==3);
    e.usage.depth=true;assert(state_count(e,2)==1);e.usage.depth=false;e.usage.store=true;assert(state_count(e,2)==1);
    e={};e.stage=5;e.varying_masks={0};assert(variant_count(e)==1);e.usage.gmem=true;assert(variant_count(e)==6);
    e.usage.images=5;assert(image_type_count(e)==9 && variant_count(e)==54);
    for(uint64_t i=0;i<9;++i){k={};image_key(e,i,k);assert(k.image_kind[0]==i%3 && k.image_kind[2]==i/3 && k.image_kind[1]==0);}
    VertexTypes huge;huge.assign_profiles({open_profile((1u<<14)-1)});
    assert(huge.size()==4782969 && huge.node_count()<=34);
    auto first=huge.at(0),last=huge.at(huge.size()-1);for(unsigned i=0;i<32;++i){assert(first[i]==0 && last[i]==(i<14?2:0));}
    bool rejected=false;try{huge.at(huge.size());}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    // Independently enumerate small, overlapping profile unions to check order,
    // exact cardinality and deduplication of the compressed graph.
    for(unsigned seed=1;seed<80;++seed){std::vector<Kinds> allowed;uint32_t state=seed;
        for(unsigned i=0;i<6;++i){Kinds p;p.fill(1);for(unsigned j=0;j<5;++j){state=state*1664525u+1013904223u;p[j]=1+(state%7);}allowed.push_back(p);}
        auto expected=expand(allowed);VertexTypes compact;compact.assign_profiles(allowed);assert(compact.size()==expected.size());
        for(unsigned i=0;i<expected.size();++i)assert(compact.at(i)==expected[i]);
    }
    std::cout<<"PASS: asset types/defaults, unknown fields, profile subsumption, binding types and state enumeration\n";
}
