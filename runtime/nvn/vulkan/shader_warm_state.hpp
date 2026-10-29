// Preparation metadata belongs to the app's writable cache, not the player's
// game directory. Canonical input paths keep aliases together and games apart.
#pragma once
#include <filesystem>
#include <string>
extern "C" {
#include "../../../third_party/d2r-digest/include/CommonCrypto/CommonDigest.h"
}
namespace shader_warm {
inline std::string state_directory(const char *game) {
    std::error_code error;
    auto input=std::filesystem::path(game)/"romfs.bin";
    auto absolute=std::filesystem::absolute(input,error);
    if(error)absolute=input;
    auto canonical=std::filesystem::weakly_canonical(absolute,error);
    auto name=(error?absolute.lexically_normal():canonical).generic_string();
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(name.data(),static_cast<CC_LONG>(name.size()),hash);
    static const char hex[]="0123456789abcdef";std::string key;
    for(auto value:hash){key.push_back(hex[value>>4]);key.push_back(hex[value&15]);}
    auto directory=std::filesystem::path("cache")/"shader-warm"/key;
    // A metadata write failure is reported by its caller; it must not invalidate
    // otherwise readable game files or already prepared shader modules.
    std::filesystem::create_directories(directory,error);
    return directory.generic_string();
}
}
