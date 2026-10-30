// Whole-library preparation on the device. No capture/replay or desktop shader bundle.
#include "shader_archive.hpp"
#include "../../diagnostics_build.h"
#include "shader_catalog.hpp"
#include "shader_variant_plan.hpp"
#include "shader_prepare_queue.hpp"
#include "shader_artifact.h"
#include "shader_warm_manifest.hpp"
#include "shader_prepare.h"
#include "shader_cache_receipt.hpp"
#include "shader_warm_state.hpp"
#include "shader_warm_job.hpp"
#include <array>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "../maxwell/shader.h"
#include "shader_warm.h"
}
#ifndef SHADER_BUILD_ID
#define SHADER_BUILD_ID "development"
#endif
std::mutex vk_shader_preparation_mutex;
namespace {
using shader_archive::Shader;
using shader_archive::Bytes;
using shader_archive::require;
using Info = std::unique_ptr<ShaderInfo, decltype(&maxwell_free)>;
uint64_t hash(const void *data, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    auto p = static_cast<const unsigned char *>(data);
    while (n--) h = (h ^ *p++) * 0x100000001b3ull;
    return h;
}
std::string identity(const char *game) {
    if(!game || !*game)return {};
    struct stat st{}; std::string image = std::string(game) + "/romfs.bin";
    if (stat(image.c_str(), &st)) return {};
    char s[256];
    snprintf(s, sizeof s, "v%u:%s:%llu:%llu:%llu:%llu:%016llx", SHADER_PREPARE_CACHE_VERSION, SHADER_BUILD_ID,
        (unsigned long long)st.st_size, (unsigned long long)st.st_mtim.tv_sec,
        (unsigned long long)st.st_mtim.tv_nsec, (unsigned long long)st.st_ino,
        (unsigned long long)vk_shader_prepare_profile());
    return std::string("full-v1:") + s + ":" + vk_shader_cache_namespace();
}
struct TranslationScope {
    TranslationScope(){vk_shader_translate_begin();}
    ~TranslationScope(){vk_shader_prepare_configure();vk_shader_translate_end();}
};
struct Record { Shader shader; uint32_t inputs = 0, outputs = 0; };
Info translate(const Shader &s, const ShaderKey &key) {
    Info info(maxwell_translate_bounded(s.binary.data(), s.constants,
        s.binary.size() - s.constants, s.stage, &key), maxwell_free);
    if(info && info->msl) {
        auto raw=shader_warm::raw_key(s);
        vk_shader_artifact_store(raw.data(),&key,vk_shader_artifact_profile(vk_shader_prepare_profile()),info.get());
    }
    return info;
}
} // namespace
namespace {
constexpr size_t full_receipt_limit=4u*1024*1024;
std::string receipt_path(const char* game) {return shader_warm::state_directory(game)+"/full.receipt";}
bool verify_game(const char* game,const shader_warm::Checkpoint& checkpoint={},const shader_warm::Progress& progress={}) {
    auto id=identity(game);if(id.empty())return false;
    if(progress)progress(0,"Checking prepared game graphics...");
    size_t count=0;
    bool ok=shader_receipt::check(receipt_path(game),id,count,full_receipt_limit,checkpoint,
        [&](size_t done,size_t total){if(progress)progress(double(done)/total,"Checking prepared game graphics...");});
    return ok && count && identity(game)==id;
}
shader_warm::Job& check_job() {static auto* owner=new shader_warm::Job;return *owner;}
}
extern "C" bool vk_shader_game_has_receipt(const char* game) {
    auto id=identity(game);if(id.empty())return false;
    std::ifstream file;size_t count=0;
    return shader_receipt::header(file,receipt_path(game),id,count,full_receipt_limit) && count;
}
extern "C" bool vk_shader_game_ready(const char *game) {
    try{return verify_game(game);}catch(const std::exception&){return false;}
}
extern "C" void vk_shader_check_start(const char* game) {
    if(!game)return;
    std::string root=game;
    check_job().start(root+'\n'+identity(game),[root](bool first,const shader_warm::Checkpoint& checkpoint,const shader_warm::Progress& progress){
        if(first && !verify_game(root.c_str(),checkpoint,progress))
            throw std::runtime_error("Prepared graphics are missing or damaged. Prepare graphics again or enable runtime compilation.");
    });
}
extern "C" void vk_shader_check_stop(void) {check_job().stop();}
extern "C" ShaderCheckStatus vk_shader_check_status(void) {
    auto state=check_job().status();ShaderCheckStatus result={state.running,state.title_ready,state.failed,state.progress,{}};
    snprintf(result.message,sizeof result.message,"%s",state.message.c_str());return result;
}
extern "C" bool vk_shader_prepare_game(const char *game, void (*progress)(double, const char *), char *why, size_t capacity) {
    std::unique_lock<std::mutex> lock(vk_shader_preparation_mutex, std::try_to_lock);
    if (!lock.owns_lock()) { snprintf(why, capacity, "graphics preparation is already running"); return false; }
    const auto id = identity(game);
    if(id.empty()){snprintf(why,capacity,"game data missing");return false;}
    std::string ready=receipt_path(game),log_path=shader_warm::state_directory(game)+"/full.log";
    // A valid receipt can be reused without rebuilding the library.
    if(verify_game(game,{},progress))return true;
    // A failed retry must never leave an old completion marker behind.
    remove(ready.c_str());
    FILE *log = fopen(log_path.c_str(), "wb");
    if(!log)log=stderr;
    bool success = false;
    try {
        require(!id.empty(), "game data missing");
        require(!diag_getenv("LARECOMP_SHADER_SUBST") && !diag_getenv("LARECOMP_DUMP_VERTEX_OUTPUT"), "disable shader substitution/instrumentation before preparation");
        if (progress) progress(0, "Checking all game shaders...");
        fprintf(log, "shader-build %s\ncache-version %u\nprofile %s\n", SHADER_BUILD_ID, SHADER_PREPARE_CACHE_VERSION, id.c_str()); fflush(log);
        shader_archive::Romfs rom((std::string(game) + "/romfs.bin").c_str());
        auto catalog=shader_catalog::read(rom,[&](const std::string& name){fprintf(log,"archive %s\n",name.c_str());fflush(log);});
        auto partners=shader_catalog::vertex_partners(catalog);
        std::vector<Record> records;
        records.reserve(catalog.records.size());
        for(auto& r:catalog.records)records.push_back({std::move(r.shader)});
        size_t selectors=0,pairs=0;for(const auto& m:catalog.models)selectors+=m.programs.size();
        for(const auto& p:partners)pairs+=p.size();
        fprintf(log,"catalog models=%zu program_selectors=%zu named_programs=%zu explicit_pairs=%zu\n",catalog.models.size(),selectors,catalog.named_programs.size(),pairs);
        require(!records.empty(), "no game shaders found");
        std::vector<shader_variant_plan::Usage> usage(records.size());
        TranslationScope translation_scope;
        fprintf(log, "double-arithmetic %s\n", maxwell_fp32_double ? "fp32-preferred" : "integer-binary64"); fflush(log);
        unsigned failed = 0; std::array<unsigned, 6> stages{};
        std::set<uint32_t> vertex_outputs;
        // Preflight the entire library before spending time/space on variants.
        // A missing opcode, including in a currently unused shader, blocks ready.
        for (size_t i = 0; i < records.size(); ++i) {
            auto &r = records[i]; stages[r.shader.stage]++;
            ShaderKey k{}; k.glsl = 1; k.vs_out_mask = ~0u; k.gather_bias_bits = vk_shader_prepare_gather();
            k.vertex_snap = r.shader.stage == STAGE_VERTEX && vk_shader_prepare_vertex_snap();
            auto si = translate(r.shader, k);
            if (!si || !si->msl) {
                fprintf(log, "translation-failed stage=%u code=%016llx reason=%s\n", r.shader.stage,
                    (unsigned long long)hash(r.shader.binary.data(), r.shader.binary.size()), si ? si->error : "out of memory");
                ++failed;
            } else {
                r.inputs = si->attr_mask; r.outputs = si->out_mask;
                usage[i]={si->attr_mask,si->out_mask,si->uses_gmem,si->uses_discard,si->writes_depth,si->uses_gmem_store,si->image_format_mask};
                if (r.shader.stage == STAGE_VERTEX) vertex_outputs.insert(si->out_mask);
            }
            if (progress && !(i % 32)) progress(0.1 * double(i + 1) / records.size(), "Checking all game shaders...");
        }
        fprintf(log, "inventory total=%zu vertex=%u fragment=%u compute=%u translation_failures=%u\n",
            records.size(), stages[0], stages[1], stages[5], failed); fflush(log);
        if (failed) throw std::runtime_error(std::to_string(failed) + " game shaders are not supported by this translator. See full.log; graphics are not ready.");
        // Preserve source-defined links; unknown interface fields stay general.
        size_t linked_fragments=0,known_masks=0,unlinked_fragments=0;
        for(size_t i=0;i<records.size();++i)if(records[i].shader.stage==STAGE_FRAGMENT) {
            if(partners[i].empty()){++unlinked_fragments;continue;}
            ++linked_fragments;std::set<uint32_t> masks;
            for(auto vertex:partners[i])masks.insert(records.at(vertex).outputs & records[i].inputs);
            known_masks+=masks.size();
        }
        fprintf(log,"catalog linked_fragments=%zu explicit_input_masks=%zu unlinked_fragments=%zu runtime_state_complete=0\n",
                linked_fragments,known_masks,unlinked_fragments);fflush(log);
        require(!vertex_outputs.empty(), "shader library has no vertex stage");
        auto plan=shader_variant_plan::build(rom,catalog,usage,[&](size_t n,size_t total){
            fprintf(log,"model-layouts %zu/%zu\n",n,total);fflush(log);
            if(progress)progress(0.1+0.1*double(n)/total,"Checking model shader layouts...");
        });
        catalog={}; // option tables are not needed during SPIR-V compilation
        uint64_t total=plan.total,done=0;
        fprintf(log,"variants planned=%llu previous_enumeration=%llu vertex_types=%llu previous_vertex_types=%llu asset_vertices=%zu general_vertices=%zu\n",
                (unsigned long long)total,(unsigned long long)plan.baseline,(unsigned long long)plan.vertex_types,
                (unsigned long long)plan.baseline_vertex_types,plan.asset_vertices,plan.general_vertices);fflush(log);
        ShaderPrepareQueue compilers;
        for (size_t i=0;i<records.size();++i) {
            const auto& r=records[i];const auto& entry=plan.entries[i];
            auto count=shader_variant_plan::type_count(entry);
            for (unsigned memory=0; memory<shader_variant_plan::memory_count(entry); ++memory) {
                // Heap import, heap windows, and CPU copy mode. The latter's
                // diagnostic writes affect early-fragment-tests and descriptors.
                maxwell_gmem_map_all = memory != 0;
                maxwell_gmem_miss_log = memory == 2;
                auto flags=shader_variant_plan::state_count(entry,memory);
                for (auto mask : entry.varying_masks) for (unsigned f=0; f<flags; ++f)
                for (uint64_t image=0;image<shader_variant_plan::image_type_count(entry);++image) {
                    ShaderKey canonical{};shader_variant_plan::key(entry,0,f,mask,canonical);canonical.gather_bias_bits=vk_shader_prepare_gather();
                    canonical.vertex_snap=r.shader.stage==STAGE_VERTEX&&vk_shader_prepare_vertex_snap();
                    // Image scalar types are independently compiled families, not
                    // vertex/fragment interface rewrites of the float image module.
                    shader_variant_plan::image_key(entry,image,canonical);
                    memset(canonical.attr_kind,0,sizeof canonical.attr_kind);memset(canonical.rt_kind,0,sizeof canonical.rt_kind);
                    auto base=translate(r.shader,canonical);
                    require(base && base->msl,base?base->error:"base shader translation allocation failed");
                    constexpr size_t batch=2048;
                    std::vector<ShaderPrepareVariant> variants;variants.reserve(std::min<uint64_t>(count,batch));
                    for (uint64_t t=0; t<count; ++t) {
                        ShaderKey k{};shader_variant_plan::key(entry,t,f,mask,k);k.gather_bias_bits=vk_shader_prepare_gather();
                        k.vertex_snap=r.shader.stage==STAGE_VERTEX&&vk_shader_prepare_vertex_snap();
                        shader_variant_plan::image_key(entry,image,k);
                        auto si=translate(r.shader,k);
                        require(si && si->msl, si ? si->error : "shader translation allocation failed");
                        ShaderPrepareVariant variant{};variant.key=vk_shader_prepare_key(si->msl,r.shader.stage);
                        if(r.shader.stage==STAGE_VERTEX)std::copy(std::begin(k.attr_kind),std::end(k.attr_kind),variant.kinds);
                        else if(r.shader.stage==STAGE_FRAGMENT)std::copy(std::begin(k.rt_kind),std::end(k.rt_kind),variant.kinds);
                        variants.push_back(variant);++done;
                        if(variants.size()==batch){
                            compilers.submit(base->msl,r.shader.stage,"game-"+std::to_string(i),std::move(variants));
                            variants.clear();variants.reserve(std::min<uint64_t>(count-t-1,batch));
                        }
                        if (!(done%32)) {
                            auto completed=compilers.done();fprintf(log,"progress %llu/%llu\n",(unsigned long long)completed,(unsigned long long)total);fflush(log);
                            if(progress)progress(0.2+0.8*double(completed)/total,"Preparing game graphics...");
                        }
                    }
                    if(!variants.empty())compilers.submit(base->msl,r.shader.stage,"game-"+std::to_string(i),std::move(variants));
                }
            }
        }
        compilers.finish();
        fprintf(log,"prepared-source-keys %zu\nshared-base-keys %zu\n",compilers.unique_sources(),compilers.unique_bases());
        require(done==total && compilers.done()==total && identity(game)==id,"graphics profile or game data changed during preparation");
        auto keys=compilers.source_keys();
        require(!keys.empty() && keys.size()<=full_receipt_limit,"graphics receipt is too large");
        vk_shader_artifact_flush();
        require(shader_receipt::save(ready,id,keys),"cannot save graphics completion receipt");
        require(verify_game(game,{},progress),"prepared graphics could not be verified");
        fprintf(log,"complete shaders=%zu variants=%llu\n",records.size(),(unsigned long long)done);
        if(progress)progress(1,"Game graphics ready.");success=true;
    } catch (const std::exception &e) {
        remove(ready.c_str());
        snprintf(why,capacity,"%s",e.what());fprintf(log,"incomplete %s\n",e.what());
    }
    if(log==stderr)fflush(log);else fclose(log);return success;
}
