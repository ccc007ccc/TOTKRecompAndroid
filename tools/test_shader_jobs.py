"""Run the production translation/compiler queues and draw wait with real threads."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1] if len(sys.argv) > 1 else root / ".work/shader-jobs").resolve()
out.mkdir(parents=True, exist_ok=True)
source = (root / "runtime/nvn/vulkan/exec.c").read_text()
queue = source[source.index("typedef struct Job {"):source.index("static bool rt_is_persistent(")]
preamble = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
static int SDL_GetNumLogicalCPUCores(void) { return 8; }
static void vk_shader_prepare_configure(void) {}
static uint64_t plat_now_ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
'''
test = r'''
static _Atomic int gate, entered, released, drained;
static void build(void *p) { usleep(1000); atomic_store((_Atomic int*)p,1); }
static void fail_build(void *p) { atomic_store((_Atomic int*)p,2); }
static void translate(void *p) { run_job(build,p); }
static void gated(void *p) { (void)p;entered=1;while(!gate)usleep(100);released=1; }
static void late_translate(void *p) { entered=1;while(!gate)usleep(100);run_job(build,p); }
static void *drain(void *p) { (void)p;vk_shader_jobs_drain();drained=1;return NULL; }
static void *request(void *p) { run_translate(translate,p);wait_built(p);assert(atomic_load((_Atomic int*)p)==1);return NULL; }
int main(int argc,char **argv) {
    (void)argv;setenv("LARECOMP_SHADER_WORKERS","2",1);
    assert(!async_skip_draws());
    if(argc==2) {
        setenv("LARECOMP_SYNC_SHADERS","1",1);_Atomic int state=0;
        run_translate(translate,&state);assert(state==1 && !jobs_workers && !translator_started);wait_built(&state);
        vk_shader_jobs_drain();puts("PASS: synchronous diagnostic builds finish without queue workers");return 0;
    }
    _Atomic int states[64]={0};pthread_t callers[64];
    for(unsigned i=0;i<64;++i)assert(!pthread_create(&callers[i],NULL,request,&states[i]));
    for(unsigned i=0;i<64;++i)assert(!pthread_join(callers[i],NULL));
    assert(jobs_workers==2 && translator_started && stat_build_wait_ns>0);
    _Atomic int failure=0;run_job(fail_build,&failure);wait_built(&failure);assert(failure==2);
    // A first-use draw waits only for its own result; unrelated work may continue.
    run_job(gated,NULL);while(!entered)usleep(100);
    _Atomic int fast=0;run_job(build,&fast);wait_built(&fast);assert(fast==1 && !released);
    gate=1;vk_shader_jobs_drain();assert(released);
    // Reboot drain must include compiler work enqueued by a late translator.
    gate=entered=drained=0;_Atomic int last=0;
    run_translate(late_translate,&last);while(!entered)usleep(100);
    pthread_t joiner;assert(!pthread_create(&joiner,NULL,drain,NULL));usleep(10000);assert(!drained);
    gate=1;assert(!pthread_join(joiner,NULL));assert(drained && last==1);
    puts("PASS: concurrent first use, per-result waits, failed result wakeup, translation-to-compiler handoff, reboot drains both queues");
}
'''
(out / "jobs.c").write_text(preamble + queue + test)
subprocess.run(["clang", "-O1", "-g", "-fsanitize=address,undefined", "-pthread", str(out / "jobs.c"),
                "-o", str(out / "test-jobs")], check=True)
for args in ([], ["--sync"]):
    subprocess.run([str(out / "test-jobs"), *args], check=True, timeout=30)
