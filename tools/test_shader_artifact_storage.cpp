// Inject one real std::thread creation failure, then verify the synchronous
// writer fallback and the ordinary asynchronous path with exact file bytes.
#include "../runtime/nvn/vulkan/shader_artifact_storage.hpp"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <unistd.h>
static std::atomic<bool> reject_thread{false};
static std::atomic<unsigned> rejected{0};
extern "C" int pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*run)(void*),void *arg) noexcept {
    using Create=int(*)(pthread_t*,const pthread_attr_t*,void *(*)(void*),void*);
    static auto real=reinterpret_cast<Create>(dlsym(RTLD_NEXT,"pthread_create"));assert(real);
    if(reject_thread.exchange(false)){++rejected;return EAGAIN;}return real(thread,attr,run,arg);
}
static std::vector<uint8_t> read(const std::string& path){std::ifstream f(path,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
    assert(argc==2);std::filesystem::create_directories(argv[1]);assert(!chdir(argv[1]));
    std::string path="cache/shader-artifacts/fallback/"+std::string(64,'0')+".bin";std::vector<uint8_t> expected={1,7,3,9};
    reject_thread=true;assert(shader_artifact_storage::enqueue(path,expected));assert(rejected==1 && read(path)==expected);
    expected[1]=11;assert(shader_artifact_storage::enqueue(path,expected));shader_artifact_storage::flush();assert(read(path)==expected);
    std::filesystem::create_directory(path+".tmp");assert(shader_artifact_storage::enqueue(path,{4,5}));shader_artifact_storage::flush();
    assert(read(path)==expected && shader_artifact_storage::stats().failed==1);
    std::cout<<"PASS: real writer thread creation failure falls back synchronously; queued publication/drain and failed replacement preserve exact bytes\n";
}
