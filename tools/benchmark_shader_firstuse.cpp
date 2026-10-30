// Read-only game inputs; cache and timings go into an isolated directory.
// Measures production CPU phases, never phone FPS or driver pipeline compilation.
#include "../runtime/nvn/vulkan/shader_catalog.hpp"
extern "C" {
#include "../runtime/nvn/maxwell/shader.h"
#include "../runtime/nvn/vulkan/shaders.h"
#include "../runtime/nvn/vulkan/shader_prepare.h"
VkG g_vk;
void rt_fatal_trace(void) { abort(); }
}
#include <chrono>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unistd.h>

static double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: benchmark_shader_firstuse ROMFS_IMAGE OUTPUT_DIRECTORY\n"; return 2; }
    try {
        shader_archive::Romfs rom(argv[1]);
        auto catalog = shader_catalog::read(rom, [](const std::string&){});
        std::array<size_t, 6> sizes{};
        std::array<unsigned, 6> largest{};
        for (unsigned i = 0; i < catalog.records.size(); ++i) {
            const auto& s = catalog.records[i].shader;
            if (s.stage >= sizes.size()) throw std::runtime_error("invalid catalog shader stage");
            if (s.constants > sizes[s.stage]) { sizes[s.stage] = s.constants; largest[s.stage] = i; }
        }
        if (chdir(argv[2])) throw std::runtime_error("cannot enter isolated benchmark directory");
        maxwell_gmem_base = 0x80000000ull; maxwell_gmem_size = 0x100000000ull;
        maxwell_gmem_map_all = true; maxwell_no_int64 = true; maxwell_fp32_double = true;
        maxwell_brx_window = true; maxwell_static_stacks = true; maxwell_structured = true;
        maxwell_native_half = true; maxwell_fast_minmax = true;
        char why[8192];
        if (!vk_shader_prepare_begin(why, sizeof why)) throw std::runtime_error(why);
        struct Session { ~Session() { vk_shader_prepare_end(); } } session;
        for (unsigned stage : {0u, 1u, 5u}) {
            if (!sizes[stage]) continue;
            unsigned id = largest[stage]; const auto& s = catalog.records[id].shader;
            ShaderKey key{}; key.glsl = 1; key.vs_out_mask = ~0u; key.gather_bias_bits = 8;
            auto translate = [&] {
                auto p = maxwell_translate_bounded(s.binary.data(), s.constants,
                    s.binary.size() - s.constants, stage, &key);
                if (!p || !p->msl) {
                    std::string error = p ? p->error : "allocation failed"; maxwell_free(p);
                    throw std::runtime_error(error);
                }
                return std::unique_ptr<ShaderInfo, decltype(&maxwell_free)>(p, maxwell_free);
            };
            auto info = translate();
            bool already_cached = vk_shader_cached(vk_shader_prepare_key(info->msl, stage));
            double t = now_ms();
            if (!vk_shader_prepare_source(info->msl, stage, "architecture-probe", why, sizeof why))
                throw std::runtime_error(why);
            double prepare_ms = now_ms() - t;
            double translation = 0, cache = 0, reflection = 0; size_t words = 0; int bindings = 0;
            unsigned instructions = info->num_insts; size_t source_bytes = strlen(info->msl);
            info.reset();
            for (unsigned repeat = 0; repeat < 5; ++repeat) {
                t = now_ms(); info = translate(); translation += now_ms() - t;
                t = now_ms();
                auto spv = vk_load_prepared_shader(info->msl, stage, "strict-cache-probe", &words);
                cache += now_ms() - t;
                VkBindingInfo out[96]; t = now_ms(); bindings = vk_reflect(spv, words, out, 96);
                reflection += now_ms() - t;
                free(spv); info.reset();
            }
            std::cout << "{\"scope\":\"host_cpu_largest_static_inputs\",\"record\":" << id << ",\"stage\":" << stage
                << ",\"instructions\":" << instructions << ",\"source_bytes\":" << source_bytes << ",\"spirv_words\":" << words
                << ",\"bindings\":" << bindings << ",\"already_cached\":" << (already_cached ? "true" : "false")
                << ",\"prepare_ms\":" << prepare_ms << ",\"warm_translate_ms\":" << translation / 5
                << ",\"warm_cache_ms\":" << cache / 5 << ",\"reflect_ms\":" << reflection / 5 << ",\"repeats\":5}\n" << std::flush;
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
