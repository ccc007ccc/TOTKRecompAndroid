#define _POSIX_C_SOURCE 200809L
#include "../runtime/test_session.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static atomic_bool stop;
static atomic_bool scene=true;
static atomic_uint_fast64_t time_offset;
static atomic_uint frame,idles;
static const char *dir;
static uint64_t fixed_now;
uint64_t plat_now_ns(void){if(fixed_now)return fixed_now;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000000+t.tv_nsec+atomic_load(&time_offset);}
void plat_sleep_until_ns(uint64_t t){uint64_t now=plat_now_ns();if(t>now){struct timespec d={(t-now)/1000000000,(t-now)%1000000000};nanosleep(&d,NULL);}}
static void idle(void){atomic_fetch_add(&idles,1);}
static void *render(void *unused){(void)unused;while(!atomic_load(&stop)){plat_sleep_until_ns(plat_now_ns()+3000000);unsigned n=atomic_fetch_add(&frame,1)+1;test_session_frame(n,atomic_load(&scene),idle);}return NULL;}
static void wait_status(const char *want){
 char path[1200],text[1024];snprintf(path,sizeof path,"%s/status.json",dir);
 uint64_t end=plat_now_ns()+5000000000ull;
 while(plat_now_ns()<end){test_session_poll();FILE*f=fopen(path,"r");if(f){size_t n=fread(text,1,sizeof text-1,f);fclose(f);text[n]=0;if(strstr(text,want))return;}plat_sleep_until_ns(plat_now_ns()+10000000);}
 fprintf(stderr,"timeout waiting for %s\n",want);abort();
}
static void command(unsigned n,const char *verb){char path[1200];snprintf(path,sizeof path,"%s/command",dir);FILE*f=fopen(path,"w");assert(f);fprintf(f,"%u %s\n",n,verb);fclose(f);char expected[80];snprintf(expected,sizeof expected,"\"request\":%u,",n);wait_status(expected);}
int main(int argc,char**argv){
 assert(argc==2||argc==3);dir=argv[1];mkdir(dir,0755);setenv("LARECOMP_TEST_SESSION",dir,1);
 bool timed=argc==3&&!strncmp(argv[2],"--timed",7);
 bool tails=argc==3&&!strcmp(argv[2],"--timed-tails");
 bool actions=argc==3&&!strcmp(argv[2],"--timed-actions");
 if(tails||actions)fixed_now=1000000000ull;
 if(timed)setenv("LARECOMP_TEST_DURATION_SECONDS","1",1);
 if(actions) {
  setenv("LARECOMP_TEST_DURATION_SECONDS","60",1);
  setenv("LARECOMP_TEST_ATTACK_JUMP","1",1);
  setenv("LARECOMP_TEST_CAMERA_TURN","1",1);
  setenv("LARECOMP_TEST_MOVE_FORWARD","1",1);
 }
 assert(test_session_enabled());
 if(actions) {
  for(unsigned f=1;f<90;++f) {
   test_session_frame(f,true,idle);
   assert(!test_session_buttons(f)&&!test_session_camera_x()&&!test_session_move_y());
  }
  test_session_frame(90,true,idle);
  assert(test_session_camera_x()==32767&&test_session_move_y()==32767);
  const unsigned ms[]={0,1999,2000,2119,2120,5999,6000,6119,6120,9999,10000,10120};
  const uint64_t expected[]={0,0,8,8,0,0,4,4,0,0,8,0};
  for(unsigned i=0;i<sizeof ms/sizeof ms[0];++i) {
   fixed_now=1000000000ull+(uint64_t)ms[i]*1000000;
   assert(test_session_buttons(90)==expected[i]);
  }
  fixed_now=15000000000ull;assert(test_session_buttons(90)==4);
  test_session_backgrounded();
  assert(!test_session_buttons(90)&&!test_session_camera_x()&&!test_session_move_y());
  test_session_end();assert(!test_session_buttons(90));
  puts("PASS: full sticks and separate Y/X pulses use elapsed time only during sampling and stop on background/exit");
  return 0;
 }
 if(timed) {
  assert(test_session_timed()&&test_session_target_fps()==60&&!test_session_should_exit());
  assert(test_session_camera_x()==0&&test_session_move_y()==0);
  bool timeout=!strcmp(argv[2],"--timed-timeout"),stall=!strcmp(argv[2],"--timed-stall"),background=!strcmp(argv[2],"--timed-background");
  pthread_t timed_thread;
  if(tails) {
   for(unsigned i=1;i<=90;i++)test_session_frame(i,true,idle);
   for(unsigned i=91;i<=189;i++){fixed_now+=i==100?10000000:1000000;test_session_frame(i,true,idle);}
   fixed_now+=1000000;test_session_frame(190,true,idle);
   fixed_now+=891000000;test_session_frame(191,true,idle);
  } else if(stall) {
   for(unsigned i=1;i<=90;i++)test_session_frame(i,true,idle);
   wait_status("\"phase\":\"sampling\"");
   assert(test_session_camera_x()==(getenv("LARECOMP_TEST_CAMERA_TURN")?32767:0));
   assert(test_session_move_y()==(getenv("LARECOMP_TEST_MOVE_FORWARD")?32767:0));
   atomic_fetch_add(&time_offset,1001000000ull);test_session_poll();
  } else {
   if(timeout)atomic_store(&scene,false);
   assert(!pthread_create(&timed_thread,NULL,render,NULL));
   if(timeout)atomic_fetch_add(&time_offset,121000000000ull);
   else {wait_status("\"phase\":\"sampling\"");if(background)test_session_backgrounded();}
  }
  wait_status("\"phase\":\"ended\"");
  assert(test_session_should_exit()&&atomic_load(&idles)==0);
  assert(test_session_camera_x()==0&&test_session_move_y()==0);
  if(!stall&&!tails){atomic_store(&stop,true);pthread_join(timed_thread,NULL);}
  if(timeout){wait_status("Startup exceeded");wait_status("\"ready\":false");}
  else {
   char p[1200],text[2048];snprintf(p,sizeof p,"%s/sample-0001.json",dir);FILE*f=fopen(p,"r");assert(f);
   size_t n=fread(text,1,sizeof text-1,f);fclose(f);text[n]=0;
   assert(strstr(text,"\"target_fps\":60,")&&strstr(text,"\"mode\":\"timed-benchmark\""));
   if(tails) {
    assert(strstr(text,"\"frames\":101,")&&strstr(text,"\"p95_ms\":1.000000,"));
    assert(strstr(text,"\"p99_ms\":10.000000,")&&strstr(text,"\"max_ms\":891.000000,"));
    assert(strstr(text,"\"low_1pct_fps\":2.219756,"));
    assert(strstr(text,"\"frames_over_50ms\":1,")&&strstr(text,"\"frames_over_100ms\":1,")&&strstr(text,"\"frames_over_200ms\":1,"));
    char *at=strstr(text,"\"frame_times_ms\":[");assert(at);at+=strlen("\"frame_times_ms\":[");
    for(unsigned frame=91;frame<=191;++frame) {
     char *end;double ms=strtod(at,&end);assert(end!=at);
     assert(ms==(frame==100?10.0:frame==191?891.0:1.0));
     assert(*end==(frame==191?']':','));at=end+1;
    }
   }
   if(stall)assert(strstr(text,"\"frames\":0,")&&strstr(text,"\"fps\":0.000000,")&&strstr(text,"\"frame_times_ms\":[]"));
   else if(background)wait_status("Benchmark left the foreground");
   else {
    assert(strstr(text,"\"first_frame\":91,"));
    double elapsed=0;const char *at=strstr(text,"\"elapsed_seconds\":");assert(at);
    assert(sscanf(at,"\"elapsed_seconds\":%lf",&elapsed)==1&&elapsed>=1.0);
   }
  }
  puts("PASS: timed 60 FPS benchmark starts at readiness, never pauses, persists results, and requests automatic exit");
  return 0;
 }
 if(argc==3)atomic_store(&scene,false);
 pthread_t thread;assert(!pthread_create(&thread,NULL,render,NULL));
 if(argc==3) {
  atomic_fetch_add(&time_offset,121000000000ull);
  wait_status("\"phase\":\"paused\"");wait_status("Startup exceeded");wait_status("\"ready\":false");
  command(1,"status");wait_status("Startup exceeded");
  command(2,"shot");wait_status("\"phase\":\"paused\"");wait_status("\"ready\":false");
  command(3,"sample 30");wait_status("Startup has not reached");assert(atomic_load(&idles)==2);
  atomic_store(&stop,true);test_session_end();pthread_join(thread,NULL);
  puts("PASS: title/startup timeout stays unready after status and screenshots; FPS sampling is refused");return 0;
 }
 wait_status("\"phase\":\"paused\"");assert(atomic_load(&frame)==90&&atomic_load(&idles)==1);
 assert(test_session_move_y()==0);
 plat_sleep_until_ns(plat_now_ns()+200000000);assert(atomic_load(&frame)==90);
 assert(test_session_target_fps()==0);
 const unsigned rates[]={30,45,60,120};
 for(unsigned i=0;i<4;i++){char cmd[32];snprintf(cmd,sizeof cmd,"fps %u",rates[i]);command(20+i,cmd);assert(test_session_target_fps()==(int)rates[i]);}
 command(24,"fps 90");wait_status("Frame rate must be");assert(test_session_target_fps()==120);
 command(1,"sample 30");wait_status("\"sample_id\":1,");wait_status("\"phase\":\"paused\"");
 assert(atomic_load(&frame)==150&&atomic_load(&idles)==2);
 char path[1200],text[1024];snprintf(path,sizeof path,"%s/sample-0001.json",dir);FILE*f=fopen(path,"r");assert(f);size_t n=fread(text,1,sizeof text-1,f);fclose(f);text[n]=0;
 assert(strstr(text,"\"frames\":30,")&&strstr(text,"\"last_frame\":150,"));
 assert(strstr(text,"\"target_fps\":120,"));
 command(2,"sample 1");wait_status("outside 30..6000");assert(atomic_load(&frame)==150);
 command(3,"resume");wait_status("\"phase\":\"running\"");
 command(25,"fps 30");wait_status("Pause before changing");assert(test_session_target_fps()==120);
 plat_sleep_until_ns(plat_now_ns()+100000000);assert(atomic_load(&frame)>150);
 command(4,"pause");wait_status("\"phase\":\"paused\"");unsigned last=atomic_load(&frame);
 plat_sleep_until_ns(plat_now_ns()+200000000);assert(atomic_load(&frame)==last);
 command(5,"gpu");wait_status("\"phase\":\"paused\"");assert(atomic_load(&frame)==last+3);
 assert(test_session_take_gpu_request()&&!test_session_take_gpu_request());
 command(6,"shot");wait_status("\"phase\":\"paused\"");assert(atomic_load(&frame)==last+4);
 assert(test_session_take_shot_request()&&!test_session_take_shot_request());
 atomic_store(&stop,true);test_session_end();pthread_join(thread,NULL);
 wait_status("\"phase\":\"ended\"");
 assert(test_session_target_fps()==0);
 command(7,"sample 30");wait_status("ended for game reboot");
 puts("PASS: pause, responsive control, warmup exclusion, exact samples, diagnostics, resume, invalid commands and shutdown while paused");
}
