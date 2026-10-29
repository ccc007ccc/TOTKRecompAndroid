// Native, read-only game asset linkage audit. Writes metadata, never game code.
// Build/run inside dev; the same sources can be built with the Android NDK.
#include "../runtime/nvn/vulkan/shader_material.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <filesystem>

static void quoted(std::ostream& out,const std::string& s) {
    out<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')out<<'\\'<<char(c);
        else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;else out<<char(c);}out<<'"';
}
template<class T> static void numbers(std::ostream& out,const T& values) {
    out<<'[';bool first=true;for(auto n:values){if(!first)out<<',';first=false;out<<n;}out<<']';
}
template<class T> static void strings(std::ostream& out,const T& values) {
    out<<'[';bool first=true;for(const auto& s:values){if(!first)out<<',';first=false;quoted(out,s);}out<<']';
}
static void properties(std::ostream& out,const std::map<std::string,std::string>& values) {
    out<<'{';bool first=true;for(const auto& [k,v]:values){if(!first)out<<',';first=false;quoted(out,k);out<<':';quoted(out,v);}out<<'}';
}
static void catalog_json(std::ostream& out,const shader_catalog::Catalog& catalog) {
    out<<"{\"schema\":1,\"records\":[";
    for(unsigned i=0;i<catalog.records.size();++i) {
        if(i)out<<',';const auto& r=catalog.records[i];uint64_t h=0xcbf29ce484222325ull;
        for(auto b:shader_archive::input_identity(r.shader))h=(h^b)*0x100000001b3ull;
        out<<"{\"id\":"<<i<<",\"stage\":"<<r.shader.stage<<",\"input_hash\":\""<<std::hex<<h<<std::dec<<"\",\"sources\":";
        strings(out,r.sources);out<<'}';
    }
    out<<"],\"models\":[";
    for(unsigned i=0;i<catalog.models.size();++i) {
        if(i)out<<',';const auto& m=catalog.models[i];out<<"{\"id\":"<<i<<",\"source\":";quoted(out,m.source);
        out<<",\"archive\":";quoted(out,m.archive);out<<",\"name\":";quoted(out,m.name);out<<",\"options\":[";
        for(unsigned j=0;j<m.options.size();++j){if(j)out<<',';const auto& o=m.options[j];out<<"{\"name\":";quoted(out,o.name);
            out<<",\"dynamic\":"<<(o.dynamic?"true":"false")<<",\"word\":"<<o.word<<",\"mask\":"<<o.mask<<",\"shift\":"<<o.shift
               <<",\"default\":"<<o.default_choice<<",\"choices\":";strings(out,o.choices);out<<'}';}
        out<<"],\"attributes\":[";
        for(unsigned j=0;j<m.attributes.size();++j){if(j)out<<',';const auto& a=m.attributes[j];out<<"{\"name\":";quoted(out,a.name);out<<",\"index\":"<<a.index<<",\"location\":"<<a.location<<'}';}
        out<<"],\"programs\":[";
        for(unsigned j=0;j<m.programs.size();++j){if(j)out<<',';const auto& p=m.programs[j];out<<"{\"id\":"<<j<<",\"variation\":"<<p.variation
            <<",\"attribute_mask\":"<<p.attribute_mask<<",\"key\":";numbers(out,p.key);out<<",\"stages\":";numbers(out,p.stages);out<<'}';}
        out<<"]}";
    }
    out<<"],\"named_programs\":[";
    for(unsigned i=0;i<catalog.named_programs.size();++i) {
        if(i)out<<',';const auto& n=catalog.named_programs[i];out<<"{\"source\":";quoted(out,n.source);out<<",\"archive\":";quoted(out,n.archive);out<<",\"name\":";quoted(out,n.name);out<<",\"variations\":[";
        for(unsigned j=0;j<n.variations.size();++j){if(j)out<<',';numbers(out,n.variations[j].stages);}
        out<<"],\"vertex_inputs\":[";
        for(unsigned j=0;j<n.variations.size();++j){if(j)out<<',';out<<'[';
            const auto& attrs=n.variations[j].vertex_inputs;
            for(unsigned a=0;a<attrs.size();++a){if(a)out<<',';out<<"{\"name\":";quoted(out,attrs[a].name);out<<",\"location\":"<<attrs[a].location<<'}';}out<<']';}
        out<<"]}";
    }
    out<<"]}\n";
}
int main(int argc,char** argv) {
    bool graph_only=argc==4 && !strcmp(argv[3],"--catalog-only");
    if(argc!=3 && !graph_only){std::cerr<<"usage: audit_shader_catalog ROMFS_IMAGE OUTPUT_DIRECTORY [--catalog-only]\n";return 2;}
    try {
        std::filesystem::create_directories(argv[2]);std::string dir=std::string(argv[2])+"/";
        shader_archive::Romfs rom(argv[1]);auto catalog=shader_catalog::read(rom,[](auto s){std::cerr<<s<<'\n';});
        std::ofstream graph(dir+"catalog.json");catalog_json(graph,catalog);graph.close();if(!graph)throw std::runtime_error("cannot write catalog");
        if(graph_only)return 0;
        auto external=shader_material::external_strings(shader_material::metadata(rom.read("Shader/ExternalBinaryString.bfres.mc")));
        std::ofstream rows(dir+"materials.jsonl"),failures(dir+"unresolved.jsonl");
        auto files=rom.files("Model/",".bfres.mc"),zs=rom.files("Model/",".bfres.zs");files.insert(files.end(),zs.begin(),zs.end());
        shader_archive::Decoder decoder;decoder.init(rom.read("Pack/ZsDic.pack.zs"));
        unsigned models=0,materials=0,shapes=0,matched=0,failed=0,no_match=0;
        std::set<int> vertex_ids,fragment_ids;std::set<std::pair<int,int>> pairs;
        std::map<uint16_t,unsigned> formats;
        for(unsigned file=0;file<files.size();++file) {
            try {
                auto raw=rom.read(files[file]);
                auto data=shader_archive::Romfs::ends(files[file],".mc")?shader_material::metadata(raw):decoder.decode(raw);
                auto parsed=shader_material::read(data,external);
                for(const auto& m:parsed) {
                    ++models;shapes+=m.shapes.size();
                    for(unsigned mi=0;mi<m.materials.size();++mi) {
                        const auto& mat=m.materials[mi];auto match=shader_material::match(mat,catalog);++materials;
                        if(match.unresolved.empty() && !match.programs.empty())++matched;else ++no_match;
                        rows<<"{\"source\":";quoted(rows,files[file]);rows<<",\"model\":";quoted(rows,m.name);rows<<",\"material\":";quoted(rows,mat.name);
                        rows<<",\"shader_archive\":";quoted(rows,mat.archive);rows<<",\"shader_model\":";quoted(rows,mat.shader_model);
                        rows<<",\"catalog_model\":"<<match.model<<",\"options\":";properties(rows,mat.options);rows<<",\"default_options\":";strings(rows,mat.default_options);
                        rows<<",\"candidate_programs\":";numbers(rows,match.programs);rows<<",\"unresolved\":";strings(rows,match.unresolved);rows<<",\"shapes\":[";
                        bool first=true;
                        for(const auto& s:m.shapes)if(s.material==mi) {
                            if(!first)rows<<',';first=false;rows<<"{\"name\":";quoted(rows,s.name);rows<<",\"inputs\":[";
                            if(match.model>=0){auto inputs=shader_material::inputs(s,mat,catalog.models[match.model]);
                                for(unsigned k=0;k<inputs.size();++k){if(k)rows<<',';const auto& a=inputs[k];rows<<"{\"shader_name\":";quoted(rows,a.shader_name);
                                    rows<<",\"vertex_name\":";quoted(rows,a.vertex_name);rows<<",\"location\":"<<a.location<<",\"format\":"<<a.format<<'}';}}
                            rows<<"]}";
                            for(const auto& a:s.attributes)++formats[a.format];
                        }
                        rows<<"]}\n";
                        if(match.model>=0)for(auto id:match.programs){auto& p=catalog.models[match.model].programs[id];
                            if(p.stages[0]>=0)vertex_ids.insert(p.stages[0]);if(p.stages[1]>=0)fragment_ids.insert(p.stages[1]);
                            if(p.stages[0]>=0 && p.stages[1]>=0)pairs.emplace(p.stages[0],p.stages[1]);}
                    }
                }
            } catch(const std::exception& e) {
                ++failed;failures<<"{\"source\":";quoted(failures,files[file]);failures<<",\"error\":";quoted(failures,e.what());failures<<"}\n";
            }
            if(file%256==0)std::cerr<<"model files "<<file+1<<'/'<<files.size()<<", materials "<<materials<<", matched "<<matched<<", parse failures "<<failed<<'\n';
        }
        rows.close();failures.close();if(!rows||!failures)throw std::runtime_error("cannot write material audit");
        unsigned programs=0,named_variations=0;std::set<int> associated;std::set<std::pair<int,int>> all_pairs;
        auto associate=[&](const shader_catalog::Program& p){for(auto id:p.stages)if(id>=0)associated.insert(id);
            if(p.stages[0]>=0 && p.stages[1]>=0)all_pairs.emplace(p.stages[0],p.stages[1]);};
        for(auto& m:catalog.models){programs+=m.programs.size();for(auto& p:m.programs)associate(p);}
        for(auto& n:catalog.named_programs){named_variations+=n.variations.size();for(auto& p:n.variations)associate(p);}
        std::ofstream report(dir+"summary.json");report<<"{\"scope\":\"static_asset_relationships\",\"runtime_state_complete\":false,\"shader_records\":"<<catalog.records.size()
            <<",\"shader_models\":"<<catalog.models.size()<<",\"program_selectors\":"<<programs<<",\"named_program_groups\":"<<catalog.named_programs.size()
            <<",\"named_program_variations\":"<<named_variations<<",\"unassociated_shader_records\":"<<catalog.records.size()-associated.size()<<",\"explicit_pairs\":"<<all_pairs.size()
            <<",\"model_files\":"<<files.size()<<",\"file_parse_failures\":"<<failed
            <<",\"models\":"<<models<<",\"materials\":"<<materials<<",\"matched_materials\":"<<matched<<",\"unresolved_materials\":"<<no_match<<",\"shapes\":"<<shapes
            <<",\"candidate_vertex_records\":"<<vertex_ids.size()<<",\"candidate_fragment_records\":"<<fragment_ids.size()<<",\"candidate_pairs\":"<<pairs.size()<<",\"vertex_formats\":{";
        bool first=true;for(auto [format,n]:formats){if(!first)report<<',';first=false;report<<'"'<<format<<"\":"<<n;}report<<"}}\n";report.close();if(!report)throw std::runtime_error("cannot write summary");
        std::cout<<"shader models="<<catalog.models.size()<<", selectors="<<programs<<", model files="<<files.size()<<", materials="<<materials<<", matched="<<matched<<", unresolved="<<no_match<<", parse failures="<<failed<<'\n';
        return failed?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
