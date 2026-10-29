#include "shader_warm_job.hpp"
#include "shader_warm.h"
#include "shader_runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
shader_warm::Job& job() {
    // Process-lifetime owner. Device exit uses _Exit; normal reboot/settings
    // changes explicitly stop and join before resetting any runtime state.
    static auto* owner=new shader_warm::Job;return *owner;
}
std::mutex context_mutex;
std::string last_game,last_manifest;
}
extern "C" bool vk_shader_warm_enabled(void) {
    const char* value=getenv("LARECOMP_SHADER_WARM");return !value || atoi(value)!=0;
}
static void start(const char* game,const char* manifest,bool resumed) {
    if(!game || !manifest || !vk_shader_warm_enabled())return;
    std::string root=game,list=manifest;
    job().start(root+'\n'+list,[root,list,resumed](bool title,const shader_warm::Checkpoint& checkpoint,const shader_warm::Progress& progress){
        for(unsigned attempt=0;attempt<3;++attempt) {
            char why[2048]{};
            if(shader_warm::prepare(root.c_str(),list.c_str(),title,progress,why,sizeof why,{checkpoint,!title || resumed}))return;
            checkpoint();
            if(!title && attempt<2 && !strcmp(why,"graphics configuration changed during warm-up")) {
                fprintf(stderr,"[shader-warm] restarting for the current heap memory path\n");continue;
            }
            fprintf(stderr,"[shader-warm] %s\n",why);throw std::runtime_error(why);
        }
    },resumed);
}
extern "C" void vk_shader_warm_start(const char* game,const char* manifest) {
    if(!game || !manifest)return;
    {std::lock_guard<std::mutex> lock(context_mutex);last_game=game;last_manifest=manifest;}
    start(game,manifest,false);
}
extern "C" void vk_shader_warm_resume(void) {
    if(!vk_shader_runtime_compile_enabled() || !vk_shader_warm_enabled())return;
    std::string game,manifest;
    {std::lock_guard<std::mutex> lock(context_mutex);game=last_game;manifest=last_manifest;}
    if(!game.empty() && !manifest.empty())start(game.c_str(),manifest.c_str(),true);
}
extern "C" void vk_shader_warm_stop(void){job().stop();}
extern "C" void vk_shader_warm_cancel_async(void){job().cancel_async();}
extern "C" ShaderWarmStatus vk_shader_warm_status(void) {
    auto state=job().status();return {state.running,state.title_ready,state.failed,state.progress};
}
