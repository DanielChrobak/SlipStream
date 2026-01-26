#pragma once

#include "common.hpp"

struct EncodedFrame {
    std::vector<uint8_t> data;
    int64_t ts = 0, encUs = 0;
    bool isKey = false;
    void Clear() { data.clear(); ts = encUs = 0; isKey = false; }
};

struct EncoderGPUMetrics {
    std::atomic<int64_t> minWaitUs{INT64_MAX}, maxWaitUs{0}, sumWaitUs{0};
    std::atomic<uint64_t> waitCount{0}, timeoutCount{0}, noWaitCount{0};

    void RecordWait(int64_t waitUs, bool timedOut) {
        if (timedOut) { timeoutCount++; return; }
        if (waitUs <= 0) { noWaitCount++; return; }
        waitCount++; sumWaitUs += waitUs;
        for (int64_t m = minWaitUs.load(); waitUs < m && !minWaitUs.compare_exchange_weak(m, waitUs););
        for (int64_t m = maxWaitUs.load(); waitUs > m && !maxWaitUs.compare_exchange_weak(m, waitUs););
    }

    struct Snapshot { int64_t minUs, maxUs, avgUs; uint64_t count, timeouts, noWait; };

    Snapshot GetAndReset() {
        Snapshot s{minWaitUs.exchange(INT64_MAX), maxWaitUs.exchange(0), 0,
                   waitCount.exchange(0), timeoutCount.exchange(0), noWaitCount.exchange(0)};
        int64_t sum = sumWaitUs.exchange(0);
        s.avgUs = s.count > 0 ? sum / static_cast<int64_t>(s.count) : 0;
        if (s.minUs == INT64_MAX) s.minUs = 0;
        return s;
    }
};

class AV1Encoder {
    AVCodecContext* codecContext = nullptr;
    AVFrame* hwFrame = nullptr;
    AVPacket* packet = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrameCtx = nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Multithread* multithread = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;

    ID3D11Device5* device5 = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    ID3D11Fence* encodeFence = nullptr;
    HANDLE fenceEvent = nullptr;
    ID3D11Query* encodeQuery = nullptr;
    uint64_t fenceValue = 0;
    bool useFence = false;

    int width, height, frameNumber = 0;
    UINT stagingWidth = 0, stagingHeight = 0;
    bool useHardware = false;

    steady_clock::time_point lastKeyframe;
    static constexpr auto KEYFRAME_INTERVAL = 2000ms;
    static constexpr DWORD GPU_WAIT_TIMEOUT_MS = 16;

    EncodedFrame outputFrame;
    std::atomic<uint64_t> encodedCount{0}, failedCount{0}, lastEncodeFence{0};
    EncoderGPUMetrics gpuMetrics;

    static inline const int64_t qpcFreq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    int64_t GetUs() const { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (n.QuadPart * 1000000) / qpcFreq; }

    bool InitHardwareContext(const AVCodec* codec) {
        if (strcmp(codec->name, "av1_nvenc")) return false;
        hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hwDevice) return false;

        auto* dc = reinterpret_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hwDevice->data)->hwctx);
        dc->device = device;
        dc->device_context = context;

        if (av_hwdevice_ctx_init(hwDevice) < 0) { av_buffer_unref(&hwDevice); return false; }
        codecContext->hw_device_ctx = av_buffer_ref(hwDevice);

        hwFrameCtx = av_hwframe_ctx_alloc(hwDevice);
        if (!hwFrameCtx) { av_buffer_unref(&hwDevice); return false; }

        auto* fc = reinterpret_cast<AVHWFramesContext*>(hwFrameCtx->data);
        fc->format = AV_PIX_FMT_D3D11;
        fc->sw_format = AV_PIX_FMT_BGRA;
        fc->width = width;
        fc->height = height;
        fc->initial_pool_size = 4;

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
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        if (SUCCEEDED(device->CreateQuery(&qd, &encodeQuery))) LOG("Encoder GPU sync: Query");
    }

    uint64_t SignalGPU() {
        if (useFence && context4 && encodeFence) { context4->Signal(encodeFence, ++fenceValue); return fenceValue; }
        if (encodeQuery) context->End(encodeQuery);
        return 0;
    }

    bool WaitForGPU(uint64_t signalValue, DWORD timeoutMs = GPU_WAIT_TIMEOUT_MS) {
        int64_t startUs = GetUs();
        if (useFence && encodeFence) {
            if (encodeFence->GetCompletedValue() >= signalValue) { gpuMetrics.RecordWait(0, false); return true; }
            encodeFence->SetEventOnCompletion(signalValue, fenceEvent);
            bool completed = (WaitForSingleObject(fenceEvent, timeoutMs) == WAIT_OBJECT_0) || (encodeFence->GetCompletedValue() >= signalValue);
            gpuMetrics.RecordWait(GetUs() - startUs, !completed);
            return completed;
        }
        if (encodeQuery) {
            { MTLock lock(multithread); context->Flush(); if (context->GetData(encodeQuery, nullptr, 0, 0) == S_OK) { gpuMetrics.RecordWait(0, false); return true; } }
            bool completed = false;
            for (DWORD elapsed = 0; elapsed < timeoutMs && !completed; elapsed++) { Sleep(1); MTLock lock(multithread); completed = context->GetData(encodeQuery, nullptr, 0, 0) == S_OK; }
            gpuMetrics.RecordWait(GetUs() - startUs, !completed);
            return completed;
        }
        gpuMetrics.RecordWait(0, false);
        return true;
    }

    void ConfigureEncoder(const AVCodec* codec) {
        auto set = [this](const char* k, const char* v) { av_opt_set(codecContext->priv_data, k, v, 0); };
        if (!strcmp(codec->name, "av1_nvenc")) {
            set("preset", "p1"); set("tune", "ull"); set("rc", "cbr"); set("cq", "23"); set("delay", "0");
            set("zerolatency", "1"); set("lookahead", "0"); set("rc-lookahead", "0"); set("forced-idr", "1");
            set("b_adapt", "0"); set("spatial-aq", "0"); set("temporal-aq", "0"); set("nonref_p", "1");
            set("strict_gop", "1"); set("multipass", "disabled"); set("ldkfs", "1"); set("surfaces", "8");
            set("aud", "0"); set("bluray-compat", "0");
        } else {
            set("preset", "12"); set("crf", "28");
            set("svtav1-params", "tune=0:fast-decode=1:enable-overlays=0:scd=0:lookahead=0:lp=1:tile-rows=0:tile-columns=1:enable-tf=0:enable-cdef=0:enable-restoration=0:rmv=0:film-grain=0");
        }
    }

    bool ValidateTexture(ID3D11Texture2D* texture) {
        if (!texture) return false;
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        return desc.Width == static_cast<UINT>(width) && desc.Height == static_cast<UINT>(height) && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    }

public:
    AV1Encoder(int w, int h, int fps, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Multithread* mt)
        : width(w), height(h), device(dev), context(ctx), multithread(mt) {
        device->AddRef();
        if (context) context->AddRef(); else device->GetImmediateContext(&context);
        if (multithread) multithread->AddRef();
        lastKeyframe = steady_clock::now() - KEYFRAME_INTERVAL;
        InitGPUSync();

        const AVCodec* codec = avcodec_find_encoder_by_name("av1_nvenc");
        if (!codec) codec = avcodec_find_encoder_by_name("libsvtav1");
        if (!codec) throw std::runtime_error("No AV1 encoder available");
        LOG("Encoder: %s", codec->name);

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) throw std::runtime_error("Failed to allocate codec context");

        useHardware = !strcmp(codec->name, "av1_nvenc") && InitHardwareContext(codec);
        if (!useHardware) codecContext->pix_fmt = AV_PIX_FMT_BGRA;

        codecContext->width = width;
        codecContext->height = height;
        codecContext->time_base = {1, fps};
        codecContext->framerate = {fps, 1};
        codecContext->bit_rate = 20000000;
        codecContext->rc_max_rate = 40000000;
        codecContext->rc_buffer_size = 40000000;
        codecContext->gop_size = fps * 2;
        codecContext->max_b_frames = 0;
        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
        codecContext->delay = 0;
        codecContext->has_b_frames = 0;
        codecContext->thread_count = useHardware ? 1 : std::min(4, std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2));

        ConfigureEncoder(codec);
        if (avcodec_open2(codecContext, codec, nullptr) < 0) throw std::runtime_error("Failed to open encoder");

        hwFrame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!hwFrame || !packet) throw std::runtime_error("Failed to allocate frame or packet");

        hwFrame->format = codecContext->pix_fmt;
        hwFrame->width = width;
        hwFrame->height = height;
        if (!useHardware && av_frame_get_buffer(hwFrame, 32) < 0) throw std::runtime_error("Failed to allocate frame buffer");
        LOG("Encoder mode: %s", useHardware ? "Hardware" : "Software");
    }

    ~AV1Encoder() {
        av_packet_free(&packet);
        av_frame_free(&hwFrame);
        av_buffer_unref(&hwFrameCtx);
        av_buffer_unref(&hwDevice);
        if (codecContext) avcodec_free_context(&codecContext);
        if (fenceEvent) CloseHandle(fenceEvent);
        SafeRelease(encodeFence, context4, device5, encodeQuery, stagingTexture, multithread, context, device);
    }

    void Flush() {
        avcodec_send_frame(codecContext, nullptr);
        while (avcodec_receive_packet(codecContext, packet) == 0) av_packet_unref(packet);
        avcodec_flush_buffers(codecContext);
        lastKeyframe = steady_clock::now() - KEYFRAME_INTERVAL;
    }

    EncodedFrame* Encode(ID3D11Texture2D* texture, int64_t timestamp, bool forceKeyframe = false) {
        LARGE_INTEGER startTime, endTime;
        QueryPerformanceCounter(&startTime);
        outputFrame.Clear();

        if (!ValidateTexture(texture)) { failedCount++; return nullptr; }
        bool needsKeyframe = forceKeyframe || (steady_clock::now() - lastKeyframe >= KEYFRAME_INTERVAL);

        if (useHardware) {
            if (av_hwframe_get_buffer(codecContext->hw_frames_ctx, hwFrame, 0) < 0) { failedCount++; return nullptr; }
            uint64_t signalValue = 0;
            {
                MTLock lock(multithread);
                context->CopySubresourceRegion(reinterpret_cast<ID3D11Texture2D*>(hwFrame->data[0]),
                    static_cast<UINT>(reinterpret_cast<intptr_t>(hwFrame->data[1])), 0, 0, 0, texture, 0, nullptr);
                context->Flush();
                signalValue = SignalGPU();
            }
            if (!WaitForGPU(signalValue, GPU_WAIT_TIMEOUT_MS)) { failedCount++; av_frame_unref(hwFrame); return nullptr; }
            lastEncodeFence = signalValue;
        } else {
            D3D11_TEXTURE2D_DESC td;
            texture->GetDesc(&td);
            if (!stagingTexture || stagingWidth != td.Width || stagingHeight != td.Height) {
                SafeRelease(stagingTexture);
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                device->CreateTexture2D(&td, nullptr, &stagingTexture);
                stagingWidth = td.Width;
                stagingHeight = td.Height;
            }
            uint64_t signalValue = 0;
            { MTLock lock(multithread); context->CopyResource(stagingTexture, texture); context->Flush(); signalValue = SignalGPU(); }
            if (!WaitForGPU(signalValue, GPU_WAIT_TIMEOUT_MS)) { failedCount++; return nullptr; }

            D3D11_MAPPED_SUBRESOURCE mapped;
            { MTLock lock(multithread); if (FAILED(context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped))) { failedCount++; return nullptr; } }
            if (av_frame_make_writable(hwFrame) < 0) { MTLock lock(multithread); context->Unmap(stagingTexture, 0); failedCount++; return nullptr; }

            auto* src = static_cast<uint8_t*>(mapped.pData);
            for (int y = 0; y < height; y++) memcpy(hwFrame->data[0] + y * hwFrame->linesize[0], src + y * mapped.RowPitch, width * 4);
            { MTLock lock(multithread); context->Unmap(stagingTexture, 0); }
            lastEncodeFence = signalValue;
        }

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

        if (ret < 0 && ret != AVERROR_EOF) { failedCount++; if (useHardware) av_frame_unref(hwFrame); return nullptr; }

        while (avcodec_receive_packet(codecContext, packet) == 0) {
            if (packet->flags & AV_PKT_FLAG_KEY) gotKeyframe = true;
            outputFrame.data.insert(outputFrame.data.end(), packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }

        if (useHardware) av_frame_unref(hwFrame);
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
    EncoderGPUMetrics::Snapshot GetGPUMetrics() { return gpuMetrics.GetAndReset(); }
    bool IsEncodeComplete() { return !useFence || !encodeFence || encodeFence->GetCompletedValue() >= lastEncodeFence.load(); }
};
