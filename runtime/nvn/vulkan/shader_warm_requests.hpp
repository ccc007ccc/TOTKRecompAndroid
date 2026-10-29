// Native counterpart of prepare_shader_seed.py's TranslationCache: keep each
// completed request independently of the changing warm list. Vulkan only needs
// the exact source key and VS output mask here, not a second copy of the GLSL.
#pragma once
#include "shader_warm_manifest.hpp"
#include <filesystem>
#include <map>
#include <unordered_map>
extern "C" {
#include "../maxwell/shader.h"
}

namespace shader_warm {
class RequestCache {
public:
    struct Result {uint64_t source=0;uint32_t outputs=0;uint8_t stage=0;};
    static constexpr size_t HeaderBytes=40,EntryBytes=80,MaxEntries=131072,MaxRecords=2*MaxEntries;
private:
    std::map<RawKey,Result> entries;
    std::unordered_map<const shader_archive::Shader*,RawKey> raw_keys;
    std::string path;
    FILE* file=nullptr;
    size_t records=0,unflushed=0;
    static RawKey digest(const void* data,size_t size) {
        RawKey result;CC_SHA256(data,static_cast<CC_LONG>(size),result.data());return result;
    }
    static void word(unsigned char* p,uint64_t value,unsigned bytes) {
        for(unsigned i=0;i<bytes;++i)p[i]=static_cast<unsigned char>(value>>(8*i));
    }
    static uint64_t number(const unsigned char* p,unsigned bytes) {
        uint64_t result=0;for(unsigned i=0;i<bytes;++i)result|=uint64_t(p[i])<<(8*i);return result;
    }
    static bool equal(const Result& a,const Result& b) {return a.source==b.source && a.outputs==b.outputs && a.stage==b.stage;}
public:
    RequestCache(const std::string& directory,const std::string& identity) {
        auto hash=digest(identity.data(),identity.size());std::string name;
        static const char hex[]="0123456789abcdef";
        for(auto value:hash){name.push_back(hex[value>>4]);name.push_back(hex[value&15]);}
        auto folder=std::filesystem::path(directory)/"requests";
        std::error_code error;std::filesystem::create_directories(folder,error);
        path=(folder/(name+".bin")).string();
        unsigned char header[HeaderBytes]{};memcpy(header,"TKSWREQ1",8);memcpy(header+8,hash.data(),hash.size());
        size_t valid_bytes=HeaderBytes;
        bool valid=false;
        if(FILE* input=fopen(path.c_str(),"rb")) {
            unsigned char saved[HeaderBytes];valid=fread(saved,1,sizeof saved,input)==sizeof saved && !memcmp(saved,header,sizeof saved);
            unsigned char row[EntryBytes];
            while(valid && records<MaxRecords && fread(row,1,sizeof row,input)==sizeof row) {
                auto checksum=digest(row,48);
                if(memcmp(checksum.data(),row+48,32) || row[45] || row[46] || row[47] ||
                   (row[44]!=0 && row[44]!=1 && row[44]!=5) || !number(row+32,8))break;
                RawKey key;memcpy(key.data(),row,32);
                auto it=entries.find(key);
                if(it==entries.end() && entries.size()==MaxEntries)break;
                entries[key]={number(row+32,8),static_cast<uint32_t>(number(row+40,4)),row[44]};
                ++records;valid_bytes+=sizeof row;
            }
            fclose(input);
        }
        if(valid) {
            // Keep the valid prefix after an interrupted append. Never append
            // behind a torn record, which would hide all subsequent entries.
            std::filesystem::resize_file(path,valid_bytes,error);
            if(!error)file=fopen(path.c_str(),"ab");
        } else {
            auto temporary=path+".part";
            if(FILE* output=fopen(temporary.c_str(),"wb")) {
                bool ok=fwrite(header,1,sizeof header,output)==sizeof header;
                if(fclose(output))ok=false;
                if(ok && !rename(temporary.c_str(),path.c_str()))file=fopen(path.c_str(),"ab");
                else remove(temporary.c_str());
            }
        }
    }
    ~RequestCache(){if(file)fclose(file);}
    RequestCache(const RequestCache&)=delete;
    RequestCache& operator=(const RequestCache&)=delete;
    const std::string& filename() const{return path;}
    size_t size() const{return entries.size();}
    RawKey key(const shader_archive::Shader& shader,const ShaderKey& state) {
        auto at=raw_keys.find(&shader);
        if(at==raw_keys.end())at=raw_keys.emplace(&shader,raw_key(shader)).first;
        shader_archive::Bytes bytes(at->second.begin(),at->second.end());
        bytes.push_back(shader.stage);
        bytes.insert(bytes.end(),std::begin(state.attr_kind),std::end(state.attr_kind));
        bytes.insert(bytes.end(),std::begin(state.rt_kind),std::end(state.rt_kind));
        bytes.push_back(state.depth_minus_one);bytes.push_back(state.flip_y);bytes.push_back(state.flip_x);bytes.push_back(state.no_ds_write);
        for(unsigned i=0;i<4;++i)bytes.push_back(state.vs_out_mask>>(8*i));
        bytes.push_back(state.glsl);bytes.push_back(state.gmem_linear_lookup);bytes.push_back(state.gather_bias_bits);bytes.push_back(state.vertex_snap);
        bytes.insert(bytes.end(),std::begin(state.image_kind),std::end(state.image_kind));
        return digest(bytes.data(),bytes.size());
    }
    bool get(const RawKey& key,Result& result) const {
        auto at=entries.find(key);if(at==entries.end())return false;result=at->second;return true;
    }
    void put(const RawKey& key,const Result& result) {
        if(!result.source || (result.stage!=0 && result.stage!=1 && result.stage!=5))return;
        auto at=entries.find(key);
        if(at!=entries.end() && equal(at->second,result))return;
        if(at==entries.end() && entries.size()==MaxEntries)return;
        entries[key]=result;
        if(!file || records==MaxRecords)return;
        unsigned char row[EntryBytes]{};memcpy(row,key.data(),32);
        word(row+32,result.source,8);word(row+40,result.outputs,4);row[44]=result.stage;
        auto checksum=digest(row,48);memcpy(row+48,checksum.data(),32);
        if(fwrite(row,1,sizeof row,file)!=sizeof row || (++unflushed>=32 && fflush(file))) {
            fclose(file);file=nullptr;return;
        }
        if(unflushed>=32)unflushed=0;++records;
    }
};
} // namespace shader_warm
