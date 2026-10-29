// Actual title requests -> production translator/compiler/cache -> strict load,
// with SPIR-V interface types checked against the recorded binding and VS mask.
#include "../runtime/nvn/vulkan/shader_warm_plan.hpp"
#include "../runtime/nvn/vulkan/shader_warm_state.hpp"
#include "../runtime/nvn/vulkan/shader_warm.h"
#include "../runtime/nvn/vulkan/shader_warm_job.hpp"
#include "../runtime/nvn/vulkan/shader_prepare.h"
#include <cassert>
#include <cstdarg>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <unistd.h>
static std::atomic<uint64_t> test_profile{0x103};
extern "C" {
#include "../runtime/nvn/vulkan/shaders.h"
VkG g_vk;
void rt_fatal_trace(void){abort();}
#ifdef __ANDROID__
void rt_fatal_message(const char* format,...){va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);abort();}
#endif
void vk_shader_translate_begin(void){}
void vk_shader_translate_end(void){}
unsigned vk_shader_prepare_gather(void){return 8;}
bool vk_shader_prepare_vertex_snap(void){return false;}
bool vk_shader_prepare_compute_linear(void){return true;}
uint64_t vk_shader_prepare_profile(void){return test_profile.load();}
}
std::mutex vk_shader_preparation_mutex;

static void interfaces(const uint32_t* spv,size_t words,int stage,const ShaderKey& key) {
    struct Type{uint32_t op=0,a=0,b=0;};
    std::vector<Type> types(spv[3]);std::vector<int> location(spv[3],-1);
    for(size_t at=5;at<words;at+=spv[at]>>16) {
        auto n=spv[at]>>16,op=spv[at]&65535;assert(n && n<=words-at);const auto* w=spv+at;
        if(op==21 || op==22 || op==23 || op==32){assert(w[1]<types.size());types[w[1]]={op,w[2],n>3?w[3]:0};}
        if(op==71 && n==4 && w[2]==30){assert(w[1]<location.size());location[w[1]]=w[3];}
    }
    for(size_t at=5;at<words;at+=spv[at]>>16) {
        auto op=spv[at]&65535;const auto* w=spv+at;
        if(op!=59 || location.at(w[2])<0)continue;
        auto loc=unsigned(location[w[2]]);
        if(stage==1 && w[3]==1)assert(loc<32 && (key.vs_out_mask&(1u<<loc)));
        if(!((stage==0 && w[3]==1) || (stage==1 && w[3]==3)))continue;
        assert(loc<(stage==0?32u:8u));auto type=types.at(w[1]);assert(type.op==32);type=types.at(type.b);
        if(type.op==23)type=types.at(type.a);
        assert(type.op==21 || type.op==22);unsigned kind=type.op==22?0:type.b?1:2;
        assert(kind==(stage==0?key.attr_kind[loc]:key.rt_kind[loc]));
    }
}
static void progress(double value,const char* text){std::cerr<<value<<' '<<text<<'\n';}
int main(int argc,char** argv) {
    assert(argc==4 || (argc==5 && !strcmp(argv[4],"--cancel-background")));auto image=std::filesystem::absolute(argv[1]),manifest=std::filesystem::absolute(argv[2]);
    auto output=std::filesystem::absolute(argv[3]);std::filesystem::create_directories(output/"game");assert(!chdir(output.c_str()));
    if(!std::filesystem::exists("game/romfs.bin"))std::filesystem::create_symlink(image,"game/romfs.bin");
    auto state=std::filesystem::path(shader_warm::state_directory("game"));
    auto title_receipt=state/"title.receipt",common_receipt=state/"common.receipt",title_log=state/"title.log";
    auto game_permissions=std::filesystem::status("game").permissions();
    std::set<std::filesystem::path> game_files;
    for(const auto& entry:std::filesystem::directory_iterator("game"))game_files.insert(entry.path());
    std::filesystem::permissions("game",std::filesystem::perms::owner_read|std::filesystem::perms::owner_exec|
        std::filesystem::perms::group_read|std::filesystem::perms::group_exec|std::filesystem::perms::others_read|std::filesystem::perms::others_exec);
    maxwell_gmem_base=0x340000000ull;maxwell_gmem_size=0xd0000000ull;
    maxwell_gmem_map_all=maxwell_gmem_miss_log=maxwell_fp32_double=true;
    maxwell_brx_window=maxwell_static_stacks=maxwell_no_switch=maxwell_no_local_struct=maxwell_explicit_fallthrough=true;
    maxwell_native_half=maxwell_fast_minmax=true;
    char why[2048]{};
    assert(vk_shader_warm_game("game",manifest.c_str(),true,progress,why,sizeof why) || (std::cerr<<why<<'\n',false));
    assert(!std::filesystem::exists("game/.shaders-ready") && std::filesystem::exists(title_receipt));
    if(argc==5) {
        std::filesystem::remove(common_receipt);
        std::atomic<unsigned> submitted_batches{0};shader_warm::Job job;
        job.start("real-cancellation",[&](bool title,const auto& checkpoint,const auto& report){
            char error[2048]{};
            auto output=[&](double value,const char* text){if(!title && value==0.2)++submitted_batches;report(value,text);};
            if(!shader_warm::prepare("game",manifest.c_str(),title,output,error,sizeof error,{checkpoint,!title})) {
                checkpoint();throw std::runtime_error(error);
            }
        });
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
        while(submitted_batches<16 && job.status().running) {
            assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(submitted_batches>=16 && job.status().title_ready);job.stop();
        assert(job.status().cancelled && !std::filesystem::exists(common_receipt));
        assert(std::filesystem::exists(title_receipt));
        std::cout<<"PASS: real background preparation cancelled and joined; title receipt retained, no unfinished background receipt\n";
        bool changed=false;
        auto change_profile=[&](double value,const char*){if(value==0.2 && !changed){changed=true;++test_profile;}};
        assert(!shader_warm::prepare("game",manifest.c_str(),false,change_profile,why,sizeof why,{}));
        assert(changed && !strcmp(why,"graphics configuration changed during warm-up") && !std::filesystem::exists(common_receipt));
        {
            std::ifstream input(state/"common.log");std::string text((std::istreambuf_iterator<char>(input)),{});
            auto at=text.find("request_cache reused=");assert(at!=std::string::npos);
            assert(std::stoull(text.substr(at+21))>0); // work completed before cancellation survived
        }
        test_profile=0x103;
        assert(vk_shader_warm_game("game",manifest.c_str(),true,progress,why,sizeof why));
        std::cout<<"PASS: profile change aborts safely and a stable profile can resume preparation\n";
    }
    setenv("LARECOMP_NATIVE_LIB_DIR","/compiler-disabled-for-strict-cache-audit",1);
    // Even without a whole-title receipt, per-request records can finish the
    // exact same list without translating it or opening the compiler again.
    std::filesystem::remove(title_receipt);
    assert(vk_shader_warm_game("game",manifest.c_str(),true,progress,why,sizeof why) || (std::cerr<<why<<'\n',false));
    {
        std::ifstream input(title_log);std::string text((std::istreambuf_iterator<char>(input)),{});
        assert(text.find(" translated=0 ")!=std::string::npos);
        std::ifstream receipt(title_receipt);std::string identity,key;size_t count;
        std::getline(receipt,identity);receipt>>count>>key;assert(count>0);
        auto module=std::filesystem::path("cache/shaders")/vk_shader_cache_namespace()/(key+".bin");
        auto held=module;held+=".held";std::filesystem::rename(module,held);
        assert(!vk_shader_warm_game("game",manifest.c_str(),true,nullptr,why,sizeof why));
        std::filesystem::rename(held,module);
        assert(vk_shader_warm_game("game",manifest.c_str(),true,nullptr,why,sizeof why));
    }
    std::cout<<"PASS: per-request reuse survives a missing receipt; missing SPIR-V still requires real preparation\n";
    shader_archive::Romfs rom(image.c_str());auto catalog=shader_catalog::read(rom,[](const std::string&){});
    auto selection=shader_warm::select(catalog,shader_warm::read(manifest.c_str()),true);
    size_t checked=0;
    auto stats=shader_warm::plan(catalog,selection,{8,false,true},[](const shader_archive::Shader& shader,const ShaderKey& key){
        return shader_warm::Info(maxwell_translate_bounded(shader.binary.data(),shader.constants,shader.binary.size()-shader.constants,shader.stage,&key),maxwell_free);
    },[&](unsigned,const ShaderKey& key,const ShaderInfo& info){
        size_t words=0;auto spv=vk_load_prepared_shader(info.msl,info.stage,"warm-check",&words);
        assert(spv && words>5);interfaces(spv,words,info.stage,key);free(spv);++checked;
    });
    assert(checked>0 && checked==stats.sources);
    // A verified warm receipt reuses every cached module without a compiler,
    // and no longer rescans or retranslates the RomFS on subsequent starts.
    assert(vk_shader_warm_game("game",manifest.c_str(),true,progress,why,sizeof why));
    std::cout<<"PASS: "<<checked<<" real title modules compile and strictly reload; numeric interfaces and linked varying masks match\n";
    // A log path that cannot be opened must not block an otherwise valid warm
    // receipt/cache. Exercise the same failure observed in the Android test app.
    std::filesystem::remove(title_log);
    std::filesystem::create_directory(title_log);
    assert(vk_shader_warm_game("game",manifest.c_str(),true,progress,why,sizeof why));
    assert(fprintf(stderr,"PASS: unwritable diagnostic log does not block title preparation or close stderr\n")>0);
    std::filesystem::remove(title_log);
    std::set<std::filesystem::path> after;
    for(const auto& entry:std::filesystem::directory_iterator("game"))after.insert(entry.path());
    assert(after==game_files);
    std::filesystem::permissions("game",game_permissions);
    std::cout<<"PASS: read-only game directory, receipt persisted in app cache, unchanged game files\n";
}
