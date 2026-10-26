// Real service-handler checks, without game code or an audio device.
#include <assert.h>
#include "../runtime/services/audio.c"

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
void plat_write_prepare(uint64_t a, uint64_t n) { (void)a; (void)n; }
void rt_fatal_trace(void) { abort(); }
static KEvent events[3];
static int event_count;
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) {
    *w = 100 + 2 * event_count; *r = *w + 1;
    return &events[event_count++];
}
uint32_t kernel_new_handle(KObject *o) { (void)o; return 200; }
void kernel_signal(KObject *o) { o->signaled = true; }

int main(void) {
    uint8_t data[16] = {0};
    char names[0x300];
    IpcReq req = {.data = data, .data_size = sizeof data, .nb = 1};
    req.b[0] = (IpcBuf){(uint64_t)(uintptr_t)names, sizeof names};
    for (int k = 0; k < 3; k++) {
        req.cmd = (uint32_t[]){0, 6, 14}[k];
        IpcResp resp = {0};
        memset(names, 0xff, sizeof names);
        audio_device(NULL, &req, &resp);
        assert(!resp.result && resp.data_size == 4 && *(uint32_t *)resp.data == 3);
        assert(!strcmp(names, "AudioStereoJackOutput"));
        assert(!strcmp(names + 0x100, "AudioBuiltInSpeakerOutput"));
        assert(!strcmp(names + 0x200, "AudioTvOutput"));
        assert(names[0xff] == 0 && names[0x1ff] == 0);
    }
    req.cmd = 13;
    IpcResp resp = {0};
    audio_device(NULL, &req, &resp);
    assert(!strcmp(names, "AudioTvOutput"));
    req.cmd = 14; req.b[0].size = 0x80;
    resp = (IpcResp){0}; audio_device(NULL, &req, &resp);
    assert(resp.data_size == 4 && *(uint32_t *)resp.data == 0);

    strcpy(names, "AudioUsbDeviceOutput");
    req.nb = 0; req.na = 1;
    req.a[0] = (IpcBuf){(uint64_t)(uintptr_t)names, 0x100};
    req.cmd = 7; *(float *)data = 128;
    resp = (IpcResp){0}; audio_device(NULL, &req, &resp); assert(!resp.result);
    req.cmd = 8;
    resp = (IpcResp){0}; audio_device(NULL, &req, &resp);
    assert(resp.data_size == 4 && *(float *)resp.data == 128);
    req.cmd = 1; *(float *)data = 129;
    resp = (IpcResp){0}; audio_device(NULL, &req, &resp); assert(resp.result);
    req.cmd = 4; req.na = 0;
    resp = (IpcResp){0}; audio_device(NULL, &req, &resp);
    assert(resp.ncopy == 1 && route_events[0]->signaled);
    req.cmd = 11;
    resp = (IpcResp){0}; audio_device(NULL, &req, &resp);
    assert(resp.ncopy == 1 && !route_events[1]->signaled);
    puts("audio route tests passed: enumeration, active name, volumes and events");
}
