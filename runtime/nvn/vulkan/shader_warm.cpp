#include "shader_warm_plan.hpp"
#include "shader_warm.h"
#include "shader_warm_job.hpp"
#include "shader_warm_observed.hpp"
#include "shader_warm_requests.hpp"
#include "shader_prepare_queue.hpp"
#include "shader_artifact.h"
#include "shader_artifact_storage.hpp"
#include "../../diagnostics_build.h"
#include "pipeline_seed.h"
#include "shader_cache_receipt.hpp"
#include <algorithm>
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
#ifndef SHADER_TRANSLATION_ID
#define SHADER_TRANSLATION_ID SHADER_BUILD_ID
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
    // A receipt certifies completed coverage, independent of which already
    // covered state was used most recently. Keep execution order unchanged;
    // canonicalize only the identity of the bounded recent set.
    std::vector<shader_archive::Bytes> covered;
    for(const auto& request:recent)covered.push_back(shader_warm::encode(request));
    std::sort(covered.begin(),covered.end());
    covered.erase(std::unique(covered.begin(),covered.end()),covered.end());
    for(const auto& bytes:covered)CC_SHA256_Update(&hash,bytes.data(),bytes.size());
    shader_warm::RawKey digest;CC_SHA256_Final(digest.data(),&hash);
    vk_shader_translate_begin();auto profile=vk_shader_prepare_profile();vk_shader_translate_end();
    if(captured_profile)*captured_profile=profile;
    std::ostringstream out;
    out<<"warm-v3:"<<SHADER_WARM_ID<<':'<<SHADER_BUILD_ID<<':'<<title<<':'<<file.st_size<<':'<<file.st_mtim.tv_sec<<':'<<file.st_mtim.tv_nsec<<':'<<file.st_ino<<':'<<std::hex<<profile<<':'<<vk_shader_cache_namespace()<<':';
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
    const bool trace_reuse=diag_getenv("LARECOMP_LOG_SHADER_WARM_REUSE")!=nullptr;
    size_t request_misses=0,artifact_misses=0,artifact_miss_cached=0,source_misses=0,metadata_deferred=0;
#if TOTK_DIAGNOSTICS
    const auto storage_before=shader_artifact_storage::stats();
#endif
    auto report_reuse=[&] {
#if TOTK_DIAGNOSTICS
        if(!trace_reuse)return;
        const auto storage=shader_artifact_storage::stats();
        fprintf(stderr,"[shader-warm-reuse] title=%d background=%d request_miss=%zu artifact_miss=%zu artifact_miss_spirv_cached=%zu source_miss=%zu stored=%llu failed=%llu metadata_deferred=%zu publication_deferred=%llu\n",
                title_only,options.background,request_misses,artifact_misses,artifact_miss_cached,source_misses,
                (unsigned long long)(storage.stored-storage_before.stored),(unsigned long long)(storage.failed-storage_before.failed),
                metadata_deferred,(unsigned long long)(storage.deferred-storage_before.deferred));
#endif
    };
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
        bool cached=warm_cached(receipt,identity,hits,checkpoint);
        if(cached) {
            std::vector<VkPipelineRecipe> seeds(4096);auto seed_id=vk_pipeline_seed_identity(143);
            cached=vk_pipeline_seed_load((state+"/pipeline-seed.bin").c_str(),&seed_id,seeds.data(),seeds.size())!=0;
        }
        if(cached) {
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
        auto compute=default_compute_requests(catalog);
        if(title_only)for(auto& request:compute)request.title=true;
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
        ShaderPrepareQueue queue(0,[&]{checkpoint();if(options.background)shader_warm::background_priority();},options.background);
        // As in upstream TranslationCache, a completed item survives cancellation
        // and additions to the shared/recent list. Identity still binds the actual
        // translator, planner and profile; every reused SPIR-V key is checked.
        std::ostringstream request_identity;
        request_identity<<SHADER_TRANSLATION_ID<<':'<<SHADER_WARM_ID<<':'<<std::hex<<profile;
        RequestCache request_cache(state,request_identity.str());
        size_t queued=0;
        std::set<uint64_t> keys,verified;
        std::vector<VkPipelineRecipe> seeds;
        std::set<std::array<uint64_t,4>> seed_keys;
        auto seed_compute=[&](unsigned record,uint64_t source) {
            const auto& shader=catalog.records.at(record).shader;
            if(shader.stage!=STAGE_COMPUTE || shader.control.size()<0x754)return;
            shader_archive::View control{shader.control.data(),shader.control.size()};uint32_t local[3];
            for(unsigned i=0;i<3;++i){local[i]=control.u32(0x748+4*i);if(!local[i])local[i]=1;}
            if(!vk_pipeline_local_size_valid(local) || seeds.size()>=4096 || !seed_keys.insert({source,local[0],local[1],local[2]}).second)return;
            VkPipelineRecipe recipe;vk_pipeline_recipe_compute(&recipe,source,local);seeds.push_back(recipe);
        };
        auto report_queued=[&] {
            if(!(++queued%64)) {
                fprintf(log,"queued=%zu completed=%llu cached_requests=%zu\n",queued,(unsigned long long)queue.done(),cached_requests);fflush(log);
                if(progress)progress(0.2,"Preparing graphics used by the warm-up list...");
            }
        };
        bool require_metadata=true;
        Reuse reuse;
        reuse.source=[&](unsigned record,const ShaderKey& key) {
            checkpoint();require_profile(profile);
            const auto& shader=catalog.records.at(record).shader;RequestCache::Result cached;
            bool deferred=false;
            if(!request_cache.get(request_cache.key(shader,key),cached) || cached.stage!=shader.stage){++request_misses;return false;}
            if(!getenv("LARECOMP_SHADER_ARTIFACTS") || atoi(getenv("LARECOMP_SHADER_ARTIFACTS"))) {
                auto raw=raw_key(shader);VkShaderArtifact artifact{};
                vk_shader_translate_begin();auto artifact_profile=vk_shader_artifact_profile(vk_shader_prepare_profile());vk_shader_translate_end();
                if(!vk_shader_artifact_load(raw.data(),&key,artifact_profile,shader.stage,&artifact)) {
                    ++artifact_misses;
                    if(trace_reuse && vk_shader_cached(cached.source))++artifact_miss_cached;
                    // A complete request mapping can still prepare its exact
                    // SPIR-V when optional metadata will not fit. Do not keep
                    // translating the shared list only to evict it again.
                    // Title/recent entries repair metadata normally; gameplay
                    // still requires a real validated ShaderInfo on every load.
                    if(require_metadata || !shader_artifact_storage::at_capacity() || vk_shader_artifact_source_required())return false;
                    deferred=true;
                }
                vk_shader_artifact_free(&artifact);
            }
            if(!verified.count(cached.source) && !vk_shader_cached(cached.source)){++source_misses;return false;}
            if(deferred)++metadata_deferred;
            seed_compute(record,cached.source);
            verified.insert(cached.source);keys.insert(cached.source);++cached_requests;report_queued();return true;
        };
        reuse.mask=[&](unsigned record,const ShaderKey& key,uint32_t& mask) {
            checkpoint();require_profile(profile);RequestCache::Result cached;
            if(!request_cache.get(request_cache.key(catalog.records.at(record).shader,key),cached) || cached.stage!=STAGE_VERTEX)return false;
            mask=cached.outputs;++cached_masks;return true;
        };
        auto translate=[&](const shader_archive::Shader& shader,const ShaderKey& key){
            checkpoint();
            vk_shader_translate_begin();
            if(vk_shader_prepare_profile()!=profile) {
                vk_shader_translate_end();throw std::runtime_error("graphics configuration changed during warm-up");
            }
            shader_warm::Info info(maxwell_translate_bounded(shader.binary.data(),shader.constants,
                                  shader.binary.size()-shader.constants,shader.stage,&key),maxwell_free);
            if(info && info->msl) {
                auto raw=raw_key(shader);
                vk_shader_artifact_store(raw.data(),&key,vk_shader_artifact_profile(profile),info.get());
            }
            auto after=vk_shader_prepare_profile();vk_shader_translate_end();
            if(after!=profile)throw std::runtime_error("graphics configuration changed during warm-up");
            ++translations;
            if(info && info->msl)request_cache.put(request_cache.key(shader,key),{vk_shader_prepare_key(info->msl,info->stage),info->out_mask,static_cast<uint8_t>(info->stage)});
            return info;
        };
        auto emit=[&](unsigned record,const ShaderKey&,const ShaderInfo& info){
            keys.insert(vk_shader_prepare_key(info.msl,info.stage));
            seed_compute(record,vk_shader_prepare_key(info.msl,info.stage));
            queue.submit(info.msl,info.stage,"warm-"+std::to_string(record));
            report_queued();
        };
        shader_artifact_storage::BackgroundScope foreground(false);
        const char* retain=diag_getenv("LARECOMP_SHADER_WARM_RETAIN");
        const bool background_list=options.background && !title_only && (!retain || atoi(retain)!=0);
        if(background_list) {
            // Recent states and current compute inputs retain metadata even
            // when the shared list is larger than the optional file budget.
            auto priority=recent;priority.insert(priority.end(),compute.begin(),compute.end());
            auto selected=shader_warm::select(catalog,priority,false);
            shader_warm::require(selected.mismatched==0,"recent shader stage mismatch");
            shader_warm::plan(catalog,selected,config,translate,emit,reuse);
        }
        require_metadata=!background_list;
        shader_warm::Stats stats;
        {
            shader_artifact_storage::BackgroundScope background(background_list);
            stats=shader_warm::plan(catalog,selection,config,translate,emit,reuse);
        }
        if(progress)progress(0.8,"Preparing graphics used by the warm-up list...");
        queue.finish();
        vk_shader_artifact_flush();
        checkpoint();
        require_profile(identity_profile);
        auto seed_identity=vk_pipeline_seed_identity(143);
        if(title_only && !seeds.empty()) {
            std::vector<VkPipelineRecipe> previous(4096);
            unsigned n=vk_pipeline_seed_load((state+"/pipeline-seed.bin").c_str(),&seed_identity,previous.data(),previous.size());
            for(unsigned i=0;i<n && seeds.size()<4096;++i) {
                bool found=false;for(const auto& seed:seeds)if(!memcmp(&seed,&previous[i],sizeof seed)){found=true;break;}
                if(!found)seeds.push_back(previous[i]);
            }
            if(!vk_pipeline_seed_save((state+"/pipeline-seed.bin").c_str(),&seed_identity,seeds.data(),seeds.size()))fprintf(log,"pipeline_seed_not_saved\n");
        }
        fprintf(log,"pipeline_seed_compute=%zu driver_pipelines=0\n",seeds.size());
        if(!save_receipt(receipt,identity,keys))fprintf(log,"receipt_not_saved\n");
        double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        fprintf(log,"result requests=%zu source_requests=%zu unique_sources=%zu vertex=%zu fragment=%zu compute=%zu unpaired_fragments=%zu seconds=%.3f driver_pipelines=0\n",
                stats.requests,stats.sources,keys.size(),stats.stages[0],stats.stages[1],stats.stages[5],stats.unpaired_fragments,seconds);
        fprintf(log,"request_cache reused=%zu masks=%zu translated=%zu entries=%zu\n",cached_requests,cached_masks,translations,request_cache.size());
        close_log();if(capacity)why[0]=0;
        fprintf(stderr,"[shader-warm] prepared title=%d requests=%zu source_requests=%zu modules=%zu seconds=%.3f\n",
                title_only,stats.requests,stats.sources,keys.size(),seconds);
        fprintf(stderr,"[shader-warm] request-cache title=%d reused=%zu masks=%zu translated=%zu\n",title_only,cached_requests,cached_masks,translations);
        report_reuse();
        // No .shaders-ready marker: a warm list is deliberately not exhaustive.
        if(progress)progress(1,"Graphics warm-up complete.");return true;
    } catch(const std::exception& error) {
        snprintf(why,capacity,"%s",error.what());
        if(log){fprintf(log,"request_cache reused=%zu masks=%zu translated=%zu\nfailed %s\n",cached_requests,cached_masks,translations,error.what());close_log();}
        fprintf(stderr,"[shader-warm] interrupted request-cache reused=%zu masks=%zu translated=%zu\n",cached_requests,cached_masks,translations);
        report_reuse();
        return false;
    }
}

extern "C" bool vk_shader_warm_game(const char* game,const char* manifest,bool title_only,
                                    void (*progress)(double,const char*),char* why,size_t capacity) {
    return shader_warm::prepare(game,manifest,title_only,progress?shader_warm::Progress(progress):shader_warm::Progress{},why,capacity,{});
}
extern "C" bool vk_shader_pipeline_seed_path(const char* game,char* out,size_t capacity) {
    if(!game || !out || !capacity)return false;
    try{auto path=shader_warm::state_directory(game)+"/pipeline-seed.bin";
        if(path.size()>=capacity)return false;memcpy(out,path.c_str(),path.size()+1);return true;
    }catch(...){return false;}
}
