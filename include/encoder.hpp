#pragma once
#include "common.hpp"

struct EncodedFrame {
    std::vector<uint8_t> data;
    int64_t ts = 0, encUs = 0;
    bool isKey = false;
    void Clear() { data.clear(); ts = encUs = 0; isKey = false; }
};

class VideoEncoder {
    AVCodecContext* codecCtx = nullptr;
    AVFrame* hwFrame = nullptr;
    AVPacket* pkt = nullptr;
    AVBufferRef* hwDev = nullptr, *hwFrameCtx = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Multithread* mt = nullptr;
    ID3D11Device5* dev5 = nullptr;
    ID3D11DeviceContext4* ctx4 = nullptr;
    ID3D11Fence* fence = nullptr;
    HANDLE fenceEvt = nullptr;
    uint64_t fenceVal = 0, lastSig = 0;
    bool useFence = false;
    int w, h, frameNum = 0, curFps = 60;
    CodecType codec = CODEC_H264;
    steady_clock::time_point lastKey;
    static constexpr auto KEY_INTERVAL = 2000ms;
    EncodedFrame out;

    static int64_t CalcBitrate(int w, int h, int fps) { return (int64_t)(0.18085 * w * h * fps); }

    bool InitHwCtx(const AVCodec* c) {
        if (strcmp(c->name, "h264_nvenc") != 0 && strcmp(c->name, "av1_nvenc") != 0) return false;
        hwDev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hwDev) return false;
        auto* dc = (AVD3D11VADeviceContext*)((AVHWDeviceContext*)hwDev->data)->hwctx;
        dc->device = dev; dc->device_context = ctx;
        if (av_hwdevice_ctx_init(hwDev) < 0) { av_buffer_unref(&hwDev); return false; }
        codecCtx->hw_device_ctx = av_buffer_ref(hwDev);
        hwFrameCtx = av_hwframe_ctx_alloc(hwDev);
        if (!hwFrameCtx) { av_buffer_unref(&hwDev); return false; }
        auto* fc = (AVHWFramesContext*)hwFrameCtx->data;
        fc->format = AV_PIX_FMT_D3D11; fc->sw_format = AV_PIX_FMT_BGRA;
        fc->width = w; fc->height = h; fc->initial_pool_size = 4;
        if (av_hwframe_ctx_init(hwFrameCtx) < 0) { av_buffer_unref(&hwFrameCtx); av_buffer_unref(&hwDev); return false; }
        codecCtx->hw_frames_ctx = av_buffer_ref(hwFrameCtx);
        codecCtx->pix_fmt = AV_PIX_FMT_D3D11;
        return true;
    }

    void InitSync() {
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev5))) &&
            SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))) &&
            SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            fenceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (fenceEvt) { useFence = true; return; }
        }
        SafeRelease(dev5, ctx4, fence);
    }

    uint64_t Signal() { if (useFence && ctx4 && fence) { ctx4->Signal(fence, ++fenceVal); lastSig = fenceVal; return fenceVal; } return 0; }

    bool WaitGPU(uint64_t v, DWORD ms = 16) {
        if (useFence && fence) {
            if (fence->GetCompletedValue() >= v) return true;
            fence->SetEventOnCompletion(v, fenceEvt);
            return WaitForSingleObject(fenceEvt, ms) == WAIT_OBJECT_0 || fence->GetCompletedValue() >= v;
        }
        MTLock lk(mt); ctx->Flush();
        return true;
    }

    void Configure() {
        auto set = [this](const char* k, const char* v) { av_opt_set(codecCtx->priv_data, k, v, 0); };
        set("preset", "p1"); set("tune", "ull"); set("zerolatency", "1"); set("rc-lookahead", "0");
        set("rc", "vbr"); set("multipass", "disabled"); set("delay", "0"); set("surfaces", "4");
        if (codec == CODEC_H264) { set("cq", "23"); set("forced-idr", "1"); set("strict_gop", "1"); }
        else { set("cq", "28"); set("range", "pc"); }
    }

public:
    VideoEncoder(int width, int height, int fps, ID3D11Device* d, ID3D11DeviceContext* c, ID3D11Multithread* m, CodecType cc = CODEC_H264)
        : w(width), h(height), curFps(fps), dev(d), ctx(c), mt(m), codec(cc) {
        dev->AddRef();
        if (ctx) ctx->AddRef(); else dev->GetImmediateContext(&ctx);
        if (mt) mt->AddRef();
        lastKey = steady_clock::now() - KEY_INTERVAL;
        InitSync();
        const AVCodec* enc = avcodec_find_encoder_by_name(cc == CODEC_H264 ? "h264_nvenc" : "av1_nvenc");
        if (!enc) throw std::runtime_error("NVENC unavailable");
        codecCtx = avcodec_alloc_context3(enc);
        if (!codecCtx) throw std::runtime_error("Codec alloc failed");
        if (!InitHwCtx(enc)) throw std::runtime_error("NVENC init failed");
        int64_t br = CalcBitrate(w, h, fps);
        codecCtx->width = w; codecCtx->height = h;
        codecCtx->time_base = {1, fps}; codecCtx->framerate = {fps, 1};
        codecCtx->bit_rate = br; codecCtx->rc_max_rate = br * 2; codecCtx->rc_buffer_size = (int)(br * 2);
        codecCtx->gop_size = fps * 2; codecCtx->max_b_frames = 0;
        codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY; codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
        codecCtx->delay = 0; codecCtx->thread_count = 1;
        codecCtx->color_range = AVCOL_RANGE_JPEG;
        codecCtx->colorspace = AVCOL_SPC_BT709;
        codecCtx->color_primaries = AVCOL_PRI_BT709;
        codecCtx->color_trc = AVCOL_TRC_BT709;
        Configure();
        if (avcodec_open2(codecCtx, enc, nullptr) < 0) throw std::runtime_error("Encoder open failed");
        hwFrame = av_frame_alloc(); pkt = av_packet_alloc();
        if (!hwFrame || !pkt) throw std::runtime_error("Frame/packet alloc failed");
        hwFrame->format = codecCtx->pix_fmt; hwFrame->width = w; hwFrame->height = h;
        LOG("Encoder: %dx%d @ %dfps, %.2f Mbps", w, h, fps, br / 1000000.0);
    }

    ~VideoEncoder() {
        av_packet_free(&pkt); av_frame_free(&hwFrame);
        av_buffer_unref(&hwFrameCtx); av_buffer_unref(&hwDev);
        if (codecCtx) avcodec_free_context(&codecCtx);
        if (fenceEvt) CloseHandle(fenceEvt);
        SafeRelease(fence, ctx4, dev5, mt, ctx, dev);
    }

    bool UpdateFPS(int fps) {
        if (fps == curFps || fps < 1 || fps > 240) return false;
        int64_t br = CalcBitrate(w, h, fps);
        codecCtx->bit_rate = br; codecCtx->rc_max_rate = br * 2; codecCtx->rc_buffer_size = (int)(br * 2);
        codecCtx->time_base = {1, fps}; codecCtx->framerate = {fps, 1}; codecCtx->gop_size = fps * 2;
        curFps = fps; lastKey = steady_clock::now() - KEY_INTERVAL;
        return true;
    }

    void Flush() {
        avcodec_send_frame(codecCtx, nullptr);
        while (avcodec_receive_packet(codecCtx, pkt) == 0) av_packet_unref(pkt);
        avcodec_flush_buffers(codecCtx);
        lastKey = steady_clock::now() - KEY_INTERVAL;
    }

    bool IsEncodeComplete() const { return !useFence || !fence || fence->GetCompletedValue() >= lastSig; }

    EncodedFrame* Encode(ID3D11Texture2D* tex, int64_t ts, bool forceKey = false) {
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        out.Clear();
        if (!tex) return nullptr;
        D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc);
        if (desc.Width != (UINT)w || desc.Height != (UINT)h) return nullptr;
        bool needKey = forceKey || (steady_clock::now() - lastKey >= KEY_INTERVAL);
        if (av_hwframe_get_buffer(codecCtx->hw_frames_ctx, hwFrame, 0) < 0) return nullptr;
        uint64_t sig = 0;
        { MTLock lk(mt); ctx->CopySubresourceRegion((ID3D11Texture2D*)hwFrame->data[0], (UINT)(intptr_t)hwFrame->data[1], 0, 0, 0, tex, 0, nullptr); ctx->Flush(); sig = Signal(); }
        if (!WaitGPU(sig, 16)) { av_frame_unref(hwFrame); return nullptr; }
        hwFrame->pts = frameNum++;
        hwFrame->pict_type = needKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        hwFrame->flags = needKey ? (hwFrame->flags | AV_FRAME_FLAG_KEY) : (hwFrame->flags & ~AV_FRAME_FLAG_KEY);
        if (needKey) lastKey = steady_clock::now();
        int ret = avcodec_send_frame(codecCtx, hwFrame);
        bool gotKey = false;
        if (ret == AVERROR(EAGAIN)) {
            while (avcodec_receive_packet(codecCtx, pkt) == 0) {
                if (pkt->flags & AV_PKT_FLAG_KEY) gotKey = true;
                out.data.insert(out.data.end(), pkt->data, pkt->data + pkt->size);
                av_packet_unref(pkt);
            }
            ret = avcodec_send_frame(codecCtx, hwFrame);
        }
        if (ret < 0 && ret != AVERROR_EOF) { av_frame_unref(hwFrame); return nullptr; }
        while (avcodec_receive_packet(codecCtx, pkt) == 0) {
            if (pkt->flags & AV_PKT_FLAG_KEY) gotKey = true;
            out.data.insert(out.data.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
        av_frame_unref(hwFrame);
        if (out.data.empty()) return nullptr;
        QueryPerformanceCounter(&t1);
        static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
        out.ts = ts; out.encUs = ((t1.QuadPart - t0.QuadPart) * 1000000) / freq; out.isKey = gotKey;
        return &out;
    }
};
