#include "../runtime/nvn/vulkan/shader_warm_job.hpp"
#include "../runtime/nvn/vulkan/shader_warm.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
static std::atomic<unsigned> title_calls{0},common_calls{0};
static bool always_fail;
static bool expect_background,compile_enabled=true;
extern "C" bool vk_shader_runtime_compile_enabled(void){return compile_enabled;}
bool shader_warm::prepare(const char*,const char*,bool title,const Progress&,char* why,size_t capacity,const Options& options) {
    options.checkpoint();
    if(expect_background)assert(options.background);
    if(title){++title_calls;return true;}
    auto attempt=++common_calls;
    if(always_fail || attempt==1){snprintf(why,capacity,"graphics configuration changed during warm-up");return false;}
    return true;
}
static void finish() {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(vk_shader_warm_status().running){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
int main() {
    vk_shader_warm_start("game","manifest");finish();
    assert(title_calls==1 && common_calls==2 && vk_shader_warm_status().title_ready && !vk_shader_warm_status().failed);
    vk_shader_warm_start("game","manifest");assert(title_calls==1);vk_shader_warm_stop();
    always_fail=true;common_calls=0;
    vk_shader_warm_start("next-game","manifest");finish();
    assert(common_calls==3 && vk_shader_warm_status().title_ready && vk_shader_warm_status().failed);vk_shader_warm_stop();
    always_fail=false;expect_background=true;common_calls=0;
    vk_shader_warm_resume();finish();assert(common_calls==2 && !vk_shader_warm_status().failed);vk_shader_warm_stop();
    compile_enabled=false;auto strict_titles=title_calls.load();vk_shader_warm_resume();assert(title_calls==strict_titles);compile_enabled=true;
    setenv("LARECOMP_SHADER_WARM","0",1);auto before=title_calls.load();
    vk_shader_warm_start("disabled","manifest");assert(title_calls==before);
    std::cout<<"PASS: API retries heap-profile changes boundedly, retains title readiness, and honors disable\n";
}
