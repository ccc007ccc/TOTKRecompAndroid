#include "shader_warm.h"
#include "shader_runtime.h"
#include <chrono>
#include <thread>
extern "C" {
#include "../../paths.h"
}
extern "C" bool vk_shader_boot_warm(const char* game,void (*idle)(void)) {
    vk_shader_warm_observe_begin(game);
    if(!vk_shader_runtime_compile_enabled() || !vk_shader_warm_enabled())return true;
    char manifest[1200]{};
    if(!rt_resource("shader-warm.bin",nullptr,manifest,sizeof manifest))return false;
    vk_shader_warm_start(game,manifest);
    for(;;) {
        auto state=vk_shader_warm_status();
        if(state.title_ready || !state.running)return !state.failed;
        if(idle)idle();std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
