#include "shader_warm_plan.hpp"
#include "shader_warm.h"
#include "shader_warm_job.hpp"
#include "shader_warm_observed.hpp"
#include "shader_warm_requests.hpp"
#include "shader_prepare_queue.hpp"
#include "shader_cache_receipt.hpp"
#include <chrono>
#include <cerrno>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#ifndef SHADER_BUILD_ID
#define SHADER_BUILD_ID "development"
#endif
#ifndef SHADER_WARM_ID
#define SHADER_WARM_ID "development"
#endif
extern std::mutex vk_shader_preparation_mutex;

namespace {
std::string warm_identity(const char* game,const char* manifest,bool title,const std::vector<shader_warm::Request>& recent={},uint64_t* captured_profile=nullptr) {
    struct stat file{};
    if(stat((std::string(game)+"/romfs.bin").c_str(),&file))return {};
    std::ifstream input(manifest,std::ios::binary);if(!input)return {};
    CC_SHA256_CTX hash;CC_SHA256_Init(&hash);char buffer[65536];size_t bytes=0;
    while(input.read(buffer,sizeof buffer) || input.gcount()) {
        bytes+=input.gcount();if(bytes>32u*1024*1024)return {};
        CC_SHA256_Update(&hash,buffer,static_cast<CC_LONG>(input.gcount()));
    }
    if(!input.eof())return {};
    for(const auto& request:recent) {
        auto bytes=shader_warm::encode(request);CC_SHA256_Update(&hash,bytes.data(),bytes.size());
    }
    shader_warm::RawKey digest;CC_SHA256_Final(digest.data(),&hash);
    vk_shader_translate_begin();auto profile=vk_shader_prepare_profile();vk_shader_translate_end();
    if(captured_profile)*captured_profile=profile;
    std::ostringstream out;
    out<<"warm-v2:"<<SHADER_WARM_ID<<':'<<SHADER_BUILD_ID<<':'<<title<<':'<<file.st_size<<':'<<file.st_mtim.tv_sec<<':'<<file.st_mtim.tv_nsec<<':'<<file.st_ino<<':'<<std::hex<<profile<<':';
    for(auto value:digest)out<<std::setw(2)<<std::setfill('0')<<unsigned(value);
    return out.str();
}
void require_profile(uint64_t profile) {
    vk_shader_translate_begin();auto current=vk_shader_prepare_profile();vk_shader_translate_end();
    if(current!=profile)throw std::runtime_error("graphics configuration changed during warm-up");
}
bool warm_cached(const std::string& path,const std::string& identity,size_t& count,const shader_warm::Checkpoint& checkpoint={}) {
    return shader_receipt::check(path,identity,count,262144,checkpoint);
}
bool save_receipt(const std::string& path,const std::string& identity,const std::set<uint64_t>& keys) {
    return shader_receipt::save(path,identity,keys);
}
}

bool shader_warm::prepare(const char* game,const char* manifest,bool title_only,const Progress& progress,
                           char* why,size_t capacity,const Options& options) {
    std::unique_lock<std::mutex> guard(vk_shader_preparation_mutex,std::try_to_lock);
    if(!guard.owns_lock()){snprintf(why,capacity,"graphics preparation is already running");return false;}
    FILE* log=nullptr;
    size_t cached_requests=0,cached_masks=0,translations=0;
    auto close_log=[&]{if(log==stderr)fflush(log);else if(log)fclose(log);log=nullptr;};
    auto checkpoint=[&]{if(options.checkpoint)options.checkpoint();};
    try {
        checkpoint();
        shader_warm::require(game && manifest,"missing game or warm manifest path");
        auto state=shader_warm::state_directory(game);
        auto log_path=state+(title_only?"/title.log":"/common.log");
        log=fopen(log_path.c_str(),"wb");
        if(!log) {
            // Readable game files may live in a directory we cannot write. A
            // diagnostic log must not prevent preparation into the app's cache.
            fprintf(stderr,"[shader-warm] cannot write %s: %s; logging to stderr\n",log_path.c_str(),strerror(errno));
            log=stderr;
        }
        auto start=std::chrono::steady_clock::now();
        auto recent=title_only?std::vector<shader_warm::Request>{}:shader_warm::read_recent_game(game);
        uint64_t identity_profile=0;
        auto identity=warm_identity(game,manifest,title_only,recent,&identity_profile);
        shader_warm::require(!identity.empty(),"cannot identify graphics warm-up inputs");
        auto receipt=state+(title_only?"/title.receipt":"/common.receipt");
        size_t hits=0;
        if(warm_cached(receipt,identity,hits,checkpoint)) {
            require_profile(identity_profile);
            double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            fprintf(log,"result cache_hits=%zu seconds=%.3f compiler_opened=0 driver_pipelines=0\n",hits,seconds);close_log();
            fprintf(stderr,"[shader-warm] ready title=%d cache_hits=%zu seconds=%.3f compiler_opened=0\n",title_only,hits,seconds);
            if(capacity)why[0]=0;if(progress)progress(1,"Graphics warm-up complete.");return true;
        }
        auto requests=shader_warm::read(manifest);
        if(progress)progress(0,"Reading graphics warm-up list...");
        shader_archive::Romfs rom((std::string(game)+"/romfs.bin").c_str());
        auto catalog=shader_catalog::read(rom,[&](const std::string&){checkpoint();});
        auto compute=title_only?std::vector<Request>{}:default_compute_requests(catalog);
        requests.insert(requests.begin(),compute.begin(),compute.end());
        requests.insert(requests.begin(),recent.begin(),recent.end());
        auto selection=shader_warm::select(catalog,requests,title_only);
        fprintf(log,"selection requests=%zu missing_from_game=%zu stage_mismatch=%zu title_only=%d default_compute=%zu\n",
                selection.requests.size(),selection.missing,selection.mismatched,title_only,compute.size());fflush(log);
        shader_warm::require(selection.mismatched==0,"shared shader stage mismatch");
        vk_shader_translate_begin();
        shader_warm::Config config{vk_shader_prepare_gather(),vk_shader_prepare_vertex_snap(),vk_shader_prepare_compute_linear()};
        auto profile=vk_shader_prepare_profile();
        vk_shader_translate_end();
        if(profile!=identity_profile)throw std::runtime_error("graphics configuration changed during warm-up");
        ShaderPrepareQueue queue(0,[&]{checkpoint();if(options.background)shader_warm::background_priority();});
        // As in upstream TranslationCache, a completed item survives cancellation
        // and additions to the shared/recent list. Identity still binds the actual
        // translator, planner and profile; every reused SPIR-V key is checked.
        std::ostringstream request_identity;
        request_identity<<SHADER_BUILD_ID<<':'<<SHADER_WARM_ID<<':'<<std::hex<<profile;
        RequestCache request_cache(state,request_identity.str());
        size_t queued=0;
        std::set<uint64_t> keys,verified;
        auto report_queued=[&] {
            if(!(++queued%64)) {
                fprintf(log,"queued=%zu completed=%llu cached_requests=%zu\n",queued,(unsigned long long)queue.done(),cached_requests);fflush(log);
                if(progress)progress(0.2,"Preparing graphics used by the warm-up list...");
            }
        };
        Reuse reuse;
        reuse.source=[&](unsigned record,const ShaderKey& key) {
            checkpoint();require_profile(profile);
            const auto& shader=catalog.records.at(record).shader;RequestCache::Result cached;
            if(!request_cache.get(request_cache.key(shader,key),cached) || cached.stage!=shader.stage)return false;
            if(!verified.count(cached.source) && !vk_shader_cached(cached.source))return false;
            verified.insert(cached.source);keys.insert(cached.source);++cached_requests;report_queued();return true;
        };
        reuse.mask=[&](unsigned record,const ShaderKey& key,uint32_t& mask) {
            checkpoint();require_profile(profile);RequestCache::Result cached;
            if(!request_cache.get(request_cache.key(catalog.records.at(record).shader,key),cached) || cached.stage!=STAGE_VERTEX)return false;
            mask=cached.outputs;++cached_masks;return true;
        };
        auto stats=shader_warm::plan(catalog,selection,config,[&](const shader_archive::Shader& shader,const ShaderKey& key){
            checkpoint();
            vk_shader_translate_begin();
            if(vk_shader_prepare_profile()!=profile) {
                vk_shader_translate_end();throw std::runtime_error("graphics configuration changed during warm-up");
            }
            shader_warm::Info info(maxwell_translate_bounded(shader.binary.data(),shader.constants,
                                  shader.binary.size()-shader.constants,shader.stage,&key),maxwell_free);
            auto after=vk_shader_prepare_profile();vk_shader_translate_end();
            if(after!=profile)throw std::runtime_error("graphics configuration changed during warm-up");
            ++translations;
            if(info && info->msl)request_cache.put(request_cache.key(shader,key),{vk_shader_prepare_key(info->msl,info->stage),info->out_mask,static_cast<uint8_t>(info->stage)});
            return info;
        },[&](unsigned record,const ShaderKey&,const ShaderInfo& info){
            keys.insert(vk_shader_prepare_key(info.msl,info.stage));
            queue.submit(info.msl,info.stage,"warm-"+std::to_string(record));
            report_queued();
        },reuse);
        if(progress)progress(0.8,"Preparing graphics used by the warm-up list...");
        queue.finish();
        checkpoint();
        require_profile(identity_profile);
        if(!save_receipt(receipt,identity,keys))fprintf(log,"receipt_not_saved\n");
        double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        fprintf(log,"result requests=%zu source_requests=%zu unique_sources=%zu vertex=%zu fragment=%zu compute=%zu unpaired_fragments=%zu seconds=%.3f driver_pipelines=0\n",
                stats.requests,stats.sources,keys.size(),stats.stages[0],stats.stages[1],stats.stages[5],stats.unpaired_fragments,seconds);
        fprintf(log,"request_cache reused=%zu masks=%zu translated=%zu entries=%zu\n",cached_requests,cached_masks,translations,request_cache.size());
        close_log();if(capacity)why[0]=0;
        fprintf(stderr,"[shader-warm] prepared title=%d requests=%zu source_requests=%zu modules=%zu seconds=%.3f\n",
                title_only,stats.requests,stats.sources,keys.size(),seconds);
        fprintf(stderr,"[shader-warm] request-cache title=%d reused=%zu masks=%zu translated=%zu\n",title_only,cached_requests,cached_masks,translations);
        // No .shaders-ready marker: a warm list is deliberately not exhaustive.
        if(progress)progress(1,"Graphics warm-up complete.");return true;
    } catch(const std::exception& error) {
        snprintf(why,capacity,"%s",error.what());
        if(log){fprintf(log,"request_cache reused=%zu masks=%zu translated=%zu\nfailed %s\n",cached_requests,cached_masks,translations,error.what());close_log();}
        fprintf(stderr,"[shader-warm] interrupted request-cache reused=%zu masks=%zu translated=%zu\n",cached_requests,cached_masks,translations);
        return false;
    }
}

extern "C" bool vk_shader_warm_game(const char* game,const char* manifest,bool title_only,
                                    void (*progress)(double,const char*),char* why,size_t capacity) {
    return shader_warm::prepare(game,manifest,title_only,progress?shader_warm::Progress(progress):shader_warm::Progress{},why,capacity,{});
}
