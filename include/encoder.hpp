#pragma once

#include "common.hpp"

struct EncodedFrame {
    std::vector<uint8_t> data;
    int64_t ts = 0, encUs = 0;
    bool isKey = false;
    void Clear() { data.clear(); ts = encUs = 0; isKey = false; }
};

class VideoEncoder {
    AVCodecContext* codecContext = nullptr;
    AVFrame* hwFrame = nullptr;
    AVPacket* packet = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrameCtx = nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Multithread* multithread = nullptr;

    ID3D11Device5* device5 = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    ID3D11Fence* encodeFence = nullptr;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    uint64_t lastSignaledFence = 0;
    bool useFence = false;

    int width, height, frameNumber = 0;
    CodecType currentCodec = CODEC_H264;

    steady_clock::time_point lastKeyframe;
    static constexpr auto KEYFRAME_INTERVAL = 2000ms;
    static constexpr DWORD GPU_WAIT_TIMEOUT_MS = 16;

    EncodedFrame outputFrame;
    std::atomic<uint64_t> encodedCount{0}, failedCount{0};
    EncoderGPUMetrics gpuMetrics;

    static inline const int64_t qpcFreq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();

    bool InitHardwareContext(const AVCodec* codec) {
        if (strcmp(codec->name, "h264_nvenc") != 0 && strcmp(codec->name, "av1_nvenc") != 0) return false;

        hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hwDevice) return false;

        auto* dc = reinterpret_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hwDevice->data)->hwctx);
        dc->device = device; dc->device_context = context;

        if (av_hwdevice_ctx_init(hwDevice) < 0) { av_buffer_unref(&hwDevice); return false; }
        codecContext->hw_device_ctx = av_buffer_ref(hwDevice);

        hwFrameCtx = av_hwframe_ctx_alloc(hwDevice);
        if (!hwFrameCtx) { av_buffer_unref(&hwDevice); return false; }

        auto* fc = reinterpret_cast<AVHWFramesContext*>(hwFrameCtx->data);
        fc->format = AV_PIX_FMT_D3D11; fc->sw_format = AV_PIX_FMT_BGRA;
        fc->width = width; fc->height = height; fc->initial_pool_size = 4;

        if (av_hwframe_ctx_init(hwFrameCtx) < 0) { av_buffer_unref(&hwFrameCtx); av_buffer_unref(&hwDevice); return false; }
        codecContext->hw_frames_ctx = av_buffer_ref(hwFrameCtx);
        codecContext->pix_fmt = AV_PIX_FMT_D3D11;
        return true;
    }

    void InitGPUSync() {
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4))) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&encodeFence)))) {
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (fenceEvent) { useFence = true; LOG("Encoder GPU sync: Fence"); return; }
        }
        SafeRelease(device5, context4, encodeFence);
        LOG("Encoder GPU sync: Flush");
    }

    uint64_t SignalGPU() {
        if (useFence && context4 && encodeFence) {
            context4->Signal(encodeFence, ++fenceValue);
            lastSignaledFence = fenceValue;
            return fenceValue;
        }
        return 0;
    }

    bool WaitForGPU(uint64_t signalValue, DWORD timeoutMs = GPU_WAIT_TIMEOUT_MS) {
        if (useFence && encodeFence) {
            if (encodeFence->GetCompletedValue() >= signalValue) { gpuMetrics.Record(0, false); return true; }
            LARGE_INTEGER start, end; QueryPerformanceCounter(&start);
            encodeFence->SetEventOnCompletion(signalValue, fenceEvent);
            bool ok = (WaitForSingleObject(fenceEvent, timeoutMs) == WAIT_OBJECT_0) || (encodeFence->GetCompletedValue() >= signalValue);
            QueryPerformanceCounter(&end);
            if (ok) gpuMetrics.Record((end.QuadPart - start.QuadPart) * 1000000 / qpcFreq, true);
            else gpuMetrics.RecordTimeout();
            return ok;
        }
        MTLock lock(multithread);
        context->Flush();
        return true;
    }

    void ConfigureH264Encoder() {
        auto set = [this](const char* k, const char* v) { av_opt_set(codecContext->priv_data, k, v, 0); };
        set("preset", "p1"); set("tune", "ull"); set("zerolatency", "1"); set("rc-lookahead", "0");
        set("rc", "vbr"); set("cq", "23"); set("multipass", "disabled");
        set("delay", "0"); set("surfaces", "4"); set("forced-idr", "1"); set("strict_gop", "1");
        set("spatial-aq", "0"); set("temporal-aq", "0"); set("b_adapt", "0"); set("nonref_p", "0");
        set("aud", "0"); set("bluray-compat", "0");
    }

    void ConfigureAV1Encoder() {
        auto set = [this](const char* k, const char* v) { av_opt_set(codecContext->priv_data, k, v, 0); };
        set("preset", "p1"); set("tune", "ull"); set("rc-lookahead", "0"); set("multipass", "disabled");
        set("rc", "vbr"); set("cq", "28"); set("delay", "0"); set("surfaces", "4"); set("range", "pc");
    }

    bool ValidateTexture(ID3D11Texture2D* texture) {
        if (!texture) return false;
        D3D11_TEXTURE2D_DESC desc; texture->GetDesc(&desc);
        return desc.Width == static_cast<UINT>(width) && desc.Height == static_cast<UINT>(height) && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    }

public:
    VideoEncoder(int w, int h, int fps, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Multithread* mt, CodecType codecType = CODEC_H264)
        : width(w), height(h), device(dev), context(ctx), multithread(mt), currentCodec(codecType) {
        device->AddRef();
        if (context) context->AddRef(); else device->GetImmediateContext(&context);
        if (multithread) multithread->AddRef();
        lastKeyframe = steady_clock::now() - KEYFRAME_INTERVAL;
        InitGPUSync();

        const AVCodec* codec = avcodec_find_encoder_by_name(codecType == CODEC_H264 ? "h264_nvenc" : "av1_nvenc");
        if (!codec) throw std::runtime_error("NVENC not available");
        LOG("Encoder: %s", codec->name);

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) throw std::runtime_error("Failed to allocate codec context");
        if (!InitHardwareContext(codec)) throw std::runtime_error("Failed to initialize NVENC hardware context");

        codecContext->width = width; codecContext->height = height;
        codecContext->time_base = {1, fps}; codecContext->framerate = {fps, 1};
        codecContext->bit_rate = 20000000; codecContext->rc_max_rate = 40000000; codecContext->rc_buffer_size = 40000000;
        codecContext->gop_size = fps * 2; codecContext->max_b_frames = 0;
        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY; codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
        codecContext->delay = 0; codecContext->has_b_frames = 0; codecContext->thread_count = 1;
        codecContext->color_range = AVCOL_RANGE_JPEG;
        codecContext->colorspace = AVCOL_SPC_BT709;
        codecContext->color_primaries = AVCOL_PRI_BT709;
        codecContext->color_trc = AVCOL_TRC_BT709;

        if (codecType == CODEC_H264) ConfigureH264Encoder();
        else ConfigureAV1Encoder();

        if (avcodec_open2(codecContext, codec, nullptr) < 0) throw std::runtime_error("Failed to open encoder");

        hwFrame = av_frame_alloc(); packet = av_packet_alloc();
        if (!hwFrame || !packet) throw std::runtime_error("Failed to allocate frame or packet");

        hwFrame->format = codecContext->pix_fmt; hwFrame->width = width; hwFrame->height = height;
        LOG("NVENC encoder initialized: %dx%d", width, height);
    }

    ~VideoEncoder() {
        av_packet_free(&packet); av_frame_free(&hwFrame);
        av_buffer_unref(&hwFrameCtx); av_buffer_unref(&hwDevice);
        if (codecContext) avcodec_free_context(&codecContext);
        if (fenceEvent) CloseHandle(fenceEvent);
        SafeRelease(encodeFence, context4, device5, multithread, context, device);
    }

    void Flush() {
        avcodec_send_frame(codecContext, nullptr);
        while (avcodec_receive_packet(codecContext, packet) == 0) av_packet_unref(packet);
        avcodec_flush_buffers(codecContext);
        lastKeyframe = steady_clock::now() - KEYFRAME_INTERVAL;
    }

    bool IsEncodeComplete() const {
        if (!useFence || !encodeFence) return true;
        return encodeFence->GetCompletedValue() >= lastSignaledFence;
    }

    EncodedFrame* Encode(ID3D11Texture2D* texture, int64_t timestamp, bool forceKeyframe = false) {
        LARGE_INTEGER startTime, endTime; QueryPerformanceCounter(&startTime);
        outputFrame.Clear();

        if (!ValidateTexture(texture)) { failedCount++; return nullptr; }
        bool needsKeyframe = forceKeyframe || (steady_clock::now() - lastKeyframe >= KEYFRAME_INTERVAL);

        if (av_hwframe_get_buffer(codecContext->hw_frames_ctx, hwFrame, 0) < 0) { failedCount++; return nullptr; }
        uint64_t signalValue = 0;
        { MTLock lock(multithread); context->CopySubresourceRegion(reinterpret_cast<ID3D11Texture2D*>(hwFrame->data[0]), static_cast<UINT>(reinterpret_cast<intptr_t>(hwFrame->data[1])), 0, 0, 0, texture, 0, nullptr); context->Flush(); signalValue = SignalGPU(); }
        if (!WaitForGPU(signalValue, GPU_WAIT_TIMEOUT_MS)) { failedCount++; av_frame_unref(hwFrame); return nullptr; }

        hwFrame->pts = frameNumber++;
        hwFrame->pict_type = needsKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        hwFrame->flags = needsKeyframe ? (hwFrame->flags | AV_FRAME_FLAG_KEY) : (hwFrame->flags & ~AV_FRAME_FLAG_KEY);
        if (needsKeyframe) lastKeyframe = steady_clock::now();

        int ret = avcodec_send_frame(codecContext, hwFrame);
        bool gotKeyframe = false;

        if (ret == AVERROR(EAGAIN)) {
            while (avcodec_receive_packet(codecContext, packet) == 0) {
                if (packet->flags & AV_PKT_FLAG_KEY) gotKeyframe = true;
                outputFrame.data.insert(outputFrame.data.end(), packet->data, packet->data + packet->size);
                av_packet_unref(packet);
            }
            ret = avcodec_send_frame(codecContext, hwFrame);
        }

        if (ret < 0 && ret != AVERROR_EOF) { failedCount++; av_frame_unref(hwFrame); return nullptr; }

        while (avcodec_receive_packet(codecContext, packet) == 0) {
            if (packet->flags & AV_PKT_FLAG_KEY) gotKeyframe = true;
            outputFrame.data.insert(outputFrame.data.end(), packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }

        av_frame_unref(hwFrame);
        if (outputFrame.data.empty()) return nullptr;

        QueryPerformanceCounter(&endTime);
        outputFrame.ts = timestamp;
        outputFrame.encUs = ((endTime.QuadPart - startTime.QuadPart) * 1000000) / qpcFreq;
        outputFrame.isKey = gotKeyframe;
        encodedCount++;
        return &outputFrame;
    }

    uint64_t GetEncoded() { return encodedCount.exchange(0); }
    uint64_t GetFailed() { return failedCount.exchange(0); }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
    CodecType GetCodec() const { return currentCodec; }
    EncoderGPUMetrics::Snapshot GetGPUMetrics() { return gpuMetrics.GetAndReset(); }
};
