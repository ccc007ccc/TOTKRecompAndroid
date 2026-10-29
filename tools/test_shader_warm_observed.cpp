#include "../runtime/nvn/vulkan/shader_warm_observed.hpp"
#include "../runtime/nvn/vulkan/shader_warm.h"
#include <cassert>
#include <filesystem>
#include <iostream>
extern "C" {
#include "../runtime/nvn/nvn.h"
}
static void put(shader_archive::Bytes& b,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)b.at(at+i)=value>>(8*i);}
int main(int argc,char** argv) {
    assert(argc==2);auto root=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(root/"game");
    std::filesystem::current_path(root);auto game=(root/"game").string();
    {std::ofstream file("game/romfs.bin");file<<"fixture";}
    if(!std::filesystem::exists("alias"))std::filesystem::create_directory_symlink("game","alias");
    assert(shader_warm::state_directory(game.c_str())==shader_warm::state_directory("alias"));
    std::filesystem::create_directories("other");{std::ofstream file("other/romfs.bin");file<<"fixture";}
    assert(shader_warm::state_directory("other")!=shader_warm::state_directory(game.c_str()));
    std::string path=shader_warm::state_directory(game.c_str())+"/observed.tsv";
    auto original_permissions=std::filesystem::status("game").permissions();
    std::filesystem::remove(path);
    shader_archive::Bytes vc(0x880),fc(0x880),cc(0x880),vb(0x100,1),fb(0x120,2),cb(0x200,3);
    auto stage=[&](unsigned kind,shader_archive::Bytes& control,shader_archive::Bytes& code){
        put(control,0,0x98761234);put(control,0x20,0x880);put(control,0x704,code.size());put(control,0x714,kind);
        NvnShaderStage result{};result.stage=kind;result.control=control.data();result.code_addr=(uintptr_t)code.data();
        result.const_offset=code.size();return result;
    };
    auto vs=stage(0,vc,vb),fs=stage(1,fc,fb),cs=stage(5,cc,cb);
    ShaderKey vk{},fk{},ck{};vk.attr_kind[3]=2;vk.flip_x=true;fk.rt_kind[1]=1;ck.image_kind[2]=2;
    vk_shader_warm_observe_begin(game.c_str());
    vk_shader_warm_observe(&vs,&fs,&vk,&fk);vk_shader_warm_observe(&vs,&fs,&vk,&fk);
    vk_shader_warm_observe(&cs,nullptr,&ck,nullptr);vk_shader_warm_observe_flush();
    auto rows=shader_warm::read_observed(path);assert(rows.size()==2 && rows[0].stage==5 && rows[0].images[2]==2);
    assert(rows[1].stage==shader_warm::Pair && rows[1].mirror && rows[1].attributes[3]==2 && rows[1].outputs[1]==1);
    shader_archive::Shader original{5,(uint32_t)cb.size(),cb,cc};assert(rows[0].first==shader_warm::raw_key(original));
    vk_shader_warm_observe_end();
    std::string legacy=game+"/.shader-warm-observed.tsv";
    std::filesystem::copy_file(path,legacy,std::filesystem::copy_options::overwrite_existing);
    auto contents=[](const std::string& file){std::ifstream in(file);return std::string(std::istreambuf_iterator<char>(in),{});};
    auto legacy_before=contents(legacy);std::filesystem::remove(path);
    std::filesystem::permissions("game",std::filesystem::perms::owner_read|std::filesystem::perms::owner_exec|
        std::filesystem::perms::group_read|std::filesystem::perms::group_exec|std::filesystem::perms::others_read|std::filesystem::perms::others_exec);
    vk_shader_warm_observe_begin(game.c_str());vk_shader_warm_observe_end();
    assert(shader_warm::read_observed(path).size()==2 && contents(legacy)==legacy_before);
    {std::ofstream file(path,std::ios::app);file<<"damaged-row\n"<<shader_warm::observed_line(rows[1]).substr(0,40);}
    auto intact=shader_warm::read_observed(path);assert(intact.size()==2);
    vk_shader_warm_observe_begin(game.c_str());vk_shader_warm_observe(&vs,&fs,&vk,&fk);vk_shader_warm_observe_end();
    rows=shader_warm::read_observed(path);assert(rows.size()==2 && rows[0].stage==shader_warm::Pair); // latest use wins
    auto bad=shader_warm::observed_line(rows[0]);bad[0]='z';shader_warm::Request ignored;assert(!shader_warm::observed_parse(bad,ignored));
    // Compaction is bounded by the newest distinct states, without deleting the
    // compiled cache that may still serve older states.
    {std::ofstream file(path);for(unsigned i=1;i<shader_warm::RecentLimit+10;++i){auto r=rows[0];r.first[0]=i;r.first[1]=i>>8;file<<shader_warm::observed_line(r)<<'\n';}}
    rows=shader_warm::read_observed(path);assert(rows.size()==shader_warm::RecentLimit);
    assert(shader_warm::compact_observed(path,rows));assert(shader_warm::read_observed(path).size()==rows.size());
    assert(contents(legacy)==legacy_before);std::filesystem::permissions("game",original_permissions);
    std::cout<<"PASS: observed identities/types, dedup, recency, corrupt rows, bounded history, app-cache isolation, path aliases, read-only legacy migration\n";
}
