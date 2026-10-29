// Exercise the production on-device planner without writing readiness markers.
#include "../runtime/nvn/vulkan/shader_variant_plan.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <memory>
extern "C" {
#include "../runtime/nvn/maxwell/shader.h"
}
int main(int argc,char** argv) {
    if(argc!=3 && !(argc==4 && !strcmp(argv[3],"--samples"))){std::cerr<<"usage: audit_shader_variants ROMFS OUTPUT_DIRECTORY [--samples]\n";return 2;}
    try {
        std::filesystem::create_directories(argv[2]);std::string dir=std::string(argv[2])+"/";
        shader_archive::Romfs rom(argv[1]);auto catalog=shader_catalog::read(rom,[](auto){});
        maxwell_no_int64=maxwell_gmem_map_all=maxwell_brx_window=maxwell_static_stacks=maxwell_structured=maxwell_native_half=maxwell_fast_minmax=maxwell_fp32_double=true;
        maxwell_gmem_base=0x340000000ull;maxwell_gmem_size=0xd0000000ull;
        std::vector<shader_variant_plan::Usage> usage;unsigned failed=0;
        for(unsigned i=0;i<catalog.records.size();++i) {
            auto& s=catalog.records[i].shader;ShaderKey k{};k.glsl=1;k.vs_out_mask=~0u;k.gather_bias_bits=8;
            std::unique_ptr<ShaderInfo,decltype(&maxwell_free)> p(maxwell_translate_bounded(s.binary.data(),s.constants,s.binary.size()-s.constants,s.stage,&k),maxwell_free);
            if(!p||!p->msl){std::cerr<<i<<": "<<(p?p->error:"allocation failure")<<'\n';++failed;usage.push_back({});}
            else usage.push_back({p->attr_mask,p->out_mask,p->uses_gmem,p->uses_discard,p->writes_depth,p->uses_gmem_store,p->image_format_mask});
        }
        if(failed)return 1;
        auto plan=shader_variant_plan::build(rom,catalog,usage,[](size_t n,size_t total){if(n%512==1 || n==total)std::cerr<<"model layouts "<<n<<'/'<<total<<'\n';});
        uint64_t by_stage[6]={};for(const auto& e:plan.entries)by_stage[e.stage]+=shader_variant_plan::variant_count(e);
        std::ofstream report(dir+"summary.json");
        report<<"{\"scope\":\"file_defined_variant_plan\",\"spirv_prepared\":false,\"shaders\":"<<plan.entries.size()<<",\"planned\":"<<plan.total
            <<",\"previous_enumeration\":"<<plan.baseline<<",\"vertex_types\":"<<plan.vertex_types<<",\"previous_vertex_types\":"<<plan.baseline_vertex_types
            <<",\"asset_vertices\":"<<plan.asset_vertices<<",\"general_vertices\":"<<plan.general_vertices<<",\"effect_vertices\":"<<plan.effect_vertices<<",\"model_files\":"<<plan.model_files
            <<",\"materials\":"<<plan.materials<<",\"shapes\":"<<plan.shapes<<",\"vertex_variants\":"<<by_stage[0]<<",\"fragment_variants\":"<<by_stage[1]<<",\"compute_variants\":"<<by_stage[5]<<"}\n";
        report.close();if(!report)throw std::runtime_error("cannot write plan summary");
        std::ofstream layouts(dir+"layouts.jsonl");
        for(unsigned i=0;i<plan.entries.size();++i){const auto& e=plan.entries[i];layouts<<"{\"id\":"<<i<<",\"stage\":"<<e.stage<<",\"inputs\":"<<e.usage.inputs<<",\"outputs\":"<<e.usage.outputs
            <<",\"image_formats\":"<<unsigned(e.usage.images)<<",\"type_count\":"<<e.vertex_types.size()<<",\"type_profiles\":[";
            const auto& profiles=e.vertex_types.allowed_profiles();for(unsigned j=0;j<profiles.size();++j){if(j)layouts<<',';layouts<<'"';for(auto a:profiles[j])layouts<<char('0'+a);layouts<<'"';}
            layouts<<"],\"varying_masks\":[";bool first=true;for(auto mask:e.varying_masks){if(!first)layouts<<',';first=false;layouts<<mask;}layouts<<"]}\n";}
        layouts.close();if(!layouts)throw std::runtime_error("cannot write type layouts");
        unsigned samples=0;
        std::ofstream families;if(argc==4)families.open(dir+"families.txt");
        if(argc==4)for(unsigned i=0;i<plan.entries.size();++i) {
            const auto& e=plan.entries[i];if(e.stage!=5 && i%97 && shader_variant_plan::count_bits(e.usage.inputs)<10)continue;
            std::set<uint64_t> types{0,shader_variant_plan::type_count(e)-1};
            for(unsigned memory=0;memory<shader_variant_plan::memory_count(e);++memory)for(auto mask:e.varying_masks)
            for(uint64_t image=0;image<shader_variant_plan::image_type_count(e);++image) {
                std::string base_name;
                for(auto t:types) {
                    unsigned flags=shader_variant_plan::state_count(e,memory)-1;ShaderKey k{};shader_variant_plan::key(e,t,flags,mask,k);k.gather_bias_bits=8;
                    shader_variant_plan::image_key(e,image,k);
                    maxwell_gmem_map_all=memory!=0;maxwell_gmem_miss_log=memory==2;const auto& s=catalog.records[i].shader;
                    std::unique_ptr<ShaderInfo,decltype(&maxwell_free)> p(maxwell_translate_bounded(s.binary.data(),s.constants,s.binary.size()-s.constants,s.stage,&k),maxwell_free);
                    if(!p||!p->msl)throw std::runtime_error(p?p->error:"allocation failure");
                    auto name="sample-"+std::to_string(samples++)+(e.stage==0?".vert":e.stage==1?".frag":".comp");std::ofstream file(dir+name);file<<p->msl;file.close();if(!file)throw std::runtime_error("cannot write sample");
                    if(base_name.empty())base_name=name; // type 0 is the all-float member
                    families<<base_name<<' '<<name<<' ';
                    for(unsigned a=0;a<32;++a)families<<char('0'+(e.stage==0?k.attr_kind[a]:e.stage==1 && a<8?k.rt_kind[a]:0));
                    families<<'\n';
                }
            }
        }
        if(argc==4){families.close();if(!families)throw std::runtime_error("cannot write sample families");}
        std::cout<<"planned "<<plan.total<<" / previous "<<plan.baseline<<", vertex types "<<plan.vertex_types<<" / "<<plan.baseline_vertex_types<<", translated samples "<<samples<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
