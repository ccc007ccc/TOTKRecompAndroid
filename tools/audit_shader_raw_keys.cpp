// Read the production catalog and export the upstream shader-list identity.
// No shader binaries leave the selected game. Build/run inside dev.
#include "../runtime/nvn/vulkan/shader_warm_manifest.hpp"
#include <iostream>
#include <iomanip>

int main(int argc, char **argv) {
    if (argc != 2) { std::cerr << "usage: audit_shader_raw_keys ROMFS\n"; return 2; }
    try {
        shader_archive::Romfs rom(argv[1]);
        auto catalog = shader_catalog::read(rom, [](const std::string&) {});
        std::cout << "# MacNativeGames raw key\tstage\tcatalog record\n";
        for (size_t i = 0; i < catalog.records.size(); ++i) {
            const auto& s = catalog.records[i].shader;
            auto digest = shader_warm::raw_key(s);
            for (auto b : digest) std::cout << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
            std::cout << std::dec << '\t' << s.stage << '\t' << i << '\n';
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
