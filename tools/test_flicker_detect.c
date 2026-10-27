#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/nvn/vulkan/flicker_detect.h"

int main(void) {
    enum { W=160,H=90 };
    uint8_t normal[W*H*4], changed[W*H*4];
    memset(normal,128,sizeof normal); memcpy(changed,normal,sizeof changed);
    assert(flicker_difference(normal,changed,W,H)==0);
    memset(changed,125,sizeof changed);
    assert(flicker_difference(normal,changed,W,H)<0.02f);
    memset(changed,0,sizeof changed);
    float full=flicker_difference(normal,changed,W,H);
    assert(full>0.49f && full<0.51f);
    memcpy(changed,normal,sizeof changed);
    for (int y=30;y<50;y++) for (int x=50;x<80;x++) memset(changed+(y*W+x)*4,0,4);
    float local=flicker_difference(normal,changed,W,H);
    assert(local>0.2f); // disappearing local patch, even though whole-frame mean changes little
    FlickerDetector d; flicker_detector_clear(&d);
    assert(!flicker_detector_begin(&d,0,1,0.01f,0.08f,1000000000ull));
    assert(flicker_detector_begin(&d,0,1,full,0.08f,1000000000ull));
    assert(flicker_detector_after(&d,0,full,0.08f,1030000000ull)==FLICKER_RECOVERED);
    // A longer flash keeps its original before image, replaces the peak, then captures recovery.
    assert(flicker_detector_begin(&d,2,0,0.2f,0.08f,2000000000ull));
    assert(flicker_new_peak(&d,0.5f));
    assert(d.before==2 && d.strength==0.5f);
    assert(flicker_detector_after(&d,0.4f,0.1f,0.08f,2060000000ull)==FLICKER_CONTINUE);
    assert(flicker_detector_after(&d,0.01f,0.49f,0.08f,2090000000ull)==FLICKER_RECOVERED);
    // A scene cut gets a settled candidate, never a false "recovered" label.
    assert(flicker_detector_begin(&d,0,1,0.4f,0.08f,3000000000ull));
    assert(flicker_detector_after(&d,0.4f,0,0.08f,3030000000ull)==FLICKER_CONTINUE);
    assert(flicker_detector_after(&d,0.4f,0,0.08f,3060000000ull)==FLICKER_CONTINUE);
    assert(flicker_detector_after(&d,0.4f,0,0.08f,3090000000ull)==FLICKER_SETTLED);
    assert(flicker_detector_begin(&d,0,1,0.4f,0.08f,4000000000ull));
    assert(flicker_detector_after(&d,0.3f,0.2f,0.08f,5600000000ull)==FLICKER_TIMEOUT);
    d.cooldown_ns=7000000000ull;
    assert(!flicker_detector_begin(&d,0,1,full,0.08f,6000000000ull));
    puts("PASS: static/noise, global/local changes, one-frame and longer flashes, peak retention, settled/timeout and cooldown");
}
