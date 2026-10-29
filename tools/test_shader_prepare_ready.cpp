// Full receipts verify actual modules and their dictionaries, outside the UI.
#include "../runtime/nvn/vulkan/shader_prepare.cpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
static uint64_t profile=123;
extern "C" uint64_t vk_shader_prepare_profile(void){return profile;}
extern "C" void vk_shader_prepare_configure(void){}
extern "C" void vk_shader_translate_begin(void){}
extern "C" void vk_shader_translate_end(void){}
extern "C" unsigned vk_shader_prepare_gather(void){return 8;}
extern "C" bool vk_shader_prepare_vertex_snap(void){return false;}
extern "C" {
#include "../runtime/nvn/vulkan/shaders.h"
VkG g_vk;
void rt_fatal_trace(void){abort();}
}
int main(int argc,char** argv) {
    namespace fs=std::filesystem;
    assert(argc==2);fs::create_directories(argv[1]);fs::current_path(argv[1]);
    fs::create_directories("game");const char* game="game";
    std::ofstream("game/romfs.bin")<<"invalid romfs fixture";
    auto ready=receipt_path(game);auto id=identity(game);
    // The old identity-only marker cannot grant readiness, even with no cache.
    std::ofstream("game/.shaders-ready")<<id;
    fs::remove(ready);assert(!vk_shader_game_ready(game));
    auto save=[&](const std::set<uint64_t>& keys){assert(shader_receipt::save(ready,identity(game),keys));};
    save({1});assert(vk_shader_game_has_receipt(game) && !vk_shader_game_ready(game));
    save({});assert(!vk_shader_game_has_receipt(game) && !vk_shader_game_ready(game));
    const char* base_source="#version 450\nlayout(location=0) in vec4 a;void main(){gl_Position=uintBitsToFloat(floatBitsToUint(a));}";
    const char* int_source="#version 450\nlayout(location=0) in ivec4 a;void main(){gl_Position=uintBitsToFloat(uvec4(a));}";
    char why[1024]{};
    auto key=vk_shader_prepare_key(int_source,STAGE_VERTEX);
    auto prepare=[&]{
        assert(vk_shader_prepare_begin(why,sizeof why));
        auto base=vk_shader_prepare_base(base_source,STAGE_VERTEX,why,sizeof why);assert(base);
        ShaderPrepareVariant variant{};variant.key=key;variant.kinds[0]=1;
        assert(vk_shader_prepare_variant(base,&variant,why,sizeof why));
        vk_shader_prepare_base_free(base);vk_shader_prepare_end();save({key});
    };
    prepare();assert(vk_shader_game_ready(game));
    auto path_for=[](uint64_t k,const char* suffix){char name[96];snprintf(name,sizeof name,"/%016llx%s",(unsigned long long)k,suffix);
        return std::string("cache/shaders/")+vk_shader_cache_namespace()+name;};
    auto base_file=path_for(vk_shader_prepare_key(base_source,STAGE_VERTEX),".base.bin");
    auto variant_file=path_for(key,".bin");
    fs::remove(base_file);assert(!vk_shader_game_ready(game));prepare();
    {std::ofstream file(variant_file,std::ios::binary|std::ios::trunc);file<<"broken";}
    assert(!vk_shader_game_ready(game));prepare();
    fs::rename("cache/shaders","cache/removed-shaders");assert(!vk_shader_game_ready(game));
    fs::rename("cache/removed-shaders","cache/shaders");assert(vk_shader_game_ready(game));
    ++profile;assert(!vk_shader_game_ready(game));--profile;
    {std::ofstream file(ready,std::ios::app);file<<"trailing";}assert(!vk_shader_game_ready(game));save({key});
    for(auto body:{"999999999\n","2\n0000000000000001\n","1\nnot-a-key\n"}) {
        std::ofstream(ready)<<id<<'\n'<<body;assert(!vk_shader_game_ready(game));
    }
    {std::ofstream file(ready);file<<id;file.put(0);file<<"ignored\n1\n"<<std::hex<<std::setw(16)<<std::setfill('0')<<key<<'\n';}
    assert(!vk_shader_game_ready(game));
    save({key});unsigned checked=0;
    try{verify_game(game,[&]{++checked;throw shader_warm::Cancelled();});assert(false);}catch(const shader_warm::Cancelled&){}
    assert(checked==1 && vk_shader_game_ready(game));
    vk_shader_check_start(game);
    for(unsigned i=0;vk_shader_check_status().running;++i){assert(i<500);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    assert(vk_shader_check_status().ready && !vk_shader_check_status().failed);vk_shader_check_stop();
    // Read-only input: reuse needs no writes beside the RomFS. Failed preparation
    // removes only the app receipt, and an unavailable log must not close stderr.
    fs::permissions("game",fs::perms::owner_read|fs::perms::owner_exec);
    assert(vk_shader_prepare_game(game,nullptr,why,sizeof why));
    assert(!fs::exists("game/shader-prepare.log"));
    fs::remove(variant_file);fs::create_directory(shader_warm::state_directory(game)+"/full.log");
    assert(!vk_shader_prepare_game(game,nullptr,why,sizeof why) && why[0]);
    assert(!fs::exists(ready) && !vk_shader_game_ready(game));
    assert(fputs("PASS: stderr survives failed diagnostic log\n",stderr)>=0);
    fs::permissions("game",fs::perms::owner_all);
    puts("PASS: full receipt, missing/corrupt modules and dictionaries, profile change, cancellation, read-only input and failed retry");
}
