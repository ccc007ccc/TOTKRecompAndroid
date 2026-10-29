// Shared receipt format for title/background and full-library preparation.
// A receipt records work; only a successful module check certifies its cache.
#pragma once
#include "shader_prepare.h"
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <set>
#include <string>
#include <sys/stat.h>

namespace shader_receipt {
inline bool header(std::ifstream& file, const std::string& path, const std::string& identity,
                   size_t& count, size_t limit) {
    struct stat st{};
    if(identity.empty() || stat(path.c_str(), &st) || st.st_size < 0 ||
       uint64_t(st.st_size) > uint64_t(limit)*17 + 1024)return false;
    file.open(path);
    // Bound the header independently of file size before allocating a string.
    char line[1024]{};
    if(!file.getline(line, sizeof line) || std::string(line,size_t(file.gcount()-1)) != identity || !(file >> count) || count > limit)return false;
    return true;
}
inline bool check(const std::string& path, const std::string& identity, size_t& count,
                  size_t limit=262144, const std::function<void()>& checkpoint={},
                  const std::function<void(size_t,size_t)>& progress={}) {
    std::ifstream file;
    if(!header(file,path,identity,count,limit))return false;
    uint64_t previous=0;
    for(size_t i=0;i<count;++i) {
        if(checkpoint)checkpoint();
        char key[17]{};
        file >> std::setw(sizeof key) >> key;
        std::string text=key;
        if(text.size()!=16 || text.find_first_not_of("0123456789abcdef")!=std::string::npos)return false;
        auto value=std::stoull(text,nullptr,16);
        if((i && value<=previous) || !vk_shader_cached(value))return false;
        previous=value;
        if(progress && (!(i%32) || i+1==count))progress(i+1,count);
    }
    file>>std::ws;
    if(checkpoint)checkpoint();
    return file.eof();
}
template<class Keys>
bool save(const std::string& path,const std::string& identity,const Keys& keys) {
    if(identity.empty())return false;
    auto temporary=path+".part";
    std::ofstream file(temporary);file<<identity<<'\n'<<keys.size()<<'\n';
    for(auto key:keys)file<<std::hex<<std::setw(16)<<std::setfill('0')<<key<<'\n';
    file.close();if(file && !rename(temporary.c_str(),path.c_str()))return true;
    remove(temporary.c_str());return false;
}
}
