// Opt-in, on-device flicker triage. Three bounded GPU readback slots also hold the
// before / peak / after images. No full-session video or per-frame disk writes.
#include "vk.h"
#include "flicker.h"
#include "flicker_detect.h"
#include "../../platform.h"
#include "../../paths.h"
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { FREE, PENDING, HELD };
typedef struct {
    VkBuffer buffer;
    VmaAllocation memory;
    uint8_t *pixels;
    uint64_t seq, frame, ns;
    int state;
} FlickerFrame;
static struct {
    FlickerStatus status;
    FlickerFrame frames[3];
    VkImage small;
    VmaAllocation memory;
    unsigned w, h;
    int previous;
    FlickerDetector detector;
    bool initialized, writer_live;
    pthread_t writer;
    atomic_bool done;
    struct {
        int before, peak, after, reason;
        unsigned event;
        float strength, threshold;
        uint64_t bytes;
        bool ok;
    } job;
    uint64_t bytes;
} fc = {.status.threshold = 0.08f, .previous = -1, .detector = {.before = -1, .peak = -1}};

static void report(void) {
    if (!fc.status.directory[0]) return;
    char path[1024]; snprintf(path, sizeof path, "%s/status.txt", fc.status.directory);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "active=%d\nsaved_events=%u\nskipped_frames=%u\nthreshold=%.3f\nbytes=%llu\nerror=%s\n"
               "每个 event 目录：01-before.png（变化前）、02-peak.png（变化时）、03-after.png（变化后）。\n"
               "preview.png 从左到右对应这三帧。event.json 记录帧号、时间与结束原因。\n"
               "候选也可能是转场或快速转镜头；settled/timeout/stopped 不表示已经恢复原画面。\n",
            fc.status.active, fc.status.saved, fc.status.skipped, fc.status.threshold,
            (unsigned long long)fc.bytes, fc.status.error);
    fclose(f);
}

static bool save_png(const char *path, uint8_t *pixels, unsigned w, unsigned h, uint64_t *bytes) {
    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBX32, pixels, w * 4);
    bool ok = surface && SDL_SavePNG(surface, path);
    SDL_DestroySurface(surface);
    struct stat st;
    if (ok && stat(path, &st) == 0) *bytes += st.st_size;
    return ok;
}

static void *save_event(void *unused) {
    (void)unused;
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s/event-%04u", fc.status.directory, fc.job.event);
    bool ok = mkdir(dir, 0755) == 0;
    const int slots[] = {fc.job.before, fc.job.peak, fc.job.after};
    const char *names[] = {"01-before.png", "02-peak.png", "03-after.png"};
    const unsigned pw = 320, ph = fc.h * pw / fc.w, height = ph + 8;
    uint8_t *preview = calloc(pw * 3 * height, 4);
    uint64_t bytes = 0;
    for (unsigned i = 0; i < 3 && ok; i++) {
        uint8_t *pixels = fc.frames[slots[i]].pixels;
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        ok = save_png(path, pixels, fc.w, fc.h, &bytes);
        if (preview) for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < pw; x++) {
            uint8_t *dst = preview + ((y * pw * 3) + i * pw + x) * 4;
            if (y < 8) { dst[0] = i == 1 ? 255 : 40; dst[1] = i == 0 ? 220 : 60; dst[2] = i == 2 ? 255 : 40; dst[3] = 255; }
            else memcpy(dst, pixels + (((y-8) * fc.h / ph) * fc.w + x * fc.w / pw) * 4, 4);
        }
    }
    if (ok && preview) {
        snprintf(path, sizeof path, "%s/preview.png", dir);
        ok = save_png(path, preview, pw * 3, height, &bytes);
    } else ok = false;
    free(preview);
    const char *reasons[] = {"unknown", "recovered", "settled", "timeout", "stopped", "interrupted"};
    snprintf(path, sizeof path, "%s/event.json", dir);
    FILE *meta = ok ? fopen(path, "w") : NULL;
    if (!meta) ok = false;
    if (meta) {
        fprintf(meta, "{\"reason\":\"%s\",\"score\":%.6f,\"threshold\":%.6f,\"width\":%u,\"height\":%u,\"frames\":[",
                reasons[fc.job.reason], fc.job.strength, fc.job.threshold, fc.w, fc.h);
        for (unsigned i = 0; i < 3; i++) {
            FlickerFrame *s = &fc.frames[slots[i]];
            fprintf(meta, "%s{\"file\":\"%s\",\"frame\":%llu,\"monotonic_ns\":%llu}", i ? "," : "", names[i],
                    (unsigned long long)s->frame, (unsigned long long)s->ns);
        }
        fputs("]}\n", meta); ok = fclose(meta) == 0;
    }
    if (ok) {
        snprintf(path, sizeof path, "%s/events.tsv", fc.status.directory);
        FILE *index = fopen(path, "a");
        if (index) {
            fprintf(index, "event-%04u\t%llu\t%llu\t%llu\t%.6f\t%s\n", fc.job.event,
                    (unsigned long long)fc.frames[slots[0]].frame, (unsigned long long)fc.frames[slots[1]].frame,
                    (unsigned long long)fc.frames[slots[2]].frame, fc.job.strength, reasons[fc.job.reason]);
            ok = fclose(index) == 0;
        } else ok = false;
    }
    fc.job.ok = ok; fc.job.bytes = bytes;
    atomic_store_explicit(&fc.done, true, memory_order_release);
    return NULL;
}

static void writer_finish(void) {
    if (!fc.writer_live || !atomic_load_explicit(&fc.done, memory_order_acquire)) return;
    pthread_join(fc.writer, NULL); fc.writer_live = false; fc.status.saving = false;
    fc.bytes += fc.job.bytes;
    if (fc.job.ok) {
        fc.status.saved++;
        LOG("[flicker] saved event-%04u frames %llu/%llu/%llu score %.4f -> %s\n", fc.job.event,
            (unsigned long long)fc.frames[fc.job.before].frame, (unsigned long long)fc.frames[fc.job.peak].frame,
            (unsigned long long)fc.frames[fc.job.after].frame, fc.job.strength, fc.status.directory);
    } else { snprintf(fc.status.error, sizeof fc.status.error, "Could not save capture"); fc.status.active = false; }
    for (int i = 0; i < 3; i++) fc.frames[i].state = FREE;
    fc.previous = -1; // PNG writing may span several game frames; never claim those are consecutive.
    flicker_detector_clear(&fc.detector);
    fc.detector.cooldown_ns = SDL_GetTicksNS() + 250000000ull;
    if (fc.status.saved >= 64 || fc.bytes >= (256ull << 20)) {
        snprintf(fc.status.error, sizeof fc.status.error, "Capture limit reached"); fc.status.active = false;
    }
    report();
}

FlickerStatus flicker_status(void) { writer_finish(); return fc.status; }
void flicker_set_threshold(float value) { fc.status.threshold = fminf(0.4f, fmaxf(0.02f, value)); }

static void release_gpu(void) {
    for (int i = 0; i < 3; i++) {
        if (fc.frames[i].buffer) vmaDestroyBuffer(g_vk.vma, fc.frames[i].buffer, fc.frames[i].memory);
        fc.frames[i] = (FlickerFrame){0};
    }
    if (fc.small) vmaDestroyImage(g_vk.vma, fc.small, fc.memory);
    fc.small = VK_NULL_HANDLE; fc.memory = NULL; fc.initialized = false;
}

void flicker_reset(void) {
    if (fc.writer_live) {
        while (!atomic_load_explicit(&fc.done, memory_order_acquire)) SDL_Delay(1);
        writer_finish();
    }
    fc.status.active = false; report();
    release_gpu(); fc.previous = -1; flicker_detector_clear(&fc.detector);
}

void flicker_start(void) {
    writer_finish();
    if (fc.status.active || fc.writer_live) return;
    // A previous stop can still have GPU copies pending. Do not reassign their storage to a new session.
    for (int i = 0; i < 3; i++) if (fc.frames[i].state == PENDING) return;
    float threshold = fc.status.threshold;
    fc.status = (FlickerStatus){.threshold = threshold}; fc.bytes = 0; fc.previous = -1;
    flicker_detector_clear(&fc.detector);
    for (int i = 0; i < 3; i++) fc.frames[i].state = FREE;
    char root[768], stamp[64]; time_t now = time(NULL); struct tm tm;
    localtime_r(&now, &tm); strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(root, sizeof root, "%s/flicker", rt_data_root());
    mkdir(root, 0755);
    snprintf(fc.status.directory, sizeof fc.status.directory, "%s/%s-%llu", root, stamp,
             (unsigned long long)(SDL_GetTicksNS() % 1000000));
    if (mkdir(fc.status.directory, 0755)) { snprintf(fc.status.error, sizeof fc.status.error, "Could not create capture directory"); return; }
    fc.status.active = true; report();
    LOG("[flicker] started threshold %.3f -> %s\n", threshold, fc.status.directory);
}
void flicker_stop(void) { fc.status.active = false; report(); LOG("[flicker] stopping\n"); }

static bool allocate_gpu(TexBackend *source) {
    fc.w = source->width > 960 ? 960 : source->width;
    fc.h = (uint64_t)source->height * fc.w / source->width;
    if (!fc.w || !fc.h || fc.h > 960) return false;
    VkImageCreateInfo image = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D,
        .format=VK_FORMAT_R8G8B8A8_UNORM, .extent={fc.w,fc.h,1}, .mipLevels=1, .arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo allocation = {.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &image, &allocation, &fc.small, &fc.memory, NULL) != VK_SUCCESS) return false;
    for (int i = 0; i < 3; i++) {
        VkBufferCreateInfo buffer = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=(uint64_t)fc.w*fc.h*4, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        allocation = (VmaAllocationCreateInfo){.usage=VMA_MEMORY_USAGE_AUTO,
            .flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(g_vk.vma, &buffer, &allocation, &fc.frames[i].buffer, &fc.frames[i].memory, &info) != VK_SUCCESS) return false;
        fc.frames[i].pixels = info.pMappedData;
    }
    fc.initialized = true; return true;
}

static void accept_frame(int index, bool interrupted) {
    FlickerFrame *current = &fc.frames[index];
    vmaInvalidateAllocation(g_vk.vma, current->memory, 0, VK_WHOLE_SIZE);
    current->state = HELD;
    FlickerDetector *d = &fc.detector;
    if (d->peak < 0) {
        if (fc.previous >= 0 && fc.status.active && !interrupted) {
            float change = flicker_difference(fc.frames[fc.previous].pixels, current->pixels, fc.w, fc.h);
            if (flicker_detector_begin(d, fc.previous, index, change, fc.status.threshold, current->ns)) return;
        }
        if (fc.previous >= 0) fc.frames[fc.previous].state = FREE;
        fc.previous = index;
        return;
    }
    float from_before = flicker_difference(fc.frames[d->before].pixels, current->pixels, fc.w, fc.h);
    if (fc.status.active && !interrupted && flicker_new_peak(d, from_before)) {
        fc.frames[d->peak].state = FREE; d->peak = index; return;
    }
    float from_peak = flicker_difference(fc.frames[d->peak].pixels, current->pixels, fc.w, fc.h);
    int reason = !fc.status.active ? 4 : interrupted ? 5 :
        flicker_detector_after(d, from_before, from_peak, fc.status.threshold, current->ns);
    if (!reason) { current->state = FREE; return; }
    fc.job.before=d->before; fc.job.peak=d->peak; fc.job.after=index; fc.job.reason=reason;
    fc.job.strength=d->strength; fc.job.threshold=fc.status.threshold; fc.job.event=fc.status.saved+1;
    fc.job.ok=false; fc.job.bytes=0;
    atomic_store_explicit(&fc.done, false, memory_order_relaxed);
    fc.writer_live = pthread_create(&fc.writer, NULL, save_event, NULL) == 0;
    fc.status.saving = fc.writer_live;
    if (!fc.writer_live) {
        snprintf(fc.status.error, sizeof fc.status.error, "Could not start capture writer"); fc.status.active=false;
        for (int i=0;i<3;i++) fc.frames[i].state=FREE;
        fc.previous=-1; flicker_detector_clear(d); report();
    }
}

// Render-thread hook, outside a render pass. GPU completion is polled, never waited on per frame.
void vk_flicker_frame(TexBackend *source, uint64_t frame, bool visible) {
    writer_finish();
    if (!fc.status.active && !fc.initialized) return;
    if (fc.writer_live) { if (fc.status.active) fc.status.skipped++; return; }
    uint64_t done = vk_completed_seq();
    for (;;) {
        int oldest = -1;
        for (int i=0;i<3;i++) if (fc.frames[i].state == PENDING &&
            (oldest < 0 || fc.frames[i].frame < fc.frames[oldest].frame)) oldest=i;
        if (oldest < 0 || fc.frames[oldest].seq > done) break;
        accept_frame(oldest, !visible);
        if (fc.writer_live) return;
    }
    if (!fc.status.active || !visible || !source) {
        if (!visible) {
            fc.previous=-1; flicker_detector_clear(&fc.detector);
            for (int i=0;i<3;i++) if (fc.frames[i].state==HELD) fc.frames[i].state=FREE;
        }
        if (!fc.status.active) {
            bool pending = false;
            for (int i=0;i<3;i++) pending |= fc.frames[i].state==PENDING;
            if (!pending) { release_gpu(); fc.previous=-1; flicker_detector_clear(&fc.detector); }
        }
        return;
    }
    VkFormatProperties src, dst;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, source->format, &src);
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R8G8B8A8_UNORM, &dst);
    if (!(src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || source->samples != VK_SAMPLE_COUNT_1_BIT) {
        snprintf(fc.status.error, sizeof fc.status.error, "Capture format is unsupported"); fc.status.active=false; report(); return;
    }
    if (!fc.initialized && !allocate_gpu(source)) {
        release_gpu(); snprintf(fc.status.error, sizeof fc.status.error, "Could not allocate capture memory"); fc.status.active=false; report(); return;
    }
    int index=-1; for (int i=0;i<3;i++) if (fc.frames[i].state==FREE) { index=i; break; }
    if (index<0) { fc.status.skipped++; return; }
    VkCommandBuffer cb=vk_cmd();
    vk_full_barrier(cb);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .image=fc.small,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets={{0,0,0},{source->width,source->height,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .dstOffsets={{0,0,0},{fc.w,fc.h,1}}};
    vkCmdBlitImage(cb,source->image,VK_IMAGE_LAYOUT_GENERAL,fc.small,VK_IMAGE_LAYOUT_GENERAL,1,&blit,
        src.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_full_barrier(cb);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent={fc.w,fc.h,1}};
    vkCmdCopyImageToBuffer(cb,fc.small,VK_IMAGE_LAYOUT_GENERAL,fc.frames[index].buffer,1,&copy);
    vk_readback_barrier(cb);
    fc.frames[index].seq=vk_pending_seq(); fc.frames[index].frame=frame;
    fc.frames[index].ns=SDL_GetTicksNS(); fc.frames[index].state=PENDING;
}
