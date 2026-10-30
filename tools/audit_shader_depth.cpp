// Offline inventory of the production fragment-depth epilogue. Static inputs
// cannot establish the frequency of a shader in an active gameplay scene.
#include "../runtime/nvn/vulkan/shader_catalog.hpp"
extern "C" {
#include "../runtime/nvn/maxwell/shader.h"
}
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: audit_shader_depth ROMFS_IMAGE\n"; return 2; }
    try {
        shader_archive::Romfs rom(argv[1]);
        auto catalog = shader_catalog::read(rom, [](const std::string&){});
        maxwell_fp32_double = true; maxwell_no_int64 = true;
        maxwell_native_half = true; maxwell_fast_minmax = true;
        unsigned fragment = 0, depth = 0, identity = 0, failed = 0;
        for (unsigned i = 0; i < catalog.records.size(); ++i) {
            const auto& s = catalog.records[i].shader;
            if (s.stage != STAGE_FRAGMENT) continue;
            ++fragment;
            ShaderKey k{}; k.glsl = 1; k.vs_out_mask = ~0u; k.gather_bias_bits = 8;
            auto info = maxwell_translate_bounded(s.binary.data(), s.constants,
                s.binary.size() - s.constants, s.stage, &k);
            if (!info || !info->msl) { ++failed; std::cerr << "translation failed for record " << i << '\n'; }
            else if (info->writes_depth) {
                ++depth;
                // Inspect the emitted fallback, rather than guessing which
                // register the packed output mask assigns to depth.
                if (strstr(info->msl, "gl_FragDepth = gl_FragCoord.z;")) {
                    ++identity; std::cerr << "identity record " << i << " discard=" << info->uses_discard
                        << " store=" << info->uses_gmem_store << '\n';
                }
            }
            maxwell_free(info);
        }
        std::cout << "{\"scope\":\"static_fragment_inputs\",\"runtime_frequency_known\":false,\"records\":"
            << catalog.records.size() << ",\"fragment\":" << fragment << ",\"writes_depth\":" << depth
            << ",\"identity_fallback\":" << identity << ",\"translation_failures\":" << failed << "}\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
