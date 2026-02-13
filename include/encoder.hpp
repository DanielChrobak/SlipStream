#pragma once
#include "common.hpp"

struct EncodedFrame {
    std::vector<uint8_t> data;
    int64_t ts=0,encUs=0;
    bool isKey=false;
    void Clear() { data.clear(); ts=encUs=0; isKey=false; }
};

class VideoEncoder {
    AVCodecContext* cctx=nullptr;
    AVFrame* hwFr=nullptr;
    AVPacket* pkt=nullptr;
    AVBufferRef* hwDev=nullptr,*hwFrCtx=nullptr;
    ID3D11Device* dev=nullptr;
    ID3D11DeviceContext* ctx=nullptr;
    ID3D11Multithread* mt=nullptr;
    ID3D11Device5* d5=nullptr;
    ID3D11DeviceContext4* c4=nullptr;
    ID3D11Fence* fence=nullptr;
    HANDLE fEvt=nullptr;
    uint64_t fVal=0,lastSig=0;
    bool useFence=false;
    int w,h,frameNum=0,curFps=60;
    CodecType codec=CODEC_AV1;
    steady_clock::time_point lastKey;
    static constexpr auto KEY_INT=2000ms;
    EncodedFrame out;

    static int64_t CalcBR(int w,int h,int fps) { return (int64_t)(0.18085*w*h*fps); }
    static const char* EncName(CodecType c) { return c==CODEC_AV1?"av1_nvenc":c==CODEC_H265?"hevc_nvenc":"h264_nvenc"; }

    bool InitHwCtx(const AVCodec* c) {
        hwDev=av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if(!hwDev) return false;
        auto* dc=(AVD3D11VADeviceContext*)((AVHWDeviceContext*)hwDev->data)->hwctx;
        dc->device=dev; dc->device_context=ctx;
        if(av_hwdevice_ctx_init(hwDev)<0) { av_buffer_unref(&hwDev); return false; }
        cctx->hw_device_ctx=av_buffer_ref(hwDev);
        hwFrCtx=av_hwframe_ctx_alloc(hwDev);
        if(!hwFrCtx) { av_buffer_unref(&hwDev); return false; }
        auto* fc=(AVHWFramesContext*)hwFrCtx->data;
        fc->format=AV_PIX_FMT_D3D11; fc->sw_format=AV_PIX_FMT_BGRA;
        fc->width=w; fc->height=h; fc->initial_pool_size=4;
        if(av_hwframe_ctx_init(hwFrCtx)<0) { av_buffer_unref(&hwFrCtx); av_buffer_unref(&hwDev); return false; }
        cctx->hw_frames_ctx=av_buffer_ref(hwFrCtx);
        cctx->pix_fmt=AV_PIX_FMT_D3D11;
        return true;
    }

    void InitSync() {
        if(SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d5)))&&
           SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&c4)))&&
           SUCCEEDED(d5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) {
            fEvt=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(fEvt) { useFence=true; return; }
        }
        SafeRelease(d5,c4,fence);
    }

    uint64_t Signal() { if(useFence&&c4&&fence) { c4->Signal(fence,++fVal); lastSig=fVal; return fVal; } return 0; }

    bool WaitGPU(uint64_t v,DWORD ms=16) {
        if(useFence&&fence) {
            if(fence->GetCompletedValue()>=v) return true;
            fence->SetEventOnCompletion(v,fEvt);
            return WaitForSingleObject(fEvt,ms)==WAIT_OBJECT_0||fence->GetCompletedValue()>=v;
        }
        MTLock lk(mt); ctx->Flush();
        return true;
    }

    void Configure() {
        auto set=[this](const char* k,const char* v) { av_opt_set(cctx->priv_data,k,v,0); };
        set("preset","p1"); set("tune","ull"); set("zerolatency","1"); set("rc-lookahead","0");
        set("rc","vbr"); set("multipass","disabled"); set("delay","0"); set("surfaces","4");
        if(codec==CODEC_H264) { set("cq","23"); set("forced-idr","1"); set("strict_gop","1"); }
        else if(codec==CODEC_H265) { set("cq","25"); set("forced-idr","1"); set("strict_gop","1"); }
        else { set("cq","28"); set("range","pc"); }
    }

public:
    VideoEncoder(int width,int height,int fps,ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Multithread* m,CodecType cc=CODEC_AV1)
        :w(width),h(height),curFps(fps),dev(d),ctx(c),mt(m),codec(cc) {
        dev->AddRef();
        if(ctx) ctx->AddRef(); else dev->GetImmediateContext(&ctx);
        if(mt) mt->AddRef();
        lastKey=steady_clock::now()-KEY_INT;
        InitSync();
        const AVCodec* enc=avcodec_find_encoder_by_name(EncName(cc));
        if(!enc) throw std::runtime_error("NVENC unavailable");
        cctx=avcodec_alloc_context3(enc);
        if(!cctx) throw std::runtime_error("Codec alloc failed");
        if(!InitHwCtx(enc)) throw std::runtime_error("NVENC init failed");
        int64_t br=CalcBR(w,h,fps);
        cctx->width=w; cctx->height=h;
        cctx->time_base={1,fps}; cctx->framerate={fps,1};
        cctx->bit_rate=br; cctx->rc_max_rate=br*2; cctx->rc_buffer_size=(int)(br*2);
        cctx->gop_size=fps*2; cctx->max_b_frames=0;
        cctx->flags|=AV_CODEC_FLAG_LOW_DELAY; cctx->flags2|=AV_CODEC_FLAG2_FAST;
        cctx->delay=0; cctx->thread_count=1;
        cctx->color_range=AVCOL_RANGE_JPEG;
        cctx->colorspace=AVCOL_SPC_BT709;
        cctx->color_primaries=AVCOL_PRI_BT709;
        cctx->color_trc=AVCOL_TRC_BT709;
        Configure();
        if(avcodec_open2(cctx,enc,nullptr)<0) throw std::runtime_error("Encoder open failed");
        hwFr=av_frame_alloc(); pkt=av_packet_alloc();
        if(!hwFr||!pkt) throw std::runtime_error("Frame/packet alloc failed");
        hwFr->format=cctx->pix_fmt; hwFr->width=w; hwFr->height=h;
        LOG("Encoder: %dx%d @ %dfps, %.2f Mbps, codec: %s",w,h,fps,br/1000000.0,EncName(cc));
    }

    ~VideoEncoder() {
        av_packet_free(&pkt); av_frame_free(&hwFr);
        av_buffer_unref(&hwFrCtx); av_buffer_unref(&hwDev);
        if(cctx) avcodec_free_context(&cctx);
        if(fEvt) CloseHandle(fEvt);
        SafeRelease(fence,c4,d5,mt,ctx,dev);
    }

    bool UpdateFPS(int fps) {
        if(fps==curFps||fps<1||fps>240) return false;
        int64_t br=CalcBR(w,h,fps);
        cctx->bit_rate=br; cctx->rc_max_rate=br*2; cctx->rc_buffer_size=(int)(br*2);
        cctx->time_base={1,fps}; cctx->framerate={fps,1}; cctx->gop_size=fps*2;
        curFps=fps; lastKey=steady_clock::now()-KEY_INT;
        return true;
    }

    void Flush() {
        avcodec_send_frame(cctx,nullptr);
        while(avcodec_receive_packet(cctx,pkt)==0) av_packet_unref(pkt);
        avcodec_flush_buffers(cctx);
        lastKey=steady_clock::now()-KEY_INT;
    }

    bool IsEncodeComplete() const { return !useFence||!fence||fence->GetCompletedValue()>=lastSig; }

    EncodedFrame* Encode(ID3D11Texture2D* tex,int64_t ts,bool forceKey=false) {
        LARGE_INTEGER t0,t1; QueryPerformanceCounter(&t0);
        out.Clear();
        if(!tex) return nullptr;
        D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc);
        if(desc.Width!=(UINT)w||desc.Height!=(UINT)h) return nullptr;
        bool needKey=forceKey||(steady_clock::now()-lastKey>=KEY_INT);
        int ret=av_hwframe_get_buffer(cctx->hw_frames_ctx,hwFr,0);
        if(ret<0) return nullptr;
        uint64_t sig=0;
        { MTLock lk(mt); ctx->CopySubresourceRegion((ID3D11Texture2D*)hwFr->data[0],(UINT)(intptr_t)hwFr->data[1],0,0,0,tex,0,nullptr); ctx->Flush(); sig=Signal(); }
        if(!WaitGPU(sig,16)) { av_frame_unref(hwFr); return nullptr; }
        hwFr->pts=frameNum++;
        hwFr->pict_type=needKey?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_NONE;
        hwFr->flags=needKey?(hwFr->flags|AV_FRAME_FLAG_KEY):(hwFr->flags&~AV_FRAME_FLAG_KEY);
        if(needKey) lastKey=steady_clock::now();
        ret=avcodec_send_frame(cctx,hwFr);
        bool gotKey=false;
        if(ret==AVERROR(EAGAIN)) {
            while(avcodec_receive_packet(cctx,pkt)==0) {
                if(pkt->flags&AV_PKT_FLAG_KEY) gotKey=true;
                out.data.insert(out.data.end(),pkt->data,pkt->data+pkt->size);
                av_packet_unref(pkt);
            }
            ret=avcodec_send_frame(cctx,hwFr);
        }
        if(ret<0&&ret!=AVERROR_EOF) { av_frame_unref(hwFr); return nullptr; }
        while(avcodec_receive_packet(cctx,pkt)==0) {
            if(pkt->flags&AV_PKT_FLAG_KEY) gotKey=true;
            out.data.insert(out.data.end(),pkt->data,pkt->data+pkt->size);
            av_packet_unref(pkt);
        }
        av_frame_unref(hwFr);
        if(out.data.empty()) return nullptr;
        QueryPerformanceCounter(&t1);
        static const int64_t freq=[] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
        out.ts=ts; out.encUs=((t1.QuadPart-t0.QuadPart)*1000000)/freq; out.isKey=gotKey;
        return &out;
    }
};
