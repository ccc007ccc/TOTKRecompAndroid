#include "renderdoc/api/replay/renderdoc_replay.h"
// Android: set renderdoc__replay__marker=1 before launch; do not link -rdynamic,
// which exports libadrenotools private loader variables as system function names.
// Desktop: link -rdynamic so RenderDoc detects the exported replay marker.
#include <cstdio>
#include <cstdlib>
#include <functional>
#ifdef __ANDROID__
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <cstring>
extern "C" void RENDERDOC_SetReplayVulkanLibrary(void *);
#endif
extern "C" __attribute__((visibility("default"))) void renderdoc__replay__marker() {}
int main(int argc, char **argv) {
  if(argc != 2 && argc != 3) return 2;
#ifdef __ANDROID__
  const char *driver=getenv("REPLAY_DRIVER"),*hooks=getenv("REPLAY_HOOKS");
  if(!driver || !hooks) return 2;
  char folder[1024];snprintf(folder,sizeof folder,"%s",driver);
  char *slash=strrchr(folder,'/');if(!slash)return 2;
  char name[128];snprintf(name,sizeof name,"%s",slash+1);slash[1]=0;
  void *lib=adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL,ADRENOTOOLS_DRIVER_CUSTOM,nullptr,hooks,folder,name,nullptr,nullptr);
  if(!lib){fprintf(stderr,"custom loader failed\n");return 2;}
  RENDERDOC_SetReplayVulkanLibrary(lib);
#endif
  RENDERDOC_SetDebugLogFile(rdcstr(argv[1])+".replay.log");
  RENDERDOC_InitialiseReplay(GlobalEnvironment(), {});
  auto file = RENDERDOC_OpenCaptureFile();
  auto status = file->OpenFile(argv[1], "rdc", nullptr);
  if(!status.OK()) { fprintf(stderr,"OpenFile: %u\n",unsigned(status.code)); return 1; }
  fprintf(stderr,"Opening replay...\n");
  auto opened = file->OpenCapture(ReplayOptions(), nullptr);
  if(!opened.first.OK()) { fprintf(stderr,"OpenCapture: %u %s\n",unsigned(opened.first.code),opened.first.internal_msg ? opened.first.internal_msg->c_str() : ""); return 1; }
  fprintf(stderr,"Opened replay\n");
  auto replay = opened.second;
  unsigned draws = 0, last = 0;
  ResourceId output;
  std::function<void(const rdcarray<ActionDescription>&)> walk = [&](const auto &actions) {
    for(const auto &a: actions) {
      last = a.eventId > last ? a.eventId : last;
      if(a.flags & ActionFlags::Drawcall) { draws++; output = a.outputs[0]; printf("draw event %u vertices %u\n",a.eventId,a.numIndices); }
      walk(a.children);
    }
  };
  fprintf(stderr,"Reading actions\n");
  walk(replay->GetRootActions());
  replay->SetFrameEvent(last, true);
  fprintf(stderr,"Reading pixels\n");
  if(argc == 3) {
    for(const auto &t: replay->GetTextures()) if(t.width == 960 && t.height == 540) output=t.resourceId;
  }
  auto pixels = replay->GetTextureData(output, Subresource());
  printf("replay: %u draws; %zu textures; %zu shaders/resources; output %zu bytes\n",draws,replay->GetTextures().size(),replay->GetResources().size(),pixels.size());
  bool ok = (argc == 3 || draws) && !pixels.empty();
  if(argc == 3) {
    int expected=atoi(argv[2]);
    ok = pixels.size() == 960*540*4;
    for(size_t i=0;i<pixels.size() && ok;i+=4) ok=pixels[i]==expected && pixels[i+1]==expected && pixels[i+2]==expected;
    printf("replayed preview pixels: expected %d -> %s\n",expected,ok ? "PASS" : "FAIL");
  }
  replay->Shutdown(); file->Shutdown(); RENDERDOC_ShutdownReplay();
  return ok ? 0 : 1;
}
