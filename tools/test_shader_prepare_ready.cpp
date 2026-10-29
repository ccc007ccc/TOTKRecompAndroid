// Completion identity migration and failure invalidation; uses only a tiny fixture.
#include "../runtime/nvn/vulkan/shader_prepare.cpp"
#include <cassert>
#include <filesystem>
#include <fstream>
static uint64_t profile=123;
extern "C" uint64_t vk_shader_prepare_profile(void){return profile;}
extern "C" void vk_shader_prepare_configure(void){}
extern "C" unsigned vk_shader_prepare_gather(void){return 8;}
extern "C" bool vk_shader_prepare_vertex_snap(void){return false;}
extern "C" {
#include "../runtime/nvn/vulkan/shaders.h"
VkG g_vk;
void rt_fatal_trace(void){abort();}
}
int main(int argc,char** argv) {
    assert(argc==2);std::filesystem::create_directories(argv[1]);const char* game=argv[1];
    auto rom=std::filesystem::path(game)/"romfs.bin",ready=std::filesystem::path(game)/".shaders-ready";
    std::ofstream(rom)<<"invalid romfs fixture";std::filesystem::remove(ready);assert(!vk_shader_game_ready(game));
    auto id=identity(game);assert(id.rfind("v3:",0)==0);
    auto write=[&](const std::string& text){std::ofstream f(ready,std::ios::binary);f<<text;f.close();assert(f);};
    write(id.substr(3));assert(!vk_shader_game_ready(game)); // legacy unversioned marker
    write("v2:"+id.substr(3));assert(!vk_shader_game_ready(game));
    write(id);assert(vk_shader_game_ready(game));++profile;assert(!vk_shader_game_ready(game));--profile;
    assert(vk_shader_game_ready(game));write(id+"x");assert(!vk_shader_game_ready(game));
    write(id);std::ofstream(rom,std::ios::app)<<"changed";assert(!vk_shader_game_ready(game));
    write(identity(game));assert(vk_shader_game_ready(game));char why[1024]{};
    assert(!vk_shader_prepare_game(game,nullptr,why,sizeof why) && why[0]);
    assert(!std::filesystem::exists(ready) && !vk_shader_game_ready(game));
    puts("PASS: v3 completion identity rejects legacy/profile/game mismatches; failed preparation removes the marker");
}
