// Append-only observation rows. A partial last row after Android kills the app
// never invalidates earlier entries. Retain the newest 8192 distinct states.
#pragma once
#include "shader_warm_manifest.hpp"
#include "shader_warm_state.hpp"
#include <deque>

namespace shader_warm {
constexpr size_t RecentLimit=8192;
inline std::string observed_line(const Request& request) {
    auto bytes=encode(request);std::string line;line.reserve(bytes.size()*2);
    static const char hex[]="0123456789abcdef";
    for(auto byte:bytes){line.push_back(hex[byte>>4]);line.push_back(hex[byte&15]);}return line;
}
inline bool observed_parse(const std::string& line,Request& request) {
    if(line.size()!=RecordBytes*2)return false;
    shader_archive::Bytes bytes(16+RecordBytes);
    memcpy(bytes.data(),"TKSWARM1",8);bytes[8]=RecordBytes;bytes[12]=1;
    for(size_t i=0;i<RecordBytes;++i) {
        auto digit=[](char c){return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:-1;};
        auto a=digit(line[2*i]),b=digit(line[2*i+1]);if(a<0 || b<0)return false;
        bytes[16+i]=(a<<4)|b;
    }
    try{request=parse(bytes).at(0);request.title=false;return true;}catch(const std::exception&){return false;}
}
inline std::vector<Request> read_observed(const std::string& path) {
    FILE* file=fopen(path.c_str(),"rb");if(!file)return {};
    std::deque<std::pair<std::string,Request>> rows;
    if(!fseeko(file,0,SEEK_END)) {
        auto size=ftello(file);off_t start=size>8*1024*1024?size-8*1024*1024:0;
        fseeko(file,start,SEEK_SET);
        if(start){int c;while((c=fgetc(file))!=EOF && c!='\n'){};}
        char buffer[RecordBytes*2+2];
        while(fgets(buffer,sizeof buffer,file)) {
            size_t length=strlen(buffer);
            if(!length || buffer[length-1]!='\n') {int c;while((c=fgetc(file))!=EOF && c!='\n'){};continue;}
            std::string line(buffer,length-1);Request request;
            if(observed_parse(line,request))rows.emplace_back(std::move(line),request);
        }
    }
    fclose(file);
    std::set<std::string> seen;std::vector<Request> result;
    for(auto it=rows.rbegin();it!=rows.rend() && result.size()<RecentLimit;++it)
        if(seen.insert(it->first).second)result.push_back(it->second);
    return result;
}
inline std::vector<Request> read_recent_game(const char *game) {
    auto path=state_directory(game)+"/observed.tsv";
    std::error_code error;
    // Read older observations once when the new writable location has not yet
    // been populated. Leave the original metadata and game files intact.
    if(!std::filesystem::exists(path,error))return read_observed(std::string(game)+"/.shader-warm-observed.tsv");
    return read_observed(path);
}
inline bool compact_observed(const std::string& path,const std::vector<Request>& recent) {
    auto temporary=path+".part";std::ofstream file(temporary);
    file<<"# TOTK observed shader states v1: identifiers and binding types only\n";
    for(auto it=recent.rbegin();it!=recent.rend();++it)file<<observed_line(*it)<<'\n';
    file.close();if(file && !rename(temporary.c_str(),path.c_str()))return true;
    remove(temporary.c_str());return false;
}
}
