// Turn observed state requests into the same ShaderKey and linked interfaces
// used by gameplay. No Cartesian product of vertex or render-target types.
#pragma once
#include "shader_warm_manifest.hpp"
#include <functional>
#include <memory>
extern "C" {
#include "../maxwell/shader.h"
}

namespace shader_warm {
using Info=std::unique_ptr<ShaderInfo,decltype(&maxwell_free)>;
using Translate=std::function<Info(const shader_archive::Shader&,const ShaderKey&)>;
using Emit=std::function<void(unsigned,const ShaderKey&,const ShaderInfo&)>;
struct Config {unsigned gather=0;bool vertex_snap=false,compute_linear=false;};
struct Stats {size_t requests=0,sources=0,unpaired_fragments=0;std::array<size_t,6> stages{};};
struct Reuse {
    std::function<bool(unsigned,const ShaderKey&)> source;
    std::function<bool(unsigned,const ShaderKey&,uint32_t&)> mask;
};

inline Stats plan(const shader_catalog::Catalog& catalog,const Selection& selection,const Config& config,
                  const Translate& translate,const Emit& emit,const Reuse& reuse={}) {
    const auto partners=shader_catalog::vertex_partners(catalog);
    std::map<unsigned,uint32_t> outputs;
    std::set<std::string> seen;
    Stats stats;stats.requests=selection.requests.size();
    auto base_key=[&](unsigned stage) {
        ShaderKey key{};key.glsl=1;key.gather_bias_bits=config.gather;
        key.vertex_snap=stage==STAGE_VERTEX && config.vertex_snap;
        key.gmem_linear_lookup=stage==STAGE_COMPUTE && config.compute_linear;
        return key;
    };
    auto checked=[&](unsigned record,const ShaderKey& key) {
        auto info=translate(catalog.records.at(record).shader,key);
        require(bool(info),"cannot allocate warm shader translation");
        if(!info->msl)throw std::runtime_error("warm record "+std::to_string(record)+": "+info->error);
        return info;
    };
    auto mask=[&](unsigned vertex) {
        auto it=outputs.find(vertex);if(it!=outputs.end())return it->second;
        auto key=base_key(STAGE_VERTEX);uint32_t cached;
        if(reuse.mask && reuse.mask(vertex,key,cached))return outputs[vertex]=cached;
        auto info=checked(vertex,key);
        return outputs[vertex]=info->out_mask;
    };
    auto submit=[&](unsigned record,const ShaderKey& key) {
        // Serialize fields explicitly: C/C++ struct padding is not a request key.
        std::string id;auto word=[&](uint32_t v){for(unsigned i=0;i<4;++i)id.push_back(char(v>>(8*i)));};
        word(record);word(key.vs_out_mask);
        id.append(reinterpret_cast<const char*>(key.attr_kind),32);
        id.append(reinterpret_cast<const char*>(key.rt_kind),8);
        id.append(reinterpret_cast<const char*>(key.image_kind),8);
        id.push_back(key.flip_y);id.push_back(key.flip_x);id.push_back(key.depth_minus_one);id.push_back(key.no_ds_write);
        if(!seen.insert(id).second)return;
        if(reuse.source && reuse.source(record,key)){++stats.sources;++stats.stages.at(catalog.records.at(record).shader.stage);return;}
        auto info=checked(record,key);emit(record,key,*info);++stats.sources;++stats.stages.at(info->stage);
    };
    auto vertex=[&](unsigned record,const Request& request) {
        auto key=base_key(STAGE_VERTEX);std::copy(request.attributes.begin(),request.attributes.end(),key.attr_kind);
        key.flip_x=request.mirror;
        // The upstream Metal pipeline key does not carry NVN origin/depth modes.
        // Four small clip variants avoid guessing these states; interface types
        // still come only from the actual recorded binding, not 3^N candidates.
        for(unsigned mode=0;mode<4;++mode){key.flip_y=mode&1;key.depth_minus_one=mode&2;submit(record,key);}
    };
    auto fragment=[&](unsigned record,const Request& request,uint32_t varying_mask) {
        auto key=base_key(STAGE_FRAGMENT);key.vs_out_mask=varying_mask;
        std::copy(request.outputs.begin(),request.outputs.end(),key.rt_kind);
        // Current Qualcomm copy-mode diagnostics can affect early depth tests.
        // The source cache collapses the second request when it changes no code.
        for(unsigned mode=0;mode<2;++mode){key.no_ds_write=mode!=0;submit(record,key);}
    };
    for(const auto& item:selection.requests) {
        const auto& request=item.request;auto stage=catalog.records.at(item.first).shader.stage;
        if(request.stage==Pair) {
            vertex(item.first,request);fragment(item.second,request,mask(item.first));
        } else if(stage==STAGE_VERTEX)vertex(item.first,request);
        else if(stage==STAGE_FRAGMENT) {
            std::set<uint32_t> masks;
            for(auto partner:partners.at(item.first))masks.insert(mask(partner));
            if(masks.empty()){++stats.unpaired_fragments;continue;}
            for(auto varying_mask:masks)fragment(item.first,request,varying_mask);
        } else if(stage==STAGE_COMPUTE) {
            auto key=base_key(stage);std::copy(request.images.begin(),request.images.end(),key.image_kind);submit(item.first,key);
        }
    }
    return stats;
}
} // namespace shader_warm
