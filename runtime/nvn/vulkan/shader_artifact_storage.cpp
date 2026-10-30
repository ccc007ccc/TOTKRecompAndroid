// Bounded optional metadata publication, independent of translation identity.
#include "shader_artifact_storage.hpp"
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_set>
namespace {
using Bytes=std::vector<uint8_t>;
std::atomic<uint64_t> stored{},failed{};
std::mutex write_lock;
struct Publication {std::string path;Bytes bytes;};
struct Publisher {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Publication> queue;
    std::unordered_set<std::string> pending;
    bool started=false;
};
Publisher& publisher(){static auto *owner=new Publisher;return *owner;}
#ifndef VK_SHADER_ARTIFACT_MAX_FILES
#define VK_SHADER_ARTIFACT_MAX_FILES 32768
#endif
bool publish(const Publication& task) {
    std::lock_guard<std::mutex> lock(write_lock);
    std::error_code error;auto directory=std::filesystem::path(task.path).parent_path();
    std::filesystem::create_directories(directory,error);if(error)return false;
    // Only this optional metadata namespace is bounded/pruned. Compiled SPIR-V,
    // receipts, game inputs and player data are never removed here.
    static std::string namespace_path;
    static std::deque<std::string> order;
    static std::unordered_set<std::string> files;
    if(namespace_path!=directory.string()) {
        namespace_path=directory.string();order.clear();files.clear();
        for(std::filesystem::directory_iterator it(directory,error),end;!error && it!=end;it.increment(error)) {
            auto name=it->path().filename().string();
            if(name.size()!=68 || name.substr(64)!=".bin" || name.find_first_not_of("0123456789abcdef",0)<64 || !it->is_regular_file(error))continue;
            files.insert(it->path().string());order.push_back(it->path().string());
        }
        if(error)return false;
    }
    size_t required=files.count(task.path)?0:1;
    while(files.size()+required>VK_SHADER_ARTIFACT_MAX_FILES && !order.empty()) {
        auto oldest=std::move(order.front());order.pop_front();
        if(oldest==task.path){order.push_back(std::move(oldest));continue;}
        if(!remove(oldest.c_str()) || errno==ENOENT)files.erase(oldest);
        else {order.push_front(std::move(oldest));return false;}
    }
    if(files.size()+required>VK_SHADER_ARTIFACT_MAX_FILES)return false;
    auto tmp=task.path+".tmp";FILE *f=fopen(tmp.c_str(),"wb");if(!f)return false;
    bool ok=fwrite(task.bytes.data(),1,task.bytes.size(),f)==task.bytes.size();if(fclose(f))ok=false;
    if(ok)ok=rename(tmp.c_str(),task.path.c_str())==0;
    if(!ok){remove(tmp.c_str());return false;}
    if(files.insert(task.path).second)order.push_back(task.path);
    ++stored;return true;
}
void writer() {
    auto& owner=publisher();
    for(;;) {
        Publication task;
        {std::unique_lock<std::mutex> lock(owner.mutex);owner.changed.wait(lock,[&]{return !owner.queue.empty();});
         task=std::move(owner.queue.front());owner.queue.pop_front();}
        try{if(!publish(task))++failed;}catch(...){++failed;}
        {std::lock_guard<std::mutex> lock(owner.mutex);owner.pending.erase(task.path);owner.changed.notify_all();}
    }
}
bool queue_publication(Publication task) {
    auto& owner=publisher();std::unique_lock<std::mutex> lock(owner.mutex);
    if(!owner.started){try{std::thread(writer).detach();owner.started=true;}catch(...){lock.unlock();return publish(task);}}
    if(owner.pending.count(task.path))return true;
    if(owner.pending.size()>=128)return false;
    auto path=task.path;owner.pending.insert(path);
    try{owner.queue.push_back(std::move(task));}catch(...){owner.pending.erase(path);throw;}
    owner.changed.notify_all();return true;
}
}
namespace shader_artifact_storage {
bool enqueue(std::string path,std::vector<uint8_t> bytes){return queue_publication({std::move(path),std::move(bytes)});}
void wait(const std::string& path){auto& owner=publisher();std::unique_lock<std::mutex> lock(owner.mutex);owner.changed.wait(lock,[&]{return !owner.pending.count(path);});}
void flush(){auto& owner=publisher();std::unique_lock<std::mutex> lock(owner.mutex);owner.changed.wait(lock,[&]{return owner.pending.empty();});}
Stats stats(){return {stored.load(),failed.load()};}
}
