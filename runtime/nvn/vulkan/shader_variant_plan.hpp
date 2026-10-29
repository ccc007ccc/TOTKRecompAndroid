// File-defined interfaces plus conservative choices for unresolved fields.
// No runtime compiler fallback: unexpected bindings remain explicit cache misses.
#pragma once
#include "shader_material.hpp"
#include "../vertex_format.h"

namespace shader_variant_plan {
using shader_archive::require;
using Kinds=std::array<uint8_t,32>;
struct Usage {uint32_t inputs=0,outputs=0;bool gmem=false,discard=false,depth=false,store=false;uint8_t images=0;};
inline uint64_t power3(unsigned n){uint64_t v=1;while(n--){require(v<=UINT64_MAX/3,"shader variant count overflow");v*=3;}return v;}
inline unsigned count_bits(uint32_t n){unsigned out=0;while(n){n&=n-1;++out;}return out;}
inline uint32_t targets(uint32_t outputs){uint32_t mask=0;for(unsigned i=0;i<8;++i)if((outputs>>(i*4))&15)mask|=1u<<i;return mask;}
inline bool covers(const Kinds& a,const Kinds& b){for(unsigned i=0;i<a.size();++i)if((a[i]&b[i])!=b[i])return false;return true;}
inline void merge(std::vector<Kinds>& profiles,const Kinds& p) {
    for(const auto& q:profiles)if(covers(q,p))return;
    profiles.erase(std::remove_if(profiles.begin(),profiles.end(),[&](const Kinds& q){return covers(p,q);}),profiles.end());profiles.push_back(p);
}
inline Kinds open_profile(uint32_t inputs) {
    Kinds out;out.fill(1); // unused and locations above the renderer's 16 bindings are float
    for(unsigned i=0;i<NVN_VERTEX_BINDINGS;++i)if(inputs&(1u<<i))out[i]=7;
    return out;
}
inline Kinds asset_profile(const std::vector<shader_material::Input>& inputs,uint32_t used) {
    auto out=open_profile(used);
    for(const auto& a:inputs)if(a.location>=0 && a.location<NVN_VERTEX_BINDINGS && (used&(1u<<a.location)) && a.format>=0) {
        auto kind=bfres_vertex_format_kind(a.format);
        // A bound integer layout can also be disabled; the renderer then binds float defaults.
        if(kind!=NVN_VERTEX_UNKNOWN)out[a.location]=1u|(1u<<kind);
    }
    return out;
}
inline void expand_at(const Kinds& allowed,unsigned at,Kinds& value,std::set<Kinds>& out) {
    if(at==allowed.size()){out.insert(value);return;}
    require(allowed[at] && !(allowed[at]&~7u),"invalid vertex type choices");
    for(unsigned k=0;k<3;++k)if(allowed[at]&(1u<<k)){value[at]=k;expand_at(allowed,at+1,value,out);}
}
inline std::vector<Kinds> expand(const std::vector<Kinds>& profiles) {
    std::set<Kinds> out;Kinds value{};for(const auto& p:profiles)expand_at(p,0,value,out);return {out.begin(),out.end()};
}
inline uint64_t compact(const Kinds& p){uint64_t out=0;for(unsigned i=0;i<NVN_VERTEX_BINDINGS;++i)out|=uint64_t(p[i])<<(i*3);return out;}
inline Kinds unpack(uint64_t packed){Kinds out;out.fill(1);for(unsigned i=0;i<NVN_VERTEX_BINDINGS;++i)out[i]=(packed>>(i*3))&7;return out;}
inline void merge(std::vector<uint64_t>& profiles,uint64_t p) {
    for(auto q:profiles)if((q&p)==p)return;
    profiles.erase(std::remove_if(profiles.begin(),profiles.end(),[&](uint64_t q){return (q&p)==q;}),profiles.end());profiles.push_back(p);
}
// A counted decision graph represents the exact union of allowed layouts. A
// 14-input general shader has 4,782,969 layouts, but only 32 graph nodes. Preserve
// lexicographic order and deduplication without allocating one Kinds per layout.
class VertexTypes {
    struct Node {std::array<uint32_t,3> next{};uint64_t count=0;};
    std::vector<Kinds> profiles;
    std::vector<Node> nodes;
    uint32_t root=0;
    using State=std::pair<unsigned,std::vector<unsigned>>;
    uint32_t build(unsigned at,const std::vector<unsigned>& active,std::map<State,uint32_t>& memo) {
        if(active.empty())return 0;if(at==32)return 1;
        auto [it,inserted]=memo.emplace(State{at,active},0);if(!inserted)return it->second;
        Node node;
        for(unsigned k=0;k<3;++k){std::vector<unsigned> subset;
            for(auto p:active)if(profiles[p][at]&(1u<<k))subset.push_back(p);
            node.next[k]=build(at+1,subset,memo);auto n=nodes[node.next[k]].count;
            require(n<=UINT64_MAX-node.count,"vertex layout count overflow");node.count+=n;
        }
        require(nodes.size()<UINT32_MAX,"vertex layout graph too large");
        uint32_t result=nodes.size();nodes.push_back(node);it->second=result;return result;
    }
public:
    VertexTypes()=default;
    VertexTypes& operator=(const std::vector<Kinds>& values){
        std::vector<Kinds> allowed;for(const auto& value:values){Kinds p;
            for(unsigned i=0;i<32;++i){require(value[i]<3,"invalid vertex scalar type");p[i]=1u<<value[i];}merge(allowed,p);}
        assign_profiles(allowed);return *this;
    }
    void assign_profiles(const std::vector<Kinds>& allowed){
        profiles=allowed;for(const auto&p:profiles)for(auto kind:p)require(kind && !(kind&~7u),"invalid vertex type choices");
        nodes.assign(2,Node{});nodes[1].count=1;std::vector<unsigned> active;for(unsigned i=0;i<profiles.size();++i)active.push_back(i);
        std::map<State,uint32_t> memo;root=build(0,active,memo);
    }
    uint64_t size()const{return nodes.empty()?0:nodes[root].count;}
    const std::vector<Kinds>& allowed_profiles()const{return profiles;}
    size_t node_count()const{return nodes.size();}
    Kinds at(uint64_t index)const{
        require(index<size(),"vertex layout index out of bounds");Kinds value{};auto node=root;
        for(unsigned at=0;at<32;++at)for(unsigned k=0;k<3;++k){auto next=nodes[node].next[k];
            if(index<nodes[next].count){value[at]=k;node=next;break;}index-=nodes[next].count;}
        return value;
    }
};
struct Entry {uint32_t stage=0;Usage usage;VertexTypes vertex_types;std::set<uint32_t> varying_masks;bool asset_types=false;};
struct Plan {
    std::vector<Entry> entries;
    uint64_t total=0,baseline=0,vertex_types=0,baseline_vertex_types=0;
    size_t model_files=0,materials=0,shapes=0,asset_vertices=0,general_vertices=0,effect_vertices=0;
};
// TotK's VFXB v51 particle/plugin streams have a fixed float4 format. Verified
// in the 1.4.3 executable's vertex-state builder: 0x378588/0x378604 (13 names
// from table 0x38ff290), and 0x378b70..0x378c4c (plugin 0..5), format 0x1905.
// Mesh inputs (sysPosAttr, etc.) come from primitive buffers and stay unresolved.
inline bool effect_float_attribute(const std::string& name) {
    static const std::set<std::string> names={"sysLocalPosAttr","sysLocalVecAttr","sysLocalDiffAttr","sysScaleAttr",
        "sysRandomAttr","sysInitRotateAttr","sysColor0Attr","sysColor1Attr","sysEmtMat0Attr","sysEmtMat1Attr",
        "sysEmtMat2Attr","sysEmitterPluginReserved","sysDefaultCustomAttr"};
    return names.count(name) || (name.size()==21 && name.compare(0,20,"sysEmitterPluginAttr")==0 && name[20]>='0' && name[20]<='5');
}
inline Kinds effect_profile(const std::vector<shader_catalog::Attribute>& attributes,uint32_t used) {
    auto out=open_profile(used);std::array<uint8_t,32> known{},unknown{};
    for(const auto& a:attributes)if(a.location>=0 && a.location<NVN_VERTEX_BINDINGS && (used&(1u<<a.location))){
        if(effect_float_attribute(a.name))known[a.location]=1;else unknown[a.location]=1;
    }
    for(unsigned i=0;i<32;++i)if(known[i] && !unknown[i])out[i]=1;
    return out;
}
inline unsigned state_count(const Entry& e,unsigned memory) {
    if(e.stage==0)return 4; // depth convention and Y flip; draw_ never sets flip_x
    if(e.stage==5)return e.usage.gmem?2:1;
    return memory==2 && e.usage.gmem && e.usage.discard && !e.usage.depth && !e.usage.store?2:1;
}
inline unsigned memory_count(const Entry& e){return e.usage.gmem?3:1;}
inline uint64_t type_count(const Entry& e){return e.stage==0?e.vertex_types.size():e.stage==1?power3(count_bits(targets(e.usage.outputs))):1;}
inline uint64_t image_type_count(const Entry& e){return power3(count_bits(e.usage.images));}
template<class Key> inline void image_key(const Entry& e,uint64_t type,Key& k) {
    for(unsigned i=0;i<8;++i)if(e.usage.images&(1u<<i)){k.image_kind[i]=type%3;type/=3;}
}
inline uint64_t variant_count(const Entry& e) {
    uint64_t states=0;for(unsigned memory=0;memory<memory_count(e);++memory)states+=state_count(e,memory);
    auto types=type_count(e),images=image_type_count(e);require(types<=UINT64_MAX/images,"image format count overflow");types*=images;
    require(types<=UINT64_MAX/states,"shader state count overflow");
    auto count=types*states;require(!e.varying_masks.empty() && count<=UINT64_MAX/e.varying_masks.size(),"shader link count overflow");return count*e.varying_masks.size();
}
inline Plan build(shader_archive::Romfs& rom,const shader_catalog::Catalog& catalog,const std::vector<Usage>& usage,
                  const std::function<void(size_t,size_t)>& progress={}) {
    require(usage.size()==catalog.records.size(),"shader usage/catalog count mismatch");
    Plan out;out.entries.resize(usage.size());std::vector<std::vector<uint64_t>> profiles(usage.size());
    std::vector<uint64_t> used_masks;uint64_t defaults=compact(open_profile(0));
    std::set<uint32_t> all_outputs;auto partners=shader_catalog::vertex_partners(catalog);
    for(unsigned i=0;i<usage.size();++i){out.entries[i].stage=catalog.records[i].shader.stage;out.entries[i].usage=usage[i];if(out.entries[i].stage==0)all_outputs.insert(usage[i].outputs);
        uint64_t mask=0;for(unsigned a=0;a<NVN_VERTEX_BINDINGS;++a)if(usage[i].inputs&(1u<<a))mask|=7ull<<(a*3);used_masks.push_back(mask);}
    require(!all_outputs.empty(),"shader catalog has no vertex stage");
    auto strings=shader_material::external_strings(shader_material::metadata(rom.read("Shader/ExternalBinaryString.bfres.mc")));
    auto files=rom.files("Model/",".bfres.mc"),zs=rom.files("Model/",".bfres.zs");files.insert(files.end(),zs.begin(),zs.end());
    shader_archive::Decoder decoder;decoder.init(rom.read("Pack/ZsDic.pack.zs"));out.model_files=files.size();
    shader_material::Matcher matcher(catalog);
    for(unsigned file=0;file<files.size();++file) {
        try {
            auto raw=rom.read(files[file]);auto bytes=shader_archive::Romfs::ends(files[file],".mc")?shader_material::metadata(raw):decoder.decode(raw);
            for(const auto& model:shader_material::read(bytes,strings)) {
                out.materials+=model.materials.size();out.shapes+=model.shapes.size();
                for(unsigned mi=0;mi<model.materials.size();++mi) {
                    auto match=matcher.find(model.materials[mi]);
                    require(match.unresolved.empty() && !match.programs.empty(),"material shader selection unresolved");
                    const auto& shader=catalog.models.at(match.model);std::set<unsigned> vertices;
                    for(auto program:match.programs){auto vertex=shader.programs.at(program).stages[0];if(vertex>=0)vertices.insert(vertex);}
                    for(const auto& shape:model.shapes)if(shape.material==mi) {
                        auto inputs=shader_material::inputs(shape,model.materials[mi],shader);
                        auto full=compact(asset_profile(inputs,~0u));
                        for(auto vertex:vertices){auto mask=used_masks.at(vertex);merge(profiles.at(vertex),(full&mask)|(defaults&~mask));}
                    }
                }
            }
        } catch(const std::exception& e){throw std::runtime_error(files[file]+": "+e.what());}
        if(progress && (!(file%128) || file+1==files.size()))progress(file+1,files.size());
    }
    std::set<unsigned> effect_vertices;
    for(const auto& group:catalog.named_programs)if(group.source.rfind("Effect/",0)==0)
        for(const auto& program:group.variations)if(program.stages[0]>=0){
            unsigned id=program.stages[0];auto p=effect_profile(program.vertex_inputs,usage[id].inputs);
            // A shared effect must not erase a previously unresolved library use.
            if(profiles[id].empty())for(const auto& source:catalog.records[id].sources)if(source.rfind("Effect/",0)!=0){
                merge(profiles[id],compact(open_profile(usage[id].inputs)));break;
            }
            merge(profiles[id],compact(p));effect_vertices.insert(id);
        }
    out.effect_vertices=effect_vertices.size();
    for(unsigned i=0;i<out.entries.size();++i) {
        auto& e=out.entries[i];std::set<uint32_t> old_masks;
        if(e.stage==0) {
            e.asset_types=!profiles[i].empty();if(e.asset_types)++out.asset_vertices;else{++out.general_vertices;profiles[i].push_back(compact(open_profile(e.usage.inputs)));}
            std::vector<Kinds> allowed;for(auto p:profiles[i])allowed.push_back(unpack(p));
            e.vertex_types.assign_profiles(allowed);out.vertex_types+=e.vertex_types.size();out.baseline_vertex_types+=power3(count_bits(e.usage.inputs));
        }
        if(e.stage==1) {
            for(auto vertex:partners[i])e.varying_masks.insert(usage.at(vertex).outputs&e.usage.inputs);
            for(auto mask:all_outputs)old_masks.insert(mask&e.usage.inputs);
            if(e.varying_masks.empty())e.varying_masks=old_masks;
        } else {e.varying_masks.insert(0);old_masks.insert(0);}
        uint64_t n=variant_count(e);require(n<=UINT64_MAX-out.total,"total shader variant overflow");out.total+=n;
        uint64_t old_types=power3(count_bits(e.stage==0?e.usage.inputs:e.stage==1?targets(e.usage.outputs):0));
        out.baseline+=old_types*(e.stage==0?8:e.stage==1?4:2)*old_masks.size()*3;
    }
    return out;
}
// Shared by the preparation loop and audit: the estimate describes actual work.
template<class Key> inline void key(const Entry& e,uint64_t type,unsigned flags,uint32_t mask,Key& k) {
    k.glsl=1;k.vs_out_mask=mask;
    if(e.stage==0){auto kinds=e.vertex_types.at(type);std::copy(kinds.begin(),kinds.end(),k.attr_kind);k.depth_minus_one=flags&1;k.flip_y=flags&2;}
    else if(e.stage==1){auto rt=targets(e.usage.outputs);for(unsigned i=0;rt;++i,rt>>=1)if(rt&1){k.rt_kind[i]=type%3;type/=3;}k.no_ds_write=flags&1;}
    else k.gmem_linear_lookup=flags&1;
}
} // namespace shader_variant_plan
