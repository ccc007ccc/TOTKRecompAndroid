// Exercise the real parameter parser and mixer with independent wire packets
// and known samples. No game data or audio device is needed.
#define _GNU_SOURCE
#define __ANDROID__ 1
#include <assert.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "../runtime/services/audio.c"

uint64_t g_guest_lo;
Settings g_settings;
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
void plat_write_prepare(uint64_t a, uint64_t n) { assert(in_guest(a, n)); }
void rt_fatal_trace(void) { abort(); }
void rt_fatal_message(const char *format, ...) {
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); abort();
}

static void u32(uint8_t *p, uint32_t value) { memcpy(p, &value, 4); }
static void u64(uint8_t *p, uint64_t value) { memcpy(p, &value, 8); }
static void f32(uint8_t *p, float value) { memcpy(p, &value, 4); }
static void check_near(float got, float want, unsigned line) {
    if (!(fabsf(got - want) < 0.00002f)) {
        fprintf(stderr,"line %u: got %.9g, expected %.9g\n",line,got,want);
        abort();
    }
}
#define near(got, want) check_near((got), (want), __LINE__)

static Renderer make_renderer(unsigned samples, unsigned voices, unsigned mixes) {
    Renderer r = {.sample_rate=48000, .sample_count=samples, .voices=voices, .nmix=mixes,
        .splitters=4, .splitter_destinations=8, .revision=0x3f564552, .effects=8, .routing_dirty=true};
    r.voice=calloc(voices,sizeof *r.voice); r.chres=calloc(voices,sizeof *r.chres); r.chst=calloc(voices,sizeof *r.chst);
    r.mix=calloc(mixes,sizeof *r.mix); r.fx=calloc(r.effects,sizeof *r.fx);
    r.splitter=calloc(r.splitters,sizeof *r.splitter); r.destination=calloc(r.splitter_destinations,sizeof *r.destination);
    r.mix_order=calloc(mixes,sizeof *r.mix_order); r.mix_pending=calloc(mixes,sizeof *r.mix_pending);
    for (unsigned i=0;i<mixes;i++) {
        r.mix[i]=(Mix){.used=1,.volume=1,.bufs=2,.dest=i?0:UNUSED_MIX,.dest_splitter=UNUSED_SPLITTER};
        r.mix[i].buf=calloc(MIXBUF_MAX*samples,sizeof(float));
        r.mix[i].vol[0]=r.mix[i].vol[25]=1;
    }
    r.sink_used=1; r.sink_inputs=2; r.sink_input[0]=0; r.sink_input[1]=1;
    return r;
}

static void free_renderer(Renderer *r) {
    for (unsigned i=0;i<r->nmix;i++) free(r->mix[i].buf);
    for (unsigned i=0;i<r->splitters;i++) free(r->splitter[i].destinations);
    for (unsigned i=0;i<r->effects;i++) effect_free(&r->fx[i]);
    free(r->voice); free(r->chres); free(r->chst); free(r->mix); free(r->fx);
    free(r->splitter); free(r->destination); free(r->mix_order); free(r->mix_pending);
}

static void pcm_voice(Renderer *r, unsigned channels, int mix, unsigned split, unsigned first, float volume) {
    uint8_t p[0x188]={0}, out[16]={0};
    p[8]=p[9]=1; p[11]=2;
    u32(p+12,48000); u32(p+24,channels); f32(p+28,1); f32(p+32,volume);
    u32(p+84,1); u32(p+88,first); u32(p+112,mix); u32(p+116,split);
    for (unsigned i=0;i<4;i++) p[120+i*56+26]=1;
    uint8_t *wb=p+120+first*56;
    u64(wb,g_guest_lo+0x1000); u64(wb+8,512*channels*2); u32(wb+20,512); wb[26]=0;
    int16_t *samples=(int16_t *)(uintptr_t)(g_guest_lo+0x1000);
    for (unsigned i=0;i<512;i++) for (unsigned c=0;c<channels;c++) samples[i*channels+c]=c?-6000:12000;
    for (unsigned c=0;c<channels;c++) u32(p+344+c*4,c);
    update_voice(r,&r->voice[0],p,out,true);
}

static size_t split_state(uint8_t *packet, unsigned id, const unsigned *destinations, unsigned count) {
    memset(packet,0,4096); u32(packet,0x48444e53); u32(packet+4,1); u32(packet+8,count);
    u32(packet+32,0x49444e53); u32(packet+36,id); u32(packet+40,48000); u32(packet+44,count);
    for (unsigned i=0;i<count;i++) u32(packet+48+4*i,destinations[i]);
    return 32+28+count*4;
}

static void destination(uint8_t *p, unsigned id, unsigned mix, unsigned stride, float left, float right, bool reset) {
    memset(p,0,stride); u32(p,0x44444e53); u32(p+4,id); u32(p+104,mix);
    f32(p+8,left); f32(p+12,right);
    unsigned flags=stride==168?156:stride==144?132:108;
    p[flags]=1; p[flags+1]=reset;
}

static void splitter_stereo_and_updates(void) {
    Renderer r=make_renderer(8,2,3);
    uint8_t packet[4096]; unsigned ids[]={2,0,3,1};
    size_t at=split_state(packet,0,ids,4);
    destination(packet+at,0,2,168,0,.75f,true);
    destination(packet+at+168,1,1,168,0,.1f,true);
    destination(packet+at+336,2,2,168,.5f,0,true);
    destination(packet+at+504,3,1,168,.25f,0,true);
    assert(update_splitters(&r,packet,at+672));
    pcm_voice(&r,2,UNUSED_MIX,0,2,.8f); // the first queued buffer is slot 2, not 0
    float out[16]; render_frame(&r,out);
    for (unsigned i=0;i<8;i++) {
        near(out[i*2],i<3?0:12000*.8f*.75f/32768);
        near(out[i*2+1],i<3?0:-6000*.8f*.85f/32768);
    }
    assert(r.chst[0].wb_index==2 && r.chst[0].played==8);
    assert(r.split_channels==2 && r.direct_channels==0);
    // Invalid/truncated updates cannot partially overwrite existing connections.
    u32(packet+48,7);
    assert(!update_splitters(&r,packet,at+671));
    assert(r.splitter[0].destinations[0]==2);
    uint8_t sparse[32+168]={0}; u32(sparse,0x48444e53); u32(sparse+8,1);
    destination(sparse+32,2,2,168,0,0,false);
    assert(update_splitters(&r,sparse,sizeof sparse));
    render_frame(&r,out);
    for (unsigned i=0;i<8;i++) near(out[2*i],12000*.8f*(.25f+.5f*(1-i/8.f))/32768);
    u32(sparse+8,0);
    assert(update_splitters(&r,sparse,32)); // a zero-entry delta must not erase routing
    render_frame(&r,out);
    near(out[6],12000*.8f*.25f/32768);
    free_renderer(&r);
}

static void splitter_versions_and_filters(void) {
    for (unsigned rev=11;rev<=15;rev++) {
        Renderer r=make_renderer(8,1,1); r.revision=0x00564552|((rev+'0')<<24);
        uint8_t packet[4096]; unsigned id=0;
        size_t at=split_state(packet,0,&id,1), stride=rev>=15?168:rev>=12?144:112;
        destination(packet+at,0,0,stride,1,1,true);
        if (rev>=12) {
            packet[at+108]=1;
            if (rev>=15) { f32(packet+at+112,.5f); f32(packet+at+124,.25f); }
            else { int16_t a=8192,b=4096; memcpy(packet+at+110,&a,2); memcpy(packet+at+116,&b,2); }
        }
        assert(update_splitters(&r,packet,at+stride));
        pcm_voice(&r,1,UNUSED_MIX,0,0,1);
        float out[16], previous=0; render_frame(&r,out);
        for (unsigned i=0;i<8;i++) {
            float x=i<3?0:12000, y=rev>=12?.5f*x+.25f*previous:x;
            near(out[i*2],y/32768); near(out[i*2+1],y/32768); previous=y;
        }
        free_renderer(&r);
    }
}

static void submix_split_order(void) {
    Renderer r=make_renderer(8,2,4); r.mix[1].dest=UNUSED_MIX; r.mix[1].dest_splitter=0; r.mix[1].volume=.5f;
    uint8_t packet[4096]; unsigned ids[]={0,1}; size_t at=split_state(packet,0,ids,2);
    destination(packet+at,0,2,168,1,0,true); destination(packet+at+168,1,3,168,0,1,true);
    assert(update_splitters(&r,packet,at+336));
    pcm_voice(&r,2,1,UNUSED_SPLITTER,0,1);
    r.chres[0].mix[0]=r.chres[0].prev[0]=1; r.chres[1].mix[1]=r.chres[1].prev[1]=1;
    float out[16]; render_frame(&r,out);
    for (unsigned i=3;i<8;i++) { near(out[2*i],6000.f/32768); near(out[2*i+1],-3000.f/32768); }
    unsigned pos[4]={0}; for(unsigned i=0;i<4;i++)pos[r.mix_order[i]]=i;
    assert(pos[1]<pos[2] && pos[1]<pos[3] && pos[2]<pos[0] && pos[3]<pos[0]);
    free_renderer(&r);
}

static void auxiliary_ring(void) {
    Renderer r=make_renderer(4,1,1); Mix *m=&r.mix[0];
    AuxiliaryParam p={.channels=2,.capacity=13,.send_info=g_guest_lo+0x10000,.return_info=g_guest_lo+0x11000};
    p.input[0]=0; p.input[1]=1; p.output[0]=1; p.output[1]=0;
    uint32_t *send=(uint32_t *)(uintptr_t)p.send_info,*ret=(uint32_t *)(uintptr_t)p.return_info;
    memset(send,0xa5,256); memset(ret,0x5a,256);
    send[17]=11; ret[16]=10;
    for(unsigned i=0;i<8;i++) { m->buf[i]=i+10.75f; ret[32+(10+i)%13]=100+i; }
    auxiliary_process(&p,m,4,true);
    for(unsigned i=0;i<8;i++) assert(send[32+(11+i)%13]==i+10);
    for(unsigned i=0;i<4;i++) { near(m->buf[4+i],100+i);near(m->buf[i],104+i); }
    assert(send[17]==6 && ret[16]==5 && send[0]==0xa5a5a5a5 && ret[0]==0x5a5a5a5a);
    assert(send[18]==0xa5a5a5a5 && ret[18]==0x5a5a5a5a);
    auxiliary_process(&p,m,4,false);
    for(unsigned i=0;i<4;i++) { near(m->buf[i],100+i);near(m->buf[4+i],104+i); }
    assert(send[16]==0 && send[17]==0 && send[19]==0 && ret[16]==0 && ret[17]==0 && ret[19]==0);
    assert(send[18]==0xa5a5a5a5 && ret[18]==0x5a5a5a5a);
    free_renderer(&r);
}

static void adpcm_resampling_and_loop(void) {
    Renderer r=make_renderer(240,1,1);
    uint8_t packet[4096];unsigned id=0;size_t at=split_state(packet,0,&id,1);
    destination(packet+at,0,0,168,1,1,true);assert(update_splitters(&r,packet,at+168));
    pcm_voice(&r,1,UNUSED_MIX,0,0,1);
    Voice *v=&r.voice[0];v->fmt=6;v->rate=16000;v->ds_addr=g_guest_lo+0x1800;
    memset((void *)(uintptr_t)v->ds_addr,0,32);
    uint8_t *data=(uint8_t *)(uintptr_t)(g_guest_lo+0x2000);data[0]=9;memset(data+1,0x11,7);
    v->wb[0]=(WaveBuf){.addr=(uintptr_t)data,.size=8,.end=14,.loop=1,.loop_count=-1};
    float out[480];render_frame(&r,out);
    for(unsigned i=30;i<240;i++) { near(out[2*i],512.f/32768);near(out[2*i+1],512.f/32768); }
    assert(r.chst[0].played==80);
    // A loop range starting at zero is valid and differs from the initial range.
    data[0]=0;for(unsigned i=0;i<7;i++)data[1+i]=(uint8_t)(((i+1)<<4)|(i+1));
    v->wb[0].start=3;v->wb[0].end=14;v->wb[0].loop_start=0;v->wb[0].loop_end=7;
    ChanState state={.valid={1,0,0,0}};int16_t samples[32];assert(decode_voice(v,0,&state,samples,32)==32);
    for(unsigned i=0;i<32;i++) {
        unsigned index=i<11?i+3:(i-11)%7;
        assert(samples[i]==(int16_t)(index/2+1));
    }
    free_renderer(&r);
}

static void limiter_peak_and_delay(void) {
    Renderer r=make_renderer(16,1,1); Limiter l={0};
    LimiterParam p={.channels_max=2,.channels=2,.sample_rate=48000,.attack=1,.release=.01f,
        .threshold=.5f,.input_gain=1,.output_gain=1,.delay_min=2,.delay_max=4};
    p.input[0]=0; p.input[1]=1; p.output[0]=1; p.output[1]=0;
    limiter_update(&l,(uint8_t *)&p);
    for(unsigned i=0;i<16;i++) { r.mix[0].buf[i]=65534; r.mix[0].buf[16+i]=-32767; }
    limiter_process(&l,&r.mix[0],16,true);
    for(unsigned i=0;i<16;i++) {
        near(r.mix[0].buf[16+i],i<2?0:16383.5f);
        near(r.mix[0].buf[i],i<2?0:-16383.5f);
    }
    near(l.input_max[0],2);near(l.gain_min[0],.25f);near(l.gain_min[1],.5f);
    free(l.delay); free_renderer(&r);
}

static void reverb_tail(void) {
    Reverb3dDelay delay={0};r3_delay_init(&delay,3);
    for(unsigned i=0;i<12;i++)near(r3_delay_push(&delay,i==0?1:0),i==3?1:0);
    free(delay.data);
    for(unsigned ch=1;ch<=6;ch++) {
        if(ch==3 || ch==5)continue;
        Renderer r=make_renderer(240,1,1);r.mix[0].bufs=6;
        Reverb3d rv={0}; Reverb3dParam p={.channels_max=6,.channels=ch,.sample_rate=48000,.room_hf=-3000,
            .hf_reference=4000,.decay_time=1.2f,.hf_decay_ratio=.7f,.room_gain=-500,.reflections_gain=-300,
            .reverb_gain=-300,.diffusion=60,.reflection_delay=.01f,.reverb_delay=.02f,.density=70,.dry_gain=0};
        for(unsigned c=0;c<ch;c++)p.input[c]=p.output[c]=c;
        reverb3d_update(&rv,(uint8_t *)&p);
        double wet=0,late=0;
        for(unsigned f=0;f<120;f++) {
            memset(r.mix[0].buf,0,240*24*sizeof(float));if(f==0)r.mix[0].buf[0]=12000;
            reverb3d_process(&rv,&r.mix[0],240,true,15);
            for(unsigned i=0;i<240*ch;i++) {
                float sample=r.mix[0].buf[i];assert(isfinite(sample) && fabsf(sample)<100000);
                wet+=(double)sample*sample;if(f>40)late+=(double)sample*sample;
            }
        }
        assert(wet>1 && late>1);
        for(unsigned c=0;c<ch;c++)for(unsigned i=0;i<240;i++)r.mix[0].buf[c*240+i]=100+c;
        reverb3d_process(&rv,&r.mix[0],240,false,15);
        for(unsigned c=0;c<ch;c++)near(r.mix[0].buf[c*240],100+c);
        reverb3d_free(&rv);free_renderer(&r);
    }
}

static void captured_update(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    uint8_t *input=malloc(size),output[0x10000];assert(fread(input,1,size,f)==(size_t)size);fclose(f);
    Renderer r=make_renderer(240,96,12);
    free(r.fx);r.effects=16;r.fx=calloc(16,sizeof *r.fx);
    IpcReq req={.na=1,.nb=1};IpcResp resp={0};
    req.a[0]=(IpcBuf){(uintptr_t)input,size};req.b[0]=(IpcBuf){(uintptr_t)output,sizeof output};
    request_update(&r,&req,&resp);
    assert(!resp.result && audio_u32(output)==0x3f564552);
    assert(r.fx[0].type==5 && r.fx[0].rv3d.p.sample_rate==48000);
    assert(r.fx[1].type==2 && r.fx[1].aux.channels==6 && r.fx[1].aux.capacity==7200);
    assert(r.fx[4].type==7 && r.fx[4].limiter.p.delay_min==4 && r.fx[4].limiter.p.delay_max==9);
    free(input);free_renderer(&r);
}

int main(int argc,char **argv) {
    void *guest=mmap(NULL,0x20000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guest!=MAP_FAILED);
    g_guest_lo=(uintptr_t)guest;g_settings.reverb=1;
    splitter_stereo_and_updates();splitter_versions_and_filters();submix_split_order();
    auxiliary_ring();adpcm_resampling_and_loop();limiter_peak_and_delay();reverb_tail();
    if(argc>1)captured_update(argv[1]);
    munmap(guest,0x20000);
    puts("audio mixer tests passed: REV11-15 splitters, sparse updates, channel fanout/filters, mix graph, wave ring, auxiliary rings, limiter and 3D reverb");
}
