// Bounded preparation-only workers. Each owns and closes its compiler session.
#pragma once
#include "shader_prepare.h"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

class ShaderPrepareQueue {
    struct Task {std::string source,name;int stage;std::vector<ShaderPrepareVariant> variants;};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> pending;
    std::vector<std::thread> threads;
    std::unordered_set<uint64_t> seen;
    std::unordered_set<uint64_t> bases;
    std::string error;
    size_t active=0,capacity;
    uint64_t completed=0;
    bool stopped=false;
    std::function<void()> checkpoint;
    void fail(const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex);if(error.empty())error=why;stopped=true;pending.clear();changed.notify_all();
    }
    void run() {
        char why[8192]{};
        bool began=false;
        try {
            if(checkpoint)checkpoint();
            for(;;) {
                if(checkpoint)checkpoint();
                Task task;
                {
                    std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return stopped || !pending.empty();});
                    if(stopped)break;task=std::move(pending.front());pending.pop_front();++active;changed.notify_all();
                }
                if(checkpoint)checkpoint();
                if(!began) {
                    if(!vk_shader_prepare_begin(why,sizeof why)){fail(why);return;}
                    began=true;
                }
                bool ok=true;
                if(task.variants.empty()) {
                    ok=vk_shader_prepare_source(task.source.c_str(),task.stage,task.name.c_str(),why,sizeof why);
                    if(ok){std::lock_guard<std::mutex> lock(mutex);++completed;}
                } else {
                    // One worker holds one base and one derived module at a time.
                    // Reuse the compiled dictionary for the entire interface family.
                    std::unique_ptr<ShaderPrepareBase,decltype(&vk_shader_prepare_base_free)> base(
                        vk_shader_prepare_base(task.source.c_str(),task.stage,why,sizeof why),vk_shader_prepare_base_free);
                    ok=bool(base);
                    if(ok)for(const auto& variant:task.variants) {
                        {std::lock_guard<std::mutex> lock(mutex);if(stopped)break;}
                        ok=vk_shader_prepare_variant(base.get(),&variant,why,sizeof why);
                        if(!ok){char key[32];snprintf(key,sizeof key,"/%016llx",(unsigned long long)variant.key);task.name+=key;break;}
                        std::lock_guard<std::mutex> lock(mutex);++completed;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);--active;
                    if(!ok){if(error.empty())error=task.name+": "+why;stopped=true;pending.clear();}changed.notify_all();
                }
            }
        }catch(const std::exception& e){fail(e.what());}
        if(began)vk_shader_prepare_end();
    }
    void stop_and_join() {
        {std::lock_guard<std::mutex> lock(mutex);stopped=true;pending.clear();changed.notify_all();}
        for(auto& thread:threads)if(thread.joinable())thread.join();
    }
public:
    explicit ShaderPrepareQueue(unsigned workers=0,std::function<void()> check={}):checkpoint(std::move(check)) {
        if(!workers)workers=std::max(1u,std::thread::hardware_concurrency());capacity=workers*2;
        try{for(unsigned i=0;i<workers;++i)threads.emplace_back([this]{run();});}catch(...){stop_and_join();throw;}
    }
    ~ShaderPrepareQueue(){stop_and_join();}
    ShaderPrepareQueue(const ShaderPrepareQueue&)=delete;
    void submit(const char* source,int stage,const std::string& name) {
        if(checkpoint)checkpoint();
        auto key=vk_shader_prepare_key(source,stage);
        std::unique_lock<std::mutex> lock(mutex);if(!error.empty())throw std::runtime_error(error);
        if(stopped)throw std::runtime_error("shader preparation queue closed");
        if(seen.count(key)){++completed;return;}
        changed.wait(lock,[&]{return stopped || pending.size()<capacity;});
        if(stopped)throw std::runtime_error(error.empty()?"shader preparation queue closed":error);
        // Another producer may have submitted the key while this one waited.
        if(seen.count(key)){++completed;return;}
        pending.push_back({source,name,stage,{}});seen.insert(key);changed.notify_all();
    }
    void submit(const char* base_source,int stage,const std::string& name,std::vector<ShaderPrepareVariant> variants) {
        if(checkpoint)checkpoint();
        if(variants.empty())throw std::invalid_argument("shader interface family is empty");
        auto base_key=vk_shader_prepare_key(base_source,stage);
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock,[&]{return stopped || pending.size()<capacity;});
        if(stopped)throw std::runtime_error(error.empty()?"shader preparation queue closed":error);
        variants.erase(std::remove_if(variants.begin(),variants.end(),[&](const ShaderPrepareVariant& v){
            if(seen.insert(v.key).second)return false;++completed;return true;
        }),variants.end());
        if(variants.empty())return;
        bases.insert(base_key);pending.push_back({base_source,name,stage,std::move(variants)});changed.notify_all();
    }
    uint64_t done(){std::lock_guard<std::mutex> lock(mutex);return completed;}
    size_t unique_sources(){std::lock_guard<std::mutex> lock(mutex);return seen.size();}
    size_t unique_bases(){std::lock_guard<std::mutex> lock(mutex);return bases.size();}
    // Call after finish(): only successful, fully persisted requests enter a receipt.
    std::vector<uint64_t> source_keys() {
        std::lock_guard<std::mutex> lock(mutex);
        if(!stopped || active || !error.empty())throw std::logic_error("shader preparation is not complete");
        std::vector<uint64_t> keys(seen.begin(),seen.end());std::sort(keys.begin(),keys.end());return keys;
    }
    void finish() {
        {
            std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return !error.empty() || (pending.empty() && !active);});
            stopped=true;changed.notify_all();
        }
        for(auto& thread:threads)if(thread.joinable())thread.join();
        if(!error.empty())throw std::runtime_error(error);
    }
};
