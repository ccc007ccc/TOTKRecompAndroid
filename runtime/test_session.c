#include "diagnostics_build.h"
#include "test_session.h"
#include "platform.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { LOADING, RUNNING, PAUSED, WARMUP, SAMPLING, DIAGNOSTIC, ENDED };
static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool enabled, auto_load, pause_when_ready, want_pause, gpu_request, shot_request, ready, camera_turn;
    unsigned phase, ready_frames, warm_left, sample_count, sample_goal, sample_id, diagnostic_left;
    uint64_t frame, request, last_ns, button_until, buttons, started_ns, duration_ns, sample_started_ns;
    double intervals[6000];
    char directory[1024], error[160];
    int save_slot;
} ts={.lock=PTHREAD_MUTEX_INITIALIZER, .wake=PTHREAD_COND_INITIALIZER};
static pthread_once_t once=PTHREAD_ONCE_INIT;
static _Atomic int target_fps;
static _Atomic bool exit_requested;
int test_session_target_fps(void) { return atomic_load_explicit(&target_fps,memory_order_relaxed); }
bool test_session_should_exit(void) { return atomic_load_explicit(&exit_requested,memory_order_acquire); }
static const char *phases[]={"loading","running","paused","warming","sampling","diagnostic","ended"};
static void status(void);

// Clone the complete save root, including stores/*/metadata, before guest boot.
// The old SAVE_SEED hook only seeds files at mount time; TotK creates the store
// before that hook and would silently start a new game. Never reuse a destination.
static bool clone_save(const char *src, const char *dst) {
    DIR *d=opendir(src);
    if(!d)return false;
    if(mkdir(dst,0700)){closedir(d);return false;}
    bool ok=true;
    struct dirent *e;
    while(ok&&(e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char a[2048],b[2048];
        if(snprintf(a,sizeof a,"%s/%s",src,e->d_name)>=(int)sizeof a||
           snprintf(b,sizeof b,"%s/%s",dst,e->d_name)>=(int)sizeof b){ok=false;break;}
        struct stat st;
        if(lstat(a,&st)){ok=false;break;}
        if(S_ISDIR(st.st_mode)){ok=clone_save(a,b);continue;}
        if(!S_ISREG(st.st_mode)){ok=false;break;}
        FILE *in=fopen(a,"rb"),*out=in?fopen(b,"wb"):NULL;
        if(!out){if(in)fclose(in);ok=false;break;}
        char buf[65536];size_t n;
        while((n=fread(buf,1,sizeof buf,in)))
            if(fwrite(buf,1,n,out)!=n){ok=false;break;}
        if(ferror(in))ok=false;
        fclose(in);
        if(fclose(out))ok=false;
    }
    closedir(d);return ok;
}

static bool have_slot(const char *root, int slot) {
    char stores[1200];snprintf(stores,sizeof stores,"%s/stores",root);
    DIR *d=opendir(stores);if(!d)return false;
    struct dirent *e;bool found=false;
    while((e=readdir(d))) {
        static const char prefix[]="1_1_0_0_0100f2c0115b6000_";
        if(strncmp(e->d_name,prefix,sizeof prefix-1))continue;
        char p[1800];struct stat st;
        snprintf(p,sizeof p,"%s/%s/files/slot_%02d/progress.sav",stores,e->d_name,slot);
        if(!stat(p,&st)&&S_ISREG(st.st_mode)&&st.st_size>0)found=true;
    }
    closedir(d);return found;
}

static void initialize(void) {
    const char *root=diag_getenv("LARECOMP_TEST_SESSION");
    if(!root||!*root)return;
    if(strlen(root)>=sizeof ts.directory){fprintf(stderr,"[test-session] path too long\n");exit(1);}
    ts.enabled=true;ts.pause_when_ready=true;
    const char *duration=diag_getenv("LARECOMP_TEST_DURATION_SECONDS");
    if(duration) {
        char *end;double seconds=strtod(duration,&end);
        if(!*duration||*end||!isfinite(seconds)||seconds<1||seconds>60) {
            fprintf(stderr,"[test-session] duration must be 1..60 seconds\n");exit(1);
        }
        ts.duration_ns=(uint64_t)(seconds*1000000000.0);
        ts.pause_when_ready=false;
        atomic_store_explicit(&target_fps,60,memory_order_relaxed);
    }
    ts.save_slot=-1;
    ts.camera_turn=diag_getenv("LARECOMP_TEST_CAMERA_TURN")!=NULL;
    const char *slot=diag_getenv("LARECOMP_TEST_SAVE_SLOT");
    const char *seed=diag_getenv("LARECOMP_TEST_SAVE_SEED");
    if(slot) {
        char *end;long value=strtol(slot,&end,10);
        if(!*slot||*end||value<0||value>5||!seed||!*seed) {
            fprintf(stderr,"[test-session] direct load needs slot 0..5 and an isolated test save seed\n");exit(1);
        }
        ts.save_slot=(int)value;
    }
    // Keep the normal title/list/confirmation flow: it initializes the game's
    // loading background. The save-load resource fixes the requested slot.
    ts.auto_load=ts.save_slot>=0 || diag_getenv("LARECOMP_TEST_AUTOLOAD")!=NULL;
    snprintf(ts.directory,sizeof ts.directory,"%s",root);
    if(mkdir(root,0755)&&access(root,W_OK)) {
        fprintf(stderr,"[test-session] directory is not writable: %s\n",root);exit(1);
    }
    if(seed&&*seed) {
        const char *dst=getenv("LARECOMP_SAVE_DIR");
        // The tool creates only the session root; the destination must be new.
        if(!dst||strncmp(dst,root,strlen(root))||strncmp(dst+strlen(root),"/save-",6)||!clone_save(seed,dst)) {
            fprintf(stderr,"[test-session] cannot clone test save; refusing to boot with an empty or reused save\n");exit(1);
        }
        fprintf(stderr,"[test-session] cloned complete save from %s to %s\n",seed,dst);
        if(ts.save_slot>=0&&!have_slot(dst,ts.save_slot)) {
            fprintf(stderr,"[test-session] slot_%02d/progress.sav is missing from the test copy\n",ts.save_slot);exit(1);
        }
    }
    ts.started_ns=plat_now_ns();status();
}
bool test_session_enabled(void){pthread_once(&once,initialize);return ts.enabled;}
bool test_session_timed(void){return test_session_enabled()&&ts.duration_ns!=0;}
int test_session_save_slot(void){return test_session_enabled()?ts.save_slot:-1;}
const char *test_session_directory(void){return test_session_enabled()?ts.directory:NULL;}

static void status(void) { // lock held; only transitions write to storage
    char path[1200],done[1200];snprintf(path,sizeof path,"%s/status.partial",ts.directory);
    snprintf(done,sizeof done,"%s/status.json",ts.directory);FILE *f=fopen(path,"w");if(!f)return;
    fprintf(f,"{\"pid\":%d,\"phase\":\"%s\",\"frame\":%llu,\"request\":%llu,\"sample_id\":%u,\"sample_frames\":%u,\"ready\":%s,\"save_slot\":%d,\"target_fps\":%d,\"timed\":%s,\"camera_turn\":%s,\"requested_seconds\":%.6f,\"error\":\"%s\"}\n",
        getpid(),phases[ts.phase],(unsigned long long)ts.frame,(unsigned long long)ts.request,ts.sample_id,ts.sample_count,
        ts.ready?"true":"false",ts.save_slot,test_session_target_fps(),ts.duration_ns?"true":"false",ts.camera_turn?"true":"false",ts.duration_ns/1e9,ts.error);
    if(!fclose(f))rename(path,done);
}
static int compare(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}
static bool result(uint64_t now) {
    double sum=0;for(unsigned i=0;i<ts.sample_count;i++)sum+=ts.intervals[i];
    qsort(ts.intervals,ts.sample_count,sizeof(double),compare);
    char path[1200],done[1200];++ts.sample_id;
    snprintf(path,sizeof path,"%s/sample-%04u.partial",ts.directory,ts.sample_id);
    snprintf(done,sizeof done,"%s/sample-%04u.json",ts.directory,ts.sample_id);
    FILE *f=fopen(path,"w");if(!f)return false;
    unsigned n=ts.sample_count;
    double median=n?(ts.intervals[(n-1)/2]+ts.intervals[n/2])/2:0, p95=n?ts.intervals[(95*n+99)/100-1]:0;
    double elapsed=ts.duration_ns?(now-ts.sample_started_ns)/1e9:sum/1000;
    double fps=elapsed>0?n/elapsed:0,mean=n?elapsed*1000/n:0;
    fprintf(f,"{\"pid\":%d,\"frames\":%u,\"first_frame\":%llu,\"last_frame\":%llu,\"mean_ms\":%.6f,\"median_ms\":%.6f,\"p95_ms\":%.6f,\"fps\":%.6f,\"target_fps\":%d,\"mode\":\"%s\",\"requested_seconds\":%.6f,\"elapsed_seconds\":%.6f}\n",
        getpid(),n,(unsigned long long)(n?ts.frame-n+1:0),(unsigned long long)(n?ts.frame:0),mean,median,p95,fps,test_session_target_fps(),
        ts.duration_ns?"timed-benchmark":"frame-timing",ts.duration_ns/1e9,elapsed);
    if(fclose(f)||rename(path,done))return false;
    fprintf(stderr,"[test-session] sample %u: %u frames %.2f fps, %.2f ms mean, %.2f ms p95\n",ts.sample_id,n,fps,mean,p95);
    return true;
}

static void finish_timed(uint64_t now,const char *error) { // lock held; no GPU wait or pause at either boundary
    if(ts.phase==ENDED)return;
    if(error)snprintf(ts.error,sizeof ts.error,"%s",error);
    if(ts.sample_started_ns&&!result(now)&&!ts.error[0])snprintf(ts.error,sizeof ts.error,"Cannot write benchmark result");
    ts.phase=ENDED;ts.want_pause=ts.pause_when_ready=ts.auto_load=false;ts.buttons=0;
    status();pthread_cond_broadcast(&ts.wake);
    atomic_store_explicit(&exit_requested,true,memory_order_release);
}

void test_session_backgrounded(void) {
    if(!test_session_timed())return;
    pthread_mutex_lock(&ts.lock);finish_timed(plat_now_ns(),"Benchmark left the foreground");pthread_mutex_unlock(&ts.lock);
}

void test_session_poll(void) {
    if(!test_session_enabled())return;
    static uint64_t last;uint64_t now=plat_now_ns();if(now-last<100000000)return;last=now;
    pthread_mutex_lock(&ts.lock);
    if(ts.duration_ns) {
        if(ts.phase==LOADING&&now-ts.started_ns>120000000000ull)
            finish_timed(now,"Startup exceeded 120 seconds");
        else if(ts.phase==SAMPLING&&now-ts.sample_started_ns>=ts.duration_ns)
            finish_timed(now,NULL); // still exits when the renderer stops producing frames
    }
    if(ts.phase==LOADING&&!ts.want_pause&&now-ts.started_ns>120000000000ull) {
        ts.want_pause=true;ts.auto_load=false;
        snprintf(ts.error,sizeof ts.error,"Startup exceeded 120 seconds; pausing at the next frame for inspection");
        status();
    }
    pthread_mutex_unlock(&ts.lock);
    char path[1200],command[128];snprintf(path,sizeof path,"%s/command",ts.directory);
    FILE *f=fopen(path,"r");if(!f)return;
    bool got=fgets(command,sizeof command,f)!=NULL;fclose(f);unlink(path);if(!got)return;
    unsigned long long id=0;unsigned arg=0;char verb[32]={0};
    if(sscanf(command,"%llu %31s %u",&id,verb,&arg)<2)return;
    pthread_mutex_lock(&ts.lock);
    ts.request=id;if(strcmp(verb,"status"))ts.error[0]=0;
    if(ts.duration_ns&&strcmp(verb,"status"))snprintf(ts.error,sizeof ts.error,"Timed benchmarks run at 60 FPS without pause or diagnostic commands");
    else if(ts.phase==ENDED&&strcmp(verb,"status"))snprintf(ts.error,sizeof ts.error,"Test session ended for game reboot");
    else if(!strcmp(verb,"pause")){ts.want_pause=ts.phase!=PAUSED;}
    else if(!strcmp(verb,"resume")) {
        ts.want_pause=ts.pause_when_ready=false;ts.auto_load=false;ts.phase=RUNNING;
    } else if(!strcmp(verb,"fps")) {
        if(ts.phase!=PAUSED)snprintf(ts.error,sizeof ts.error,"Pause before changing the test frame rate");
        else if(arg!=0&&arg!=30&&arg!=45&&arg!=60&&arg!=120)snprintf(ts.error,sizeof ts.error,"Frame rate must be 0, 30, 45, 60 or 120");
        else atomic_store_explicit(&target_fps,(int)arg,memory_order_relaxed);
    } else if(!strcmp(verb,"sample")&&arg>=30&&arg<=6000) {
        if(ts.phase!=PAUSED)snprintf(ts.error,sizeof ts.error,"Pause at the test view before sampling");
        else if(!ts.ready)snprintf(ts.error,sizeof ts.error,"Startup has not reached a test scene; resume and inspect before sampling");
        else {ts.sample_goal=arg;ts.sample_count=0;ts.warm_left=30;ts.phase=WARMUP;ts.want_pause=false;ts.auto_load=false;}
    } else if(!strcmp(verb,"gpu")||!strcmp(verb,"shot")) {
        if(ts.phase==SAMPLING||ts.phase==WARMUP||ts.phase==DIAGNOSTIC)snprintf(ts.error,sizeof ts.error,"Diagnostics must run outside an FPS sample");
        else {
            bool gpu=!strcmp(verb,"gpu");
            if(gpu)ts.gpu_request=true;else ts.shot_request=true;
            if(ts.phase==PAUSED){ts.phase=DIAGNOSTIC;ts.diagnostic_left=gpu?3:1;ts.want_pause=false;}
        }
    } else if(!strcmp(verb,"a")) {ts.buttons=1;ts.button_until=ts.frame+3;}
    else if(strcmp(verb,"status"))snprintf(ts.error,sizeof ts.error,"Unknown command or frame count outside 30..6000");
    status();pthread_cond_broadcast(&ts.wake);pthread_mutex_unlock(&ts.lock);
}

uint64_t test_session_buttons(uint64_t frame) {
    if(!test_session_enabled())return 0;
    pthread_mutex_lock(&ts.lock);uint64_t b=frame<ts.button_until?ts.buttons:0;
    if(ts.auto_load&&ts.phase==LOADING&&frame>=120&&frame%60<2)b|=1;
    pthread_mutex_unlock(&ts.lock);return b;
}
int32_t test_session_camera_x(void) {
    if(!test_session_enabled())return 0;
    pthread_mutex_lock(&ts.lock);
    int32_t x=ts.camera_turn&&ts.ready&&ts.duration_ns&&ts.phase==SAMPLING?16384:0;
    pthread_mutex_unlock(&ts.lock);return x;
}
void test_session_file_read(const char* path) {
    if(!path || !test_session_enabled() || ts.save_slot<0)return;
    const char* save=getenv("LARECOMP_SAVE_DIR");
    if(!save || !*save)return;
    size_t prefix=strlen(save),length=strlen(path);
    char suffix[48];snprintf(suffix,sizeof suffix,"/slot_%02d/progress.sav",ts.save_slot);
    size_t tail=strlen(suffix);
    if(length<=prefix+tail || strncmp(path,save,prefix) || path[prefix]!='/' ||
       strcmp(path+length-tail,suffix))return;
    pthread_mutex_lock(&ts.lock);
    if(ts.auto_load) {
        ts.auto_load=false;
        fprintf(stderr,"[test-session] menu confirmation complete at frame %llu; automatic input stopped\n",
                (unsigned long long)ts.frame);
    }
    pthread_mutex_unlock(&ts.lock);
}
bool test_session_take_gpu_request(void) {
    if(!test_session_enabled())return false;
    pthread_mutex_lock(&ts.lock);bool r=ts.gpu_request;ts.gpu_request=false;pthread_mutex_unlock(&ts.lock);return r;
}
bool test_session_take_shot_request(void) {
    if(!test_session_enabled())return false;
    pthread_mutex_lock(&ts.lock);bool r=ts.shot_request;ts.shot_request=false;pthread_mutex_unlock(&ts.lock);return r;
}
void test_session_end(void) {
    if(!test_session_enabled())return;
    pthread_mutex_lock(&ts.lock);
    if(ts.duration_ns){finish_timed(plat_now_ns(),"Benchmark interrupted by a game reboot");pthread_mutex_unlock(&ts.lock);return;}
    ts.phase=ENDED;ts.want_pause=ts.pause_when_ready=ts.auto_load=ts.gpu_request=ts.shot_request=false;
    atomic_store_explicit(&target_fps,0,memory_order_relaxed);
    ts.buttons=0;status();pthread_cond_broadcast(&ts.wake);pthread_mutex_unlock(&ts.lock);
}
void test_session_manual(bool resume) {
    if(!test_session_enabled())return;
    pthread_mutex_lock(&ts.lock);
    if(ts.duration_ns){pthread_mutex_unlock(&ts.lock);return;}
    if(ts.phase!=ENDED) {
        ts.auto_load=ts.pause_when_ready=false;
        ts.want_pause=!resume&&ts.phase!=PAUSED;
        if(resume)ts.phase=RUNNING;
        status();pthread_cond_broadcast(&ts.wake);
    }
    pthread_mutex_unlock(&ts.lock);
}

void test_session_frame(uint64_t frame,bool scene,void(*gpu_idle)(void)) {
    if(!test_session_enabled())return;
    pthread_mutex_lock(&ts.lock);ts.frame=frame;uint64_t now=plat_now_ns();
    if(ts.phase==ENDED){pthread_mutex_unlock(&ts.lock);return;}
    if(!ts.ready&&(ts.phase==LOADING||ts.phase==RUNNING)) {
        if(scene)ts.ready_frames++;else ts.ready_frames=0;
        if(ts.ready_frames>=90){
            ts.ready=true;ts.auto_load=false;ts.phase=RUNNING;ts.want_pause=ts.pause_when_ready;
            if(ts.duration_ns){ts.phase=SAMPLING;ts.sample_count=0;ts.sample_started_ns=now;status();}
        }
    } else if(ts.phase==WARMUP) {
        if(--ts.warm_left==0){ts.phase=SAMPLING;ts.sample_count=0;fprintf(stderr,"[test-session] sample begin frame %llu\n",(unsigned long long)frame);status();}
    } else if(ts.phase==DIAGNOSTIC) {
        if(--ts.diagnostic_left==0)ts.want_pause=true;
    } else if(ts.phase==SAMPLING) {
        if(ts.sample_count==sizeof ts.intervals/sizeof ts.intervals[0]) {
            if(ts.duration_ns)finish_timed(now,"Benchmark frame capacity exceeded before its time limit");
        } else {
            ts.intervals[ts.sample_count++]=(now-ts.last_ns)/1e6;
            if(ts.duration_ns){if(now-ts.sample_started_ns>=ts.duration_ns)finish_timed(now,NULL);}
            else if(ts.sample_count==ts.sample_goal){if(!result(now))snprintf(ts.error,sizeof ts.error,"Cannot write sample result");ts.want_pause=true;}
        }
    }
    ts.last_ns=now;
    if(ts.want_pause) {
        pthread_mutex_unlock(&ts.lock);gpu_idle();pthread_mutex_lock(&ts.lock);
        if(!ts.want_pause){ts.last_ns=plat_now_ns();pthread_mutex_unlock(&ts.lock);return;}
        ts.phase=PAUSED;ts.want_pause=false;status();
        fprintf(stderr,"[test-session] paused after frame %llu; ready for cooling or another sample\n",(unsigned long long)frame);
        while(ts.phase==PAUSED)pthread_cond_wait(&ts.wake,&ts.lock);
        ts.last_ns=plat_now_ns();
    }
    pthread_mutex_unlock(&ts.lock);
}
