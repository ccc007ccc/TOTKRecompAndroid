#include "diagnostic.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

enum { IMAGE_BYTES=24<<20, BLOB_BYTES=8<<20, LOG_BYTES=4<<20,
       MAX_IMAGES=512, MAX_BLOBS=8192, THUMB_SIDE=128 };
typedef struct {
    uint64_t address, offset, bytes;
    unsigned group, slot, level, layer, source_w, source_h, w, h, format, stored_format, aspect, status;
} DiagImage;
typedef struct {
    uint64_t address, original_bytes, offset, hash;
    unsigned kind, bytes;
} DiagBlob;
struct VkDiagnostic {
    VkBuffer buffer;
    VmaAllocation memory;
    uint8_t *pixels, *blobs;
    char *log;
    uint64_t used, seq;
    unsigned blob_used, log_used, nimages, nblobs, group, image_skipped, blob_skipped, log_skipped;
    bool dirty, gpu_failed;
    DiagImage images[MAX_IMAGES];
    DiagBlob entries[MAX_BLOBS];
    struct { VkFormat format; VkImage image; VmaAllocation memory; VkImageAspectFlags aspect; } scratch[5];
};
static VkDiagnostic *current;

VkDiagnostic *vk_diag_create(void) {
    VkDiagnostic *d=calloc(1,sizeof *d);
    if(!d)return NULL;
    d->blobs=malloc(BLOB_BYTES);d->log=malloc(LOG_BYTES);
    if(!d->blobs||!d->log){vk_diag_destroy(d);return NULL;}
    return d;
}
void vk_diag_destroy(VkDiagnostic *d) {
    if(!d)return;
    if(current==d)current=NULL;
    if(d->buffer)vmaDestroyBuffer(g_vk.vma,d->buffer,d->memory);
    for(unsigned i=0;i<5;i++)if(d->scratch[i].image)
        vmaDestroyImage(g_vk.vma,d->scratch[i].image,d->scratch[i].memory);
    free(d->blobs);free(d->log);free(d);
}
void vk_diag_begin(VkDiagnostic *d) {
    current=d;
    d->used=d->blob_used=d->log_used=d->nimages=d->nblobs=d->group=0;
    d->seq=0;
    d->image_skipped=d->blob_skipped=d->log_skipped=0;
    d->dirty=false;d->gpu_failed=false;
}
void vk_diag_end(VkDiagnostic *d) {
    current=NULL;
    if(d->buffer && d->used)vmaInvalidateAllocation(g_vk.vma,d->memory,0,d->used);
}
void vk_diag_cancel(void) { current=NULL; }
bool vk_diag_active(void) { return current!=NULL; }
bool vk_diag_dirty(void) { return current&&current->dirty; }
void vk_diag_mark_dirty(void) { if(current)current->dirty=true; }
unsigned vk_diag_group(void) { return current?current->group:0; }
void vk_diag_next_group(void) { if(current){current->group++;current->dirty=false;} }

void vk_diag_log(const char *format, ...) {
    if(!current)return;
    char line[4096];va_list args;va_start(args,format);
    int n=vsnprintf(line,sizeof line,format,args);va_end(args);
    // Preserve room for later draw/skip/clear summaries when verbose bindings
    // fill the detailed log. A late UI failure must not disappear behind terrain.
    if(n<0 || n>=(int)sizeof line || (unsigned)n>LOG_BYTES-current->log_used ||
       (format[0]==' ' && LOG_BYTES-current->log_used<(2u<<20))){current->log_skipped++;return;}
    memcpy(current->log+current->log_used,line,n);current->log_used+=n;
}
unsigned vk_diag_blob(unsigned kind,uint64_t address,const void *data,uint64_t size,unsigned limit) {
    VkDiagnostic *d=current;if(!d||!data||!size)return 0;
    unsigned bytes=size<limit?(unsigned)size:limit;
    if(bytes>BLOB_BYTES)bytes=BLOB_BYTES;
    uint64_t hash=0xcbf29ce484222325ull;
    const uint8_t *src=data;
    unsigned i=0;
    for(;i+8<=bytes;i+=8){uint64_t v;memcpy(&v,src+i,8);hash=(hash^v)*0x100000001b3ull;}
    for(;i<bytes;i++)hash=(hash^src[i])*0x100000001b3ull;
    for(i=0;i<d->nblobs;i++){
        DiagBlob *b=&d->entries[i];
        if(b->kind==kind&&b->address==address&&b->original_bytes==size&&b->bytes==bytes&&b->hash==hash&&
           !memcmp(d->blobs+b->offset,data,bytes))return i+1;
    }
    if(d->nblobs==MAX_BLOBS||bytes>BLOB_BYTES-d->blob_used){d->blob_skipped++;return 0;}
    d->entries[d->nblobs]=(DiagBlob){address,size,d->blob_used,hash,kind,bytes};
    memcpy(d->blobs+d->blob_used,data,bytes);d->blob_used+=bytes;
    return ++d->nblobs;
}

bool vk_diag_float_color(VkFormat f) {
    // Feature bits alone do not make integer -> float blits legal. Keep an
    // explicit list of the backend's normalized/float attachment formats;
    // unsupported and future formats retain metadata without GPU commands.
    switch(f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return true;
    default: return false;
    }
}

static unsigned texel_bytes(VkFormat f,VkImageAspectFlags aspect) {
    if(aspect==VK_IMAGE_ASPECT_COLOR_BIT)return 8;
    if(aspect==VK_IMAGE_ASPECT_STENCIL_BIT)return 1;
    if(f==VK_FORMAT_D16_UNORM)return 2;
    if(f==VK_FORMAT_D32_SFLOAT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||f==VK_FORMAT_D24_UNORM_S8_UINT)return 4;
    return 0;
}
void vk_diag_image(TexBackend *src,uint64_t address,unsigned slot,unsigned level,unsigned layer,VkImageAspectFlags aspect) {
    VkDiagnostic *d=current;if(!d||!src)return;
    if(d->nimages==MAX_IMAGES){d->image_skipped++;return;}
    DiagImage *e=&d->images[d->nimages++];memset(e,0,sizeof *e);
    *e=(DiagImage){.address=address,.group=d->group,.slot=slot,.level=level,.layer=layer,
        .format=src->format,.aspect=aspect,.status=1};
    if(level>=src->levels||layer>=src->layers||src->samples!=VK_SAMPLE_COUNT_1_BIT||src->depth!=1)goto skipped;
    e->source_w=src->width>>level;e->source_h=src->height>>level;
    if(!e->source_w)e->source_w=1;if(!e->source_h)e->source_h=1;
    unsigned side=e->source_w>e->source_h?e->source_w:e->source_h;
    e->w=side>THUMB_SIDE?(uint64_t)e->source_w*THUMB_SIDE/side:e->source_w;
    e->h=side>THUMB_SIDE?(uint64_t)e->source_h*THUMB_SIDE/side:e->source_h;
    if(!e->w)e->w=1;if(!e->h)e->h=1;
    if(aspect==VK_IMAGE_ASPECT_COLOR_BIT&&!vk_diag_float_color(src->format))goto skipped;
    VkFormat dst=aspect==VK_IMAGE_ASPECT_COLOR_BIT?VK_FORMAT_R16G16B16A16_SFLOAT:src->format;
    e->stored_format=dst;
    unsigned bpp=texel_bytes(dst,aspect);if(!bpp)goto skipped;
    VkFormatProperties from,to;vkGetPhysicalDeviceFormatProperties(g_vk.phys,src->format,&from);
    vkGetPhysicalDeviceFormatProperties(g_vk.phys,dst,&to);
    if(!(from.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(to.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))goto skipped;
    uint64_t offset=(d->used+255)&~255ull,size=(uint64_t)e->w*e->h*bpp;
    if(offset>IMAGE_BYTES||size>IMAGE_BYTES-offset){e->status=2;goto skipped;}
    if(d->gpu_failed){e->status=3;goto skipped;}
    if(!d->buffer){
        VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=IMAGE_BYTES,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO,.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if(vmaCreateBuffer(g_vk.vma,&bi,&ai,&d->buffer,&d->memory,&info)!=VK_SUCCESS){d->gpu_failed=true;e->status=3;goto skipped;}
        d->pixels=info.pMappedData;
    }
    unsigned s=0;for(;s<5;s++)if(!d->scratch[s].image||d->scratch[s].format==dst)break;
    if(s==5)goto skipped;
    if(!d->scratch[s].image){
        VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=dst,
            .extent={THUMB_SIDE,THUMB_SIDE,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
            .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai={.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if(vmaCreateImage(g_vk.vma,&ii,&ai,&d->scratch[s].image,&d->scratch[s].memory,NULL)!=VK_SUCCESS){e->status=3;goto skipped;}
        d->scratch[s].format=dst;d->scratch[s].aspect=aspect==VK_IMAGE_ASPECT_COLOR_BIT?aspect:src->aspect;
    }
    VkCommandBuffer cb=vk_cmd();vk_full_barrier(cb);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_GENERAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=d->scratch[s].image,.subresourceRange={d->scratch[s].aspect,0,1,0,1}};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkImageBlit blit={.srcSubresource={aspect,level,layer,1},.srcOffsets={{0,0,0},{e->source_w,e->source_h,1}},
        .dstSubresource={aspect,0,0,1},.dstOffsets={{0,0,0},{e->w,e->h,1}}};
    vkCmdBlitImage(cb,src->image,VK_IMAGE_LAYOUT_GENERAL,d->scratch[s].image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);
    vk_full_barrier(cb);
    VkBufferImageCopy copy={.bufferOffset=offset,.imageSubresource={aspect,0,0,1},.imageExtent={e->w,e->h,1}};
    vkCmdCopyImageToBuffer(cb,d->scratch[s].image,VK_IMAGE_LAYOUT_GENERAL,d->buffer,1,&copy);
    vk_full_barrier(cb); // finish reads before later draws/copies overwrite either image
    e->offset=offset;e->bytes=size;e->status=0;d->used=offset+size;d->seq=vk_pending_seq();return;
skipped:
    d->image_skipped++;
}

static bool compressed(const char *path,const void *data,unsigned length,uint64_t *bytes) {
    gzFile f=gzopen(path,"wb1");if(!f)return false;
    bool ok=!length||gzwrite(f,data,length)==(int)length;
    if(gzclose(f)!=Z_OK)ok=false;
    struct stat st;if(!stat(path,&st))*bytes+=st.st_size;
    return ok;
}
uint64_t vk_diag_raw_bytes(const VkDiagnostic *d) { return d?d->used+d->blob_used+d->log_used+65536:0; }
uint64_t vk_diag_pending_seq(const VkDiagnostic *d) { return d?d->seq:0; }
bool vk_diag_write(VkDiagnostic *d,const char *dir,uint64_t *bytes) {
    if(!d||mkdir(dir,0755))return false;
    char path[1280];
    snprintf(path,sizeof path,"%s/stages.bin.gz",dir);
    bool ok=compressed(path,d->pixels,(unsigned)d->used,bytes);
    snprintf(path,sizeof path,"%s/inputs.bin.gz",dir);
    ok=compressed(path,d->blobs,d->blob_used,bytes)&&ok;
    snprintf(path,sizeof path,"%s/draws.txt.gz",dir);
    ok=compressed(path,d->log,d->log_used,bytes)&&ok;
    snprintf(path,sizeof path,"%s/diagnostic.json",dir);FILE *f=fopen(path,"w");if(!f)return false;
    fprintf(f,"{\"format\":\"light-diagnostic-v1\",\"replayable\":false,\"thumbnail_side\":%u,\"image_budget\":%u,\"input_budget\":%u,\"log_budget\":%u,\"groups\":%u,\"images_skipped\":%u,\"inputs_skipped\":%u,\"log_lines_skipped\":%u,\"images\":[",
        THUMB_SIDE,IMAGE_BYTES,BLOB_BYTES,LOG_BYTES,d->group,d->image_skipped,d->blob_skipped,d->log_skipped);
    const char *status[]={"ok","unsupported","budget","allocation"};
    for(unsigned i=0;i<d->nimages;i++){
        DiagImage *e=&d->images[i];
        fprintf(f,"%s{\"group\":%u,\"slot\":%u,\"address\":\"%llx\",\"level\":%u,\"layer\":%u,\"source_width\":%u,\"source_height\":%u,\"width\":%u,\"height\":%u,\"format\":%u,\"stored_format\":%u,\"aspect\":%u,\"offset\":%llu,\"bytes\":%llu,\"status\":\"%s\"}",
            i?",":"",e->group,e->slot,(unsigned long long)e->address,e->level,e->layer,e->source_w,e->source_h,e->w,e->h,e->format,e->stored_format,e->aspect,(unsigned long long)e->offset,(unsigned long long)e->bytes,status[e->status]);
    }
    fputs("],\"inputs\":[",f);
    for(unsigned i=0;i<d->nblobs;i++){
        DiagBlob *b=&d->entries[i];
        fprintf(f,"%s{\"id\":%u,\"kind\":%u,\"address\":\"%llx\",\"original_bytes\":%llu,\"offset\":%llu,\"bytes\":%u,\"hash\":\"%016llx\"}",
            i?",":"",i+1,b->kind,(unsigned long long)b->address,(unsigned long long)b->original_bytes,(unsigned long long)b->offset,b->bytes,(unsigned long long)b->hash);
    }
    fputs("]}\n",f);ok=fclose(f)==0&&ok;
    struct stat st;if(!stat(path,&st))*bytes+=st.st_size;
    return ok;
}
