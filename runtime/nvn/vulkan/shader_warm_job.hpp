// Lifetime and cancellation of the title -> background preparation sequence.
// The owner serializes start/stop; progress readers never block on worker joins.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace shader_warm {
using Checkpoint=std::function<void()>;
using Progress=std::function<void(double,const char*)>;
struct Cancelled:std::runtime_error {Cancelled():std::runtime_error("graphics warm-up cancelled") {}};
inline void background_priority() {
#if defined(__linux__)
    // Linux/Android niceness is per thread. Fill idle cores while gameplay's
    // ordinary-priority threads run first; never change the process's CPU mask.
    setpriority(PRIO_PROCESS,0,10);
#endif
}
class Job {
public:
    struct State {bool running=false,title_ready=false,failed=false,cancelled=false;double progress=0;std::string message;};
    using Run=std::function<void(bool,const Checkpoint&,const Progress&)>;
private:
    std::mutex control,mutex;
    std::atomic<bool> cancelled{false};
    std::thread thread;
    std::string key;
    State state;
    void join() {
        cancelled=true;
        if(thread.joinable())thread.join();
    }
public:
    ~Job(){stop();}
    void start(const std::string& identity,Run run,bool low_priority=false) {
        std::lock_guard<std::mutex> owner(control);
        if(key==identity)return;
        join();key=identity;cancelled=false;
        {std::lock_guard<std::mutex> lock(mutex);state={};state.running=true;}
        try {
            thread=std::thread([this,run=std::move(run),low_priority]{
                if(low_priority)background_priority();
                auto checkpoint=[this]{if(cancelled.load())throw Cancelled();};
                auto progress=[this](double value,const char* message){
                    std::lock_guard<std::mutex> lock(mutex);state.progress=value;state.message=message?message:"";
                };
                try {
                    checkpoint();run(true,checkpoint,progress);checkpoint();
                    {std::lock_guard<std::mutex> lock(mutex);state.title_ready=true;state.progress=0;}
                    background_priority();
                    run(false,checkpoint,progress);checkpoint();
                } catch(const std::exception& e) {
                    std::lock_guard<std::mutex> lock(mutex);
                    state.cancelled=cancelled.load();state.failed=!state.cancelled;state.message=e.what();
                }
                std::lock_guard<std::mutex> lock(mutex);state.running=false;
            });
        } catch(const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex);state.running=false;state.failed=true;state.message=e.what();
        }
    }
    void stop() {
        std::lock_guard<std::mutex> owner(control);join();key.clear();
    }
    State status(){std::lock_guard<std::mutex> lock(mutex);return state;}
};
struct Options {Checkpoint checkpoint;bool background=false;};
bool prepare(const char* game,const char* manifest,bool title_only,const Progress& progress,
             char* why,size_t capacity,const Options& options);
} // namespace shader_warm
