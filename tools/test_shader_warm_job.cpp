#include "../runtime/nvn/vulkan/shader_warm_job.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

template<class Predicate> static void until(Predicate done) {
    auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done()){assert(std::chrono::steady_clock::now()<limit);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
int main() {
    shader_warm::Job job;
    std::atomic<unsigned> titles{0},backgrounds{0},active{0},peak{0};
    std::atomic<bool> release_title{false};
    auto run=[&](bool title,const shader_warm::Checkpoint& check,const shader_warm::Progress& progress){
        if(title){++titles;progress(.25,"title");while(!release_title){check();std::this_thread::yield();}}
        else {
            ++backgrounds;
#if defined(__linux__)
            assert(getpriority(PRIO_PROCESS,0)>=10);
#endif
            for(;;){check();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        }
    };
    job.start("first",run);until([&]{return titles==1;});
    assert(job.status().running && !job.status().title_ready);
    job.start("first",run);assert(titles==1); // duplicate calls from the launcher's frames are idempotent
    release_title=true;until([&]{return backgrounds==1;});assert(job.status().title_ready);
    job.stop();assert(!job.status().running && job.status().cancelled && job.status().title_ready);
    release_title=false;job.start("first",run);until([&]{return titles==2;});
    job.stop();assert(!job.status().title_ready && job.status().cancelled);
    // A lifecycle observer can request cancellation while a worker is inside
    // an uncancellable compiler call; it must not join under SDL's watch lock.
    std::atomic<bool> inside{false},leave{false};
    job.start("async-cancel",[&](bool,const auto& check,const auto&){
        inside=true;while(!leave)std::this_thread::yield();check();
    });
    until([&]{return inside.load();});
    job.cancel_async();assert(job.status().running);leave=true;
    until([&]{return !job.status().running;});
    assert(job.status().cancelled && !job.status().failed);job.stop();
    job.start("failure",[](bool title,const auto&,const auto&){if(!title)throw std::runtime_error("test background failure");});
    until([&]{return !job.status().running;});assert(job.status().title_ready && job.status().failed);
    // Replacing a job waits for its owner; no previous task can still touch a
    // released game/profile after start returns for the next one.
    auto blocking=[&](bool,const auto& check,const auto&){
        auto count=++active;peak.store(std::max(peak.load(),count));
        try{for(;;){check();std::this_thread::yield();}}catch(...){--active;throw;}
    };
    job.start("old",blocking);until([&]{return active==1;});
    job.start("new",blocking);until([&]{return active==1;});job.stop();
    assert(active==0 && peak==1);
    job.start("complete",[](bool,const auto& check,const auto&){check();});until([&]{return !job.status().running;});
    assert(job.status().title_ready && !job.status().failed && !job.status().cancelled);
    std::cout<<"PASS: title/background ordering, duplicate start, cancellation, restart, replacement joins, failures and completion\n";
}
