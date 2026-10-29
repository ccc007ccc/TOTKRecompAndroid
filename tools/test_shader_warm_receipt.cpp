// Cache receipts must not hide missing modules, profile changes or malformed
// metadata. The cache lookup is controlled here; full modules are tested by the
// real title preparation/strict-loader integration test.
#include "../runtime/nvn/vulkan/shader_warm.cpp"
#include <cassert>
#include <filesystem>
#include <iostream>
static std::set<uint64_t> available;
extern "C" bool vk_shader_cached(uint64_t key){return available.count(key);}

int main(int argc,char** argv) {
    assert(argc==2);std::filesystem::create_directories(argv[1]);
    auto file=std::string(argv[1])+"/receipt";size_t count=0;
    available={1,2,3};assert(save_receipt(file,"identity-a",available));
    assert(warm_cached(file,"identity-a",count) && count==3);
    assert(!warm_cached(file,"identity-b",count));
    available.erase(2);assert(!warm_cached(file,"identity-a",count));available.insert(2);
    {std::ofstream out(file,std::ios::app);out<<"trailing data\n";}
    assert(!warm_cached(file,"identity-a",count));
    for(auto content:{"identity-a\n999999999\n","identity-a\n1\nnot-a-key\n","identity-a\n2\n0000000000000001\n"}) {
        {std::ofstream out(file);out<<content;}assert(!warm_cached(file,"identity-a",count));
    }
    assert(save_receipt(file,"identity-a",available));assert(warm_cached(file,"identity-a",count));
    assert(!std::filesystem::exists(file+".part"));
    std::cout<<"PASS: warm receipt identity, missing modules, invalid keys/counts, truncation, trailing data, atomic replacement\n";
}
