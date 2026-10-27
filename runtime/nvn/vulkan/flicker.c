// Opt-in, on-device GPU capture. Two slots retain complete RDC files in memfd
// and matching previews. Only selected events (or a manual frame) reach storage.
#include "vk.h"
#include "flicker.h"
#include "flicker_detect.h"
#include "renderdoc_bridge.h"
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
#include <errno.h>
#include <fcntl.h>
#include <sys/statvfs.h>

enum { FREE, HELD };
typedef struct {
    VkBuffer buffer;
    VmaAllocation memory;
    uint8_t *pixels;
    uint64_t seq, frame, ns;
    int state;
    int rdc; // -1 or an owned anonymous file descriptor
    uint64_t rdc_bytes;
} FlickerFrame;
static struct {
    FlickerStatus status;
    FlickerFrame frames[2];
    VkImage small;
    VmaAllocation memory;
    unsigned w, h;
    int previous;
    int capturing;
    bool capture_single;
    bool initialized, writer_live;
    pthread_t writer;
    atomic_bool done;
    struct {
        int before, after;
        unsigned event, count;
        float strength, threshold;
        uint64_t bytes;
        bool ok;
    } job;
    uint64_t bytes;
} fc = {.status.threshold = 0.08f, .previous = -1, .capturing = -1,
        .frames = {{.rdc=-1}, {.rdc=-1}}};

static void free_frame(int i) {
    if (fc.frames[i].rdc >= 0) close(fc.frames[i].rdc);
    fc.frames[i].rdc = -1; fc.frames[i].rdc_bytes = 0; fc.frames[i].state = FREE;
}

static void report(void) {
    if (!fc.status.directory[0]) return;
    char path[1024]; snprintf(path, sizeof path, "%s/status.txt", fc.status.directory);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "active=%d\nsaved_events=%u\nskipped_frames=%u\nthreshold=%.3f\nbytes=%llu\nerror=%s\n"
               "每个 event 目录：01-before.rdc/png（变化前）、02-after.rdc/png（超过阈值的变化帧）。\n"
               "手动单帧保存 frame.rdc、frame.png 和 preview.png；RDC 可用 RenderDoc 1.45 或更新版本回放。\n"
               "preview.png 从左到右对应变化前和变化后。event.json 记录帧号、时间和阈值。\n"
               "候选也可能是转场或快速转镜头；变化后是发生变化的帧，不表示已经恢复原画面。\n",
            fc.status.active, fc.status.saved, fc.status.skipped, fc.status.threshold,
            (unsigned long long)fc.bytes, fc.status.error);
    fclose(f);
}

static bool save_rdc(const char *path, FlickerFrame *frame, uint64_t *bytes) {
    int out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) return false;
    char block[65536]; uint64_t offset = 0; bool ok = true;
    while (offset < frame->rdc_bytes) {
        size_t size = frame->rdc_bytes - offset;
        if (size > sizeof block) size = sizeof block;
        ssize_t got = pread(frame->rdc, block, size, offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { ok = false; break; }
        ssize_t written = 0;
        while (written < got) {
            ssize_t n = write(out, block + written, got - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            written += n;
        }
        if (!ok) break;
        offset += got;
    }
    if (ok) ok = fsync(out) == 0;
    if (close(out)) ok = false;
    *bytes += offset;
    return ok;
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
    char dir[1024], final[1024], path[1200];
    snprintf(final, sizeof final, "%s/event-%04u", fc.status.directory, fc.job.event);
    snprintf(dir, sizeof dir, "%s/event-%04u.partial", fc.status.directory, fc.job.event);
    bool ok = mkdir(dir, 0755) == 0;
    const int slots[] = {fc.job.before, fc.job.after};
    const char *names[] = {"01-before.png", "02-after.png"};
    const char *captures[] = {"01-before.rdc", "02-after.rdc"};
    unsigned count = fc.job.count;
    const unsigned pw = 320, ph = fc.h * pw / fc.w, height = ph + 8;
    uint8_t *preview = calloc(pw * count * height, 4);
    uint64_t bytes = 0;
    uint64_t required = 32ull << 20;
    for (unsigned i=0; i<count; i++) required += fc.frames[slots[i]].rdc_bytes;
    struct statvfs space;
    if (!statvfs(fc.status.directory, &space) && (uint64_t)space.f_bavail * space.f_frsize < required) ok = false;
    for (unsigned i = 0; i < count && ok; i++) {
        uint8_t *pixels = fc.frames[slots[i]].pixels;
        snprintf(path, sizeof path, "%s/%s", dir, count == 1 ? "frame.rdc" : captures[i]);
        ok = save_rdc(path, &fc.frames[slots[i]], &bytes);
        snprintf(path, sizeof path, "%s/%s", dir, count == 1 ? "frame.png" : names[i]);
        if (ok) ok = save_png(path, pixels, fc.w, fc.h, &bytes);
        if (preview) for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < pw; x++) {
            uint8_t *dst = preview + ((y * pw * count) + i * pw + x) * 4;
            if (y < 8) { dst[0] = i == 1 ? 255 : 40; dst[1] = i == 0 ? 220 : 60; dst[2] = i == 2 ? 255 : 40; dst[3] = 255; }
            else memcpy(dst, pixels + (((y-8) * fc.h / ph) * fc.w + x * fc.w / pw) * 4, 4);
        }
    }
    if (ok && preview) {
        snprintf(path, sizeof path, "%s/preview.png", dir);
        ok = save_png(path, preview, pw * count, height, &bytes);
    } else ok = false;
    free(preview);
    const char *reason = count == 1 ? "manual" : "threshold";
    snprintf(path, sizeof path, "%s/event.json", dir);
    FILE *meta = ok ? fopen(path, "w") : NULL;
    if (!meta) ok = false;
    if (meta) {
        fprintf(meta, "{\"format\":\"renderdoc-rdc\",\"reason\":\"%s\",\"score\":%.6f,\"threshold\":%.6f,\"width\":%u,\"height\":%u,\"frames\":[",
                reason, fc.job.strength, fc.job.threshold, fc.w, fc.h);
        for (unsigned i = 0; i < count; i++) {
            FlickerFrame *s = &fc.frames[slots[i]];
            fprintf(meta, "%s{\"file\":\"%s\",\"capture\":\"%s\",\"rdc_bytes\":%llu,\"frame\":%llu,\"monotonic_ns\":%llu}", i ? "," : "",
                    count == 1 ? "frame.png" : names[i], count == 1 ? "frame.rdc" : captures[i], (unsigned long long)s->rdc_bytes,
                    (unsigned long long)s->frame, (unsigned long long)s->ns);
        }
        fputs("]}\n", meta); ok = fclose(meta) == 0;
    }
    if (ok) ok = rename(dir, final) == 0;
    if (ok) {
        snprintf(path, sizeof path, "%s/events.tsv", fc.status.directory);
        FILE *index = fopen(path, "a");
        if (index) {
            fprintf(index, "event-%04u\t%llu\t%llu\t%.6f\t%s\n", fc.job.event,
                    (unsigned long long)fc.frames[slots[0]].frame, (unsigned long long)fc.frames[slots[count-1]].frame, fc.job.strength, reason);
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
        LOG("[flicker] saved event-%04u frames %llu/%llu score %.4f -> %s\n", fc.job.event,
            (unsigned long long)fc.frames[fc.job.before].frame,
            (unsigned long long)fc.frames[fc.job.after].frame, fc.job.strength, fc.status.directory);
    } else { snprintf(fc.status.error, sizeof fc.status.error, "Could not save capture"); fc.status.active = false; }
    for (int i = 0; i < 2; i++) free_frame(i);
    fc.previous = -1; // PNG writing may span several game frames; never claim those are consecutive.
    if (fc.status.saved >= 64 || fc.bytes >= (8ull << 30)) {
        snprintf(fc.status.error, sizeof fc.status.error, "Capture limit reached"); fc.status.active = false;
    }
    report();
}

FlickerStatus flicker_status(void) { writer_finish(); fc.status.available = rd_capture_available(); return fc.status; }
void flicker_set_threshold(float value) { fc.status.threshold = fminf(0.4f, fmaxf(0.02f, value)); }

static void release_gpu(void) {
    for (int i = 0; i < 2; i++) {
        if (fc.frames[i].buffer) vmaDestroyBuffer(g_vk.vma, fc.frames[i].buffer, fc.frames[i].memory);
        free_frame(i);
        fc.frames[i] = (FlickerFrame){.rdc=-1};
    }
    if (fc.small) vmaDestroyImage(g_vk.vma, fc.small, fc.memory);
    fc.small = VK_NULL_HANDLE; fc.memory = NULL; fc.initialized = false;
}

void flicker_reset(void) {
    if (fc.capturing >= 0) { rd_capture_discard(g_vk.inst); fc.capturing = -1; }
    if (fc.writer_live) {
        while (!atomic_load_explicit(&fc.done, memory_order_acquire)) SDL_Delay(1);
        writer_finish();
    }
    fc.status.active = fc.status.single_pending = false; report();
    release_gpu(); fc.previous = -1;
}

void flicker_start(void) {
    writer_finish();
    if (fc.status.active || fc.writer_live) return;
    if (!rd_capture_available()) { snprintf(fc.status.error, sizeof fc.status.error, "GPU capture library is unavailable"); return; }
    if (fc.capturing >= 0) return;
    float threshold = fc.status.threshold;
    fc.status = (FlickerStatus){.threshold = threshold}; fc.bytes = 0; fc.previous = -1;
    for (int i = 0; i < 2; i++) free_frame(i);
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
void flicker_single(void) {
    writer_finish();
    if (fc.writer_live || fc.status.single_pending) return;
    if (!fc.status.active) {
        flicker_start();
        if (!fc.status.active) return;
        fc.status.active = false;
    }
    fc.status.single_pending = true;
}

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
    for (int i = 0; i < 2; i++) {
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

static void capture_fail(const char *message) {
    if (fc.capturing >= 0) rd_capture_discard(g_vk.inst);
    fc.capturing = -1;
    fc.status.active = fc.status.single_pending = false;
    snprintf(fc.status.error, sizeof fc.status.error, "%s", message);
    for (int i=0;i<2;i++) free_frame(i);
    fc.previous = -1; report();
}

static void save_selected(int before, int after, unsigned count, float strength) {
    fc.job.before=before; fc.job.after=after; fc.job.count=count;
    fc.job.strength=strength; fc.job.threshold=fc.status.threshold; fc.job.event=fc.status.saved+1;
    fc.job.ok=false; fc.job.bytes=0;
    atomic_store_explicit(&fc.done, false, memory_order_relaxed);
    fc.writer_live = pthread_create(&fc.writer, NULL, save_event, NULL) == 0;
    fc.status.saving = fc.writer_live;
    if (!fc.writer_live) capture_fail("Could not start capture writer");
}

// Begin only at a complete frame boundary, after the previous present/commands
// have been submitted. All commands generating the next game image are captured.
void flicker_next_frame(bool visible) {
    writer_finish();
    if (fc.capturing >= 0 || fc.writer_live) return;
    if (!visible || (!fc.status.active && !fc.status.single_pending)) {
        for (int i=0;i<2;i++) free_frame(i);
        fc.previous = -1;
        if (!fc.status.active && !fc.status.single_pending && fc.initialized) release_gpu();
        return;
    }
    int index = fc.previous == 0 ? 1 : 0;
    free_frame(index);
    fc.capture_single = fc.status.single_pending;
    fc.frames[index].rdc = rd_capture_begin(g_vk.inst, fc.status.error, sizeof fc.status.error);
    if (fc.frames[index].rdc < 0) {
        fc.status.active=fc.status.single_pending=false; report(); return;
    }
    fc.capturing = index;
}

// Complete the same frame whose game image supplies this preview. RenderDoc
// already synchronises capture; waiting for its readback avoids mismatching RDC
// resources and an earlier/later asynchronously completed screenshot.
void vk_flicker_frame(TexBackend *source, uint64_t frame, bool visible) {
    writer_finish();
    if (fc.capturing < 0) { if (fc.status.active) fc.status.skipped++; return; }
    if (!visible || !source || (!fc.status.active && !fc.capture_single)) {
        rd_capture_discard(g_vk.inst); free_frame(fc.capturing); fc.capturing=-1;
        if (fc.previous>=0) free_frame(fc.previous);
        fc.previous=-1; return;
    }
    VkFormatProperties src, dst;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, source->format, &src);
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_R8G8B8A8_UNORM, &dst);
    if (!(src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || source->samples != VK_SAMPLE_COUNT_1_BIT) {
        capture_fail("Capture format is unsupported"); return;
    }
    if (!fc.initialized && !allocate_gpu(source)) {
        capture_fail("Could not allocate capture memory"); release_gpu(); return;
    }
    int index=fc.capturing;
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
    fc.frames[index].ns=SDL_GetTicksNS();
    extern void renderer_submit(void);
    renderer_submit();
    vk_wait_seq(fc.frames[index].seq);
    vmaInvalidateAllocation(g_vk.vma, fc.frames[index].memory, 0, VK_WHOLE_SIZE);
    bool ok = rd_capture_end(g_vk.inst);
    fc.capturing=-1;
    struct stat st;
    if (!ok || fstat(fc.frames[index].rdc, &st) || st.st_size < 100) {
        capture_fail("Could not finish GPU capture"); return;
    }
    fc.frames[index].rdc_bytes=st.st_size;
    fc.frames[index].state=HELD;
    // Bounded retained captures; RenderDoc's transient resource snapshots are additional.
    uint64_t retained=fc.frames[0].rdc_bytes+fc.frames[1].rdc_bytes;
    if (retained > (2ull<<30)) { capture_fail("GPU capture memory limit reached"); return; }
    if (fc.capture_single) {
        fc.status.single_pending=false;
        if (fc.previous>=0) free_frame(fc.previous);
        fc.previous=-1;
        save_selected(index,index,1,0); return;
    }
    if (fc.previous>=0) {
        FlickerFrame *before=&fc.frames[fc.previous];
        if (before->frame+1 == frame) {
            float change=flicker_difference(before->pixels,fc.frames[index].pixels,fc.w,fc.h);
            if (change >= fc.status.threshold) { save_selected(fc.previous,index,2,change); return; }
        }
        free_frame(fc.previous);
    }
    fc.previous=index;
}
