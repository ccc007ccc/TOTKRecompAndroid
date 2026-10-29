#include "../runtime/nvn/vulkan/shader_archive.hpp"
#include <cassert>
#include <array>
#include <iostream>
using namespace shader_archive;
static void put(Bytes &b, size_t at, uint32_t v) { for (int i=0;i<4;i++) b.at(at+i)=v>>(8*i); }
static bool rejected(const std::function<void()> &fn) {try{fn();return false;}catch(const std::runtime_error&){return true;}}
int main(int argc, char **argv) {
    Bytes control(0x880), binary(0xa0);
    put(control,0,0x98761234);put(control,0x20,0x880);put(control,0x700,0xa0);put(control,0x704,0xa0);put(control,0x708,0x30);
    put(binary,0,0x12345678);put(binary,0x30,1<<10);
    unsigned count=0; auto emit=[&](Shader s){assert(s.stage==0 && s.constants==0xa0 && s.binary==binary);++count;};
    nvn(View(binary),View(control),0,emit);assert(count==1);
    assert(rejected([&]{nvn(View(binary),View(control),1,emit);}));
    for(size_t size=0;size<0x880;size++) assert(rejected([&]{nvn(View(binary),View(control.data(),size),0,emit);}));
    put(control,0x700,0xa8);assert(rejected([&]{nvn(View(binary),View(control),0,emit);}));
    assert(rejected([&]{View(binary).sub(SIZE_MAX,2);}));
    assert(rejected([&]{bfsha(View(binary),emit);}));
    assert(rejected([&]{bnsh(View(binary),emit);}));
    assert(rejected([&]{sharcb(View(binary),emit);}));
    Decoder decoder;Bytes broken{1,2,3};assert(rejected([&]{decoder.decode(broken);}));
    put(control,0x700,0xa0);
    // Library controls may carry trailing reflection data; retain every byte.
    auto extended=control;extended.resize(0x88c,0x5a);
    nvn(View(binary),View(extended),0,[&](Shader s){assert(s.control==extended);});
    // NintendoSDK/TextureShader.bnsh uses the earlier v9 fixed header.
    auto sdk=control;sdk.resize(0x7d1);put(sdk,8,9);put(sdk,0x20,0x7d1);
    nvn(View(binary),View(sdk),0,[&](Shader s){assert(s.control==sdk && s.constants==0xa0);});
    sdk.pop_back();assert(rejected([&]{nvn(View(binary),View(sdk),0,emit);}));
    // Shared-list identities hash the full compute blob, while the translator
    // must still see instruction-relative code and constant offsets.
    auto cs_control=control;Bytes cs_binary(0x180);
    for(size_t i=4;i<cs_binary.size();++i)cs_binary[i]=(unsigned char)(i*17+3);
    put(cs_binary,0,0x12345679);put(cs_control,0x700,0x160);put(cs_control,0x704,0x180);
    put(cs_control,0x708,0x100);put(cs_control,0x714,5);
    nvn(View(cs_binary),View(cs_control),5,[&](Shader s){
        assert(s.constants==0x60 && s.binary==View(cs_binary).sub(0x100,0x80).bytes());
        auto original=s.code_prefix;original.insert(original.end(),s.binary.begin(),s.binary.end());
        assert(original==cs_binary && s.control==cs_control);
    });
    assert(archive_kind("Lib/agl/resources.sarc.zs")==ArchiveKind::Sarc);
    assert(archive_kind("UI/LayoutArchive/Common.blarc.zs")==ArchiveKind::Sarc);
    assert(archive_kind("Lib/NintendoSDK/TextureShader.bnsh")==ArchiveKind::Bnsh);
    assert(archive_kind("Effect/Example.Nin_NX_NVN.esetb.byml.zs")==ArchiveKind::Effect);
    assert(archive_kind("Shader/ExternalBinaryString.bfres.mc")==ArchiveKind::None);
    // Optional real corpus: no game data is included in the repository.
    if(argc==2) {
        Romfs rom(argv[1]);std::set<Bytes> seen;std::array<unsigned,6> stages{};
        inventory(rom,[&](Shader s){if(seen.insert(input_identity(s)).second)stages[s.stage]++;},[](const std::string&){});
        std::cout<<"unique stages: "<<stages[0]<<" vertex, "<<stages[1]<<" fragment, "<<stages[5]<<" compute\n";
    }
    std::cout<<"PASS: bounded NVN/archive readers reject malformed input\n";
}
