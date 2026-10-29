#include "../runtime/nvn/vulkan/shader_prepare_queue.hpp"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
static thread_local bool open;
static std::atomic<unsigned> sessions,opened,closed,compiled;
static std::atomic<unsigned> base_opened,base_closed,derived;
static std::atomic<bool> reject_begin;
static const auto caller=std::this_thread::get_id();
extern "C" bool vk_shader_prepare_begin(char* why,size_t size) {
    assert(!open && std::this_thread::get_id()!=caller);
    if(reject_begin){snprintf(why,size,"test compiler unavailable");return false;}
    open=true;++sessions;++opened;return true;
}
extern "C" void vk_shader_prepare_end(){assert(open);open=false;--sessions;++closed;}
extern "C" uint64_t vk_shader_prepare_key(const char* source,int stage) {
    uint64_t h=0xcbf29ce484222325ull^stage;while(*source)h=(h^(unsigned char)*source++)*0x100000001b3ull;return h;
}
extern "C" bool vk_shader_prepare_source(const char* source,int,const char*,char* why,size_t size) {
    assert(open && std::this_thread::get_id()!=caller);++compiled;std::this_thread::yield();
    if(!strcmp(source,"bad")){snprintf(why,size,"test compile failure");return false;}return true;
}
struct ShaderPrepareBase {std::string source;};
extern "C" ShaderPrepareBase* vk_shader_prepare_base(const char* source,int,char* why,size_t size) {
    assert(open && std::this_thread::get_id()!=caller);
    if(!strcmp(source,"bad-base")){snprintf(why,size,"test base failure");return nullptr;}
    ++base_opened;return new ShaderPrepareBase{source};
}
extern "C" void vk_shader_prepare_base_free(ShaderPrepareBase* base) {if(base){assert(open);++base_closed;delete base;}}
extern "C" bool vk_shader_prepare_variant(ShaderPrepareBase*,const ShaderPrepareVariant* v,char* why,size_t size) {
    assert(open);++derived;
    if(v->kinds[0]==3){snprintf(why,size,"test derive failure");return false;}return true;
}
int main() {
    {
        ShaderPrepareQueue queue(3);
        bool premature=false;try{queue.source_keys();}catch(const std::logic_error&){premature=true;}assert(premature);
        for(unsigned i=0;i<128;++i)queue.submit(("source"+std::to_string(i%7)).c_str(),i%2,"test");
        queue.finish();assert(queue.done()==128 && queue.unique_sources()==14 && compiled==14);
        auto keys=queue.source_keys();assert(keys.size()==14 && std::is_sorted(keys.begin(),keys.end()));
        assert(std::adjacent_find(keys.begin(),keys.end())==keys.end());
        assert(sessions==0 && opened>=1 && opened<=3 && closed==opened);
        bool rejected=false;try{queue.submit("source0",0,"closed");}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    }
    {
        ShaderPrepareQueue queue(3);std::vector<ShaderPrepareVariant> variants;
        for(unsigned i=0;i<300;++i){ShaderPrepareVariant v{};v.key=i%27;v.kinds[0]=i%3;variants.push_back(v);}
        queue.submit("base",0,"family",variants);queue.submit("base",0,"duplicate-family",variants);
        queue.finish();assert(queue.done()==600 && queue.unique_sources()==27 && queue.unique_bases()==1);
        auto keys=queue.source_keys();assert(keys.size()==27);
        for(unsigned i=0;i<27;++i)assert(keys[i]==i);
        assert(derived==27 && base_opened==1 && base_closed==1 && compiled==14 && sessions==0);
    }
    for(const char* source:{"base","bad-base"}) {
        bool failed=false;
        try{ShaderPrepareQueue queue(2);ShaderPrepareVariant v{};v.kinds[0]=3;queue.submit(source,0,"failed-family",{v});queue.finish();}
        catch(const std::runtime_error& e){failed=strstr(e.what(),"test derive failure") || strstr(e.what(),"test base failure");}
        assert(failed && base_opened==base_closed && sessions==0 && compiled==14);
    }
    bool failed=false;
    try{ShaderPrepareQueue queue(3);queue.submit("bad",0,"bad-program");queue.finish();}
    catch(const std::runtime_error& e){failed=strstr(e.what(),"test compile failure")!=nullptr;}
    assert(failed && sessions==0 && opened==closed);
    reject_begin=true;failed=false;
    try{ShaderPrepareQueue queue(3);queue.submit("valid",0,"test");queue.finish();}
    catch(const std::runtime_error& e){failed=strstr(e.what(),"test compiler unavailable")!=nullptr;}
    assert(failed && sessions==0 && opened==closed);
    reject_begin=false;
    {ShaderPrepareQueue queue(2);for(unsigned i=0;i<8;++i)queue.submit(std::to_string(i).c_str(),0,"cancel-on-destruction");}
    assert(sessions==0 && opened==closed);
    auto before=opened.load();
    {ShaderPrepareQueue queue;queue.finish();}
    assert(opened==before); // all-cache work must not need a compiler library
    bool cancelled=false;before=opened.load();
    try{ShaderPrepareQueue queue(3,[]{throw std::runtime_error("cancel before open");});queue.finish();}
    catch(const std::runtime_error& e){cancelled=!strcmp(e.what(),"cancel before open");}
    assert(cancelled && opened==before && sessions==0 && opened==closed);
    assert(sessions==0 && opened==closed);
    std::cout<<"PASS: bounded workers, source/variant deduplication, shared bases, compile/derive failure, session closure and cancellation\n";
}
