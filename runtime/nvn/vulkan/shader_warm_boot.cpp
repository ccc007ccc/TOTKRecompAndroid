#include "shader_warm.h"
#include "shader_runtime.h"
#include "shader_prepare.h"
#include <chrono>
#include <thread>
extern "C" {
#include "../../paths.h"
}
extern "C" bool vk_shader_boot_warm(const char* game,void (*idle)(void)) {
    vk_shader_warm_observe_begin(game);
    if(!vk_shader_runtime_compile_enabled()) {
        // Recheck at every launch after settings have loaded, even if the UI
        // checked earlier: the OS or user may have removed cached modules.
        vk_shader_check_stop();vk_shader_check_start(game);
        for(;;) {
            auto state=vk_shader_check_status();
            if(!state.running){vk_shader_check_stop();return state.ready && !state.failed;}
            if(idle)idle();std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if(!vk_shader_warm_enabled())return true;
    char manifest[1200]{};
    if(!rt_resource("shader-warm.bin",nullptr,manifest,sizeof manifest))return false;
    vk_shader_warm_start(game,manifest);
    for(;;) {
        auto state=vk_shader_warm_status();
        if(state.title_ready || !state.running)return !state.failed;
        if(idle)idle();std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
