// Portable module requests from the original shared shader list. No Metal ABI
// structures enter the runtime; every source is selected from local game bytes.
#pragma once
#include "shader_catalog.hpp"
#include <array>
#include <fstream>
#include <limits>
extern "C" {
#include "../../../third_party/d2r-digest/include/CommonCrypto/CommonDigest.h"
}

namespace shader_warm {
using RawKey = std::array<unsigned char,32>;
using shader_archive::require;
constexpr unsigned Pair=6, InferStage=255, RecordBytes=116;
struct Request {
    unsigned stage=0; bool title=false,mirror=false;
    RawKey first{},second{};
    std::array<uint8_t,32> attributes{};
    std::array<uint8_t,8> outputs{},images{};
};
inline shader_archive::Bytes encode(const Request& request) {
    shader_archive::Bytes bytes(RecordBytes);
    bytes[0]=request.stage;bytes[1]=request.title | (request.mirror<<1);
    std::copy(request.first.begin(),request.first.end(),bytes.begin()+4);
    std::copy(request.second.begin(),request.second.end(),bytes.begin()+36);
    std::copy(request.attributes.begin(),request.attributes.end(),bytes.begin()+68);
    std::copy(request.outputs.begin(),request.outputs.end(),bytes.begin()+100);
    std::copy(request.images.begin(),request.images.end(),bytes.begin()+108);
    return bytes;
}
inline RawKey raw_key(const shader_archive::Shader& shader) {
    auto control=shader_archive::View(shader.control);
    auto declared=control.u32(0x20);
    size_t length=declared>=0x7cc && declared<0x880 ? declared : 0x880;
    require(shader.control.size()>=length,"short shader control for shared-list identity");
    require(control.u32(0x704)==shader.code_prefix.size()+shader.binary.size(),"shared-list code length mismatch");
    CC_SHA256_CTX context; RawKey key;
    CC_SHA256_Init(&context);
    CC_SHA256_Update(&context,shader.control.data(),static_cast<CC_LONG>(length));
    CC_SHA256_Update(&context,shader.code_prefix.data(),static_cast<CC_LONG>(shader.code_prefix.size()));
    CC_SHA256_Update(&context,shader.binary.data(),static_cast<CC_LONG>(shader.binary.size()));
    CC_SHA256_Final(key.data(),&context);
    return key;
}
inline std::vector<Request> parse(const shader_archive::Bytes& bytes) {
    shader_archive::View data(bytes);
    require(data.n>=16 && data.magic("TKSWARM1",8) && data.u32(8)==RecordBytes,"unsupported shader warm manifest");
    auto count=data.u32(12);
    require(count<=200000 && data.n==16ull+uint64_t(count)*RecordBytes,"invalid shader warm manifest length");
    std::vector<Request> requests; requests.reserve(count);
    for(unsigned i=0;i<count;++i) {
        auto row=data.sub(16+size_t(i)*RecordBytes,RecordBytes);
        Request request; request.stage=row.p[0]; request.title=row.p[1]&1;request.mirror=row.p[1]&2;
        require((request.stage==0 || request.stage==1 || request.stage==5 || request.stage==Pair || request.stage==InferStage) &&
                !(row.p[1]&~3u) && row.u16(2)==0,"invalid shader warm request flags/stage");
        std::copy_n(row.p+4,32,request.first.begin());std::copy_n(row.p+36,32,request.second.begin());
        require(request.first!=RawKey{} && (request.stage==Pair ? request.second!=RawKey{} : request.second==RawKey{}),"invalid shader warm source keys");
        std::copy_n(row.p+68,32,request.attributes.begin());std::copy_n(row.p+100,8,request.outputs.begin());std::copy_n(row.p+108,8,request.images.begin());
        for(unsigned at=68;at<RecordBytes;++at)require(row.p[at]<=2,"invalid shader warm numeric type");
        requests.push_back(request);
    }
    return requests;
}
inline std::vector<Request> read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);require(bool(file),"cannot open shader warm manifest");
    auto size=file.tellg();require(size>=16 && size<=16+200000*RecordBytes,"shader warm manifest exceeds limit");
    shader_archive::Bytes bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);
    require(bool(file),"cannot read shader warm manifest");return parse(bytes);
}
struct Selected {Request request;unsigned first,second=std::numeric_limits<unsigned>::max();};
struct Selection {std::vector<Selected> requests;size_t missing=0,mismatched=0;};
inline Selection select(const shader_catalog::Catalog& catalog,const std::vector<Request>& requests,bool title_only) {
    std::map<RawKey,unsigned> keys;
    for(unsigned i=0;i<catalog.records.size();++i)keys.emplace(raw_key(catalog.records[i].shader),i);
    Selection selection;
    for(const auto& request:requests) {
        if(title_only && !request.title)continue;
        auto a=keys.find(request.first),b=keys.find(request.second);
        if(a==keys.end() || (request.stage==Pair && b==keys.end())){++selection.missing;continue;}
        auto stage=catalog.records[a->second].shader.stage;
        if((request.stage==Pair && (stage!=0 || catalog.records[b->second].shader.stage!=1)) ||
           (request.stage!=Pair && request.stage!=InferStage && request.stage!=stage)){++selection.mismatched;continue;}
        selection.requests.push_back({request,a->second,request.stage==Pair?b->second:std::numeric_limits<unsigned>::max()});
    }
    return selection;
}
// Upstream's seed includes a default request for each native shader. Compute is
// a small inventory here and its first-use compilation can block several seconds;
// include those defaults before the common graphics list, without type products.
inline std::vector<Request> default_compute_requests(const shader_catalog::Catalog& catalog) {
    std::vector<Request> result;
    for(const auto& record:catalog.records)if(record.shader.stage==5) {
        Request request;request.stage=5;request.first=raw_key(record.shader);result.push_back(request);
    }
    return result;
}
} // namespace shader_warm
