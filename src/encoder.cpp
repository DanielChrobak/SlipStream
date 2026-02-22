#include "encoder.hpp"

namespace {
    constexpr const char* ENC_NAMES[3][3] = {
        {"av1_nvenc", "hevc_nvenc", "h264_nvenc"},
        {"av1_qsv", "hevc_qsv", "h264_qsv"},
        {"av1_amf", "hevc_amf", "h264_amf"}
    };

    inline int64_t CalcBitrate(int w, int h, int fps) {
        return int64_t(0.18085 * w * h * fps);
    }

    inline const char* GetEncName(CodecType c, GPUVendor v) {
        return v <= GPUVendor::AMD ? ENC_NAMES[static_cast<int>(v)][static_cast<int>(c)] : nullptr;
    }

    std::vector<GPUVendor> GetVendorPriority(GPUVendor detected) {
        std::vector<GPUVendor> list;
        if (detected != GPUVendor::UNKNOWN) list.push_back(detected);
        for (auto v : {GPUVendor::NVIDIA, GPUVendor::INTEL, GPUVendor::AMD})
            if (v != detected) list.push_back(v);
        return list;
    }
}

const char* VideoEncoder::VendorName(GPUVendor v) {
    static const char* names[] = {"NVIDIA NVENC", "Intel QSV", "AMD AMF", "Unknown"};
    return names[v <= GPUVendor::AMD ? static_cast<int>(v) : 3];
}

const char* VideoEncoder::CodecName(CodecType c) {
    static const char* names[] = {"AV1", "H.265/HEVC", "H.264/AVC"};
    return c <= CODEC_H264 ? names[static_cast<int>(c)] : "Unknown";
}

GPUVendor VideoEncoder::DetectGPU(ID3D11Device* device) {
    if (!device) return GPUVendor::UNKNOWN;

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    GPUVendor result = GPUVendor::UNKNOWN;

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                switch (desc.VendorId) {
                    case 0x10DE: result = GPUVendor::NVIDIA; break;
                    case 0x8086: result = GPUVendor::INTEL; break;
                    case 0x1002: result = GPUVendor::AMD; break;
                }
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                DBG("VideoEncoder: GPU detected: %s (VendorID: 0x%04X)", name, desc.VendorId);
            }
            adapter->Release();
        }
        dxgiDev->Release();
    }
    return result;
}

uint8_t VideoEncoder::ProbeSupport(ID3D11Device* device) {
    uint8_t support = 0;
    GPUVendor detected = DetectGPU(device);
    LOG("VideoEncoder: Probing encoder support (detected GPU: %s)", VendorName(detected));

    for (GPUVendor v : GetVendorPriority(detected)) {
        for (int c = 0; c <= 2; c++) {
            if (!(support & (1 << c))) {
                if (auto* name = GetEncName(static_cast<CodecType>(c), v)) {
                    if (avcodec_find_encoder_by_name(name)) {
                        support |= (1 << c);
                        DBG("VideoEncoder: Found encoder %s for %s", name, CodecName(static_cast<CodecType>(c)));
                    }
                }
            }
        }
    }
    LOG("VideoEncoder: Codec support: AV1=%d H265=%d H264=%d",
        (support&1)?1:0, (support&2)?1:0, (support&4)?1:0);
    return support;
}

bool VideoEncoder::InitHwCtx() {
    hwDev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwDev) { ERR("VideoEncoder: av_hwdevice_ctx_alloc failed"); return false; }

    auto* dc = (AVD3D11VADeviceContext*)((AVHWDeviceContext*)hwDev->data)->hwctx;
    dc->device = dev;
    dc->device_context = ctx;

    int ret = av_hwdevice_ctx_init(hwDev);
    if (ret < 0) {
        ERR("VideoEncoder: av_hwdevice_ctx_init failed: %s", AvErr(ret));
        av_buffer_unref(&hwDev);
        return false;
    }
    cctx->hw_device_ctx = av_buffer_ref(hwDev);

    hwFrCtx = av_hwframe_ctx_alloc(hwDev);
    if (!hwFrCtx) {
        ERR("VideoEncoder: av_hwframe_ctx_alloc failed");
        av_buffer_unref(&hwDev);
        return false;
    }

    auto* fc = (AVHWFramesContext*)hwFrCtx->data;
    fc->format = AV_PIX_FMT_D3D11;
    fc->sw_format = AV_PIX_FMT_BGRA;
    fc->width = w;
    fc->height = h;
    fc->initial_pool_size = 4;

    ret = av_hwframe_ctx_init(hwFrCtx);
    if (ret < 0) {
        ERR("VideoEncoder: av_hwframe_ctx_init failed: %s", AvErr(ret));
        av_buffer_unref(&hwFrCtx);
        av_buffer_unref(&hwDev);
        return false;
    }

    cctx->hw_frames_ctx = av_buffer_ref(hwFrCtx);
    cctx->pix_fmt = AV_PIX_FMT_D3D11;
    DBG("VideoEncoder: Hardware context initialized");
    return true;
}

void VideoEncoder::InitSync() {
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d5)))) return;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c4)))) { SafeRelease(d5); return; }
    if (FAILED(d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        SafeRelease(d5, c4);
        return;
    }
    fEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fEvt) { SafeRelease(fence, c4, d5); return; }
    useFence = true;
    DBG("VideoEncoder: Using D3D11 fence-based GPU sync");
}

uint64_t VideoEncoder::Signal() {
    if (useFence && c4 && fence && SUCCEEDED(c4->Signal(fence, ++fVal))) {
        lastSig = fVal;
        return fVal;
    }
    return 0;
}

bool VideoEncoder::WaitGPU(uint64_t v, DWORD ms) {
    if (useFence && fence) {
        if (fence->GetCompletedValue() >= v) return true;
        if (FAILED(fence->SetEventOnCompletion(v, fEvt))) return false;
        DWORD result = WaitForSingleObject(fEvt, ms);
        return result == WAIT_OBJECT_0 || fence->GetCompletedValue() >= v;
    }
    MTLock lk(mt);
    ctx->Flush();
    return true;
}

void VideoEncoder::Configure() {
    auto set = [this](const char* k, const char* v) {
        if (av_opt_set(cctx->priv_data, k, v, 0) < 0)
            DBG("VideoEncoder: av_opt_set(%s=%s) failed", k, v);
    };

    DBG("VideoEncoder: Configuring for %s", VendorName(vendor));
    const char* cq = codec == CODEC_H264 ? "23" : codec == CODEC_H265 ? "25" : "28";

    switch (vendor) {
        case GPUVendor::NVIDIA:
            set("preset", "p1"); set("tune", "ull"); set("zerolatency", "1");
            set("rc-lookahead", "0"); set("rc", "vbr"); set("multipass", "disabled");
            set("delay", "0"); set("surfaces", "4"); set("cq", cq);
            set("no-scenecut", "1");
            if (codec != CODEC_AV1) { set("forced-idr", "1"); }
            break;
        case GPUVendor::INTEL:
            set("preset", "veryfast"); set("look_ahead", "0");
            set("async_depth", "1"); set("low_power", "1"); set("global_quality", cq);
            break;
        case GPUVendor::AMD:
            set("usage", "ultralowlatency"); set("quality", "speed");
            set("rc", "vbr_latency"); set("header_insertion_mode", "gop");
            set("enforce_hrd", "0"); set("qp_i", cq); set("qp_p", cq);
            break;
        default:
            WARN("VideoEncoder: Unknown GPU vendor");
    }
}

bool VideoEncoder::TryInit(GPUVendor v, CodecType cc) {
    const char* encName = GetEncName(cc, v);
    if (!encName) return false;

    const AVCodec* enc = avcodec_find_encoder_by_name(encName);
    if (!enc) { DBG("VideoEncoder: Encoder %s not found", encName); return false; }

    LOG("VideoEncoder: Trying %s (%s on %s)", encName, CodecName(cc), VendorName(v));

    cctx = avcodec_alloc_context3(enc);
    if (!cctx) { ERR("VideoEncoder: avcodec_alloc_context3 failed"); return false; }

    if (!InitHwCtx()) { avcodec_free_context(&cctx); return false; }

    int64_t br = CalcBitrate(w, h, curFps);
    cctx->width = w;
    cctx->height = h;
    cctx->time_base = {1, curFps};
    cctx->framerate = {curFps, 1};
    cctx->bit_rate = br;
    cctx->rc_max_rate = br * 2;
    cctx->rc_buffer_size = static_cast<int>(br * 2);
    cctx->gop_size = -1;
    cctx->max_b_frames = 0;
    cctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    cctx->flags2 |= AV_CODEC_FLAG2_FAST;
    cctx->delay = 0;
    cctx->thread_count = 1;
    cctx->color_range = AVCOL_RANGE_JPEG;
    cctx->colorspace = AVCOL_SPC_BT709;
    cctx->color_primaries = AVCOL_PRI_BT709;
    cctx->color_trc = AVCOL_TRC_BT709;

    vendor = v;
    Configure();

    if (avcodec_open2(cctx, enc, nullptr) < 0) {
        ERR("VideoEncoder: avcodec_open2 failed for %s", encName);
        av_buffer_unref(&hwFrCtx);
        av_buffer_unref(&hwDev);
        avcodec_free_context(&cctx);
        vendor = GPUVendor::UNKNOWN;
        return false;
    }

    LOG("VideoEncoder: Successfully initialized %s", encName);
    return true;
}

bool VideoEncoder::DrainPackets(bool& gotKey) {
    int ret;
    while ((ret = avcodec_receive_packet(cctx, pkt)) == 0) {
        if (pkt->flags & AV_PKT_FLAG_KEY) gotKey = true;
        out.data.insert(out.data.end(), pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
    }
    return !out.data.empty();
}

VideoEncoder::VideoEncoder(int width, int height, int fps, ID3D11Device* d,
                           ID3D11DeviceContext* c, ID3D11Multithread* m, CodecType cc)
    : w(width), h(height), curFps(fps), dev(d), ctx(c), mt(m), codec(cc) {
    LOG("VideoEncoder: Creating %dx%d @ %dfps, codec: %s", w, h, fps, CodecName(cc));

    dev->AddRef();
    if (ctx) ctx->AddRef(); else dev->GetImmediateContext(&ctx);
    if (mt) mt->AddRef();

    lastKey = steady_clock::now() - KEY_INT;
    InitSync();

    for (GPUVendor v : GetVendorPriority(DetectGPU(dev))) {
        if (TryInit(v, cc)) break;
    }

    if (!cctx) throw std::runtime_error("No hardware encoder available");

    hwFr = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!hwFr || !pkt) throw std::runtime_error("Frame/packet alloc failed");

    hwFr->format = cctx->pix_fmt;
    hwFr->width = w;
    hwFr->height = h;

    LOG("Encoder: %dx%d @ %dfps, %.2f Mbps, codec: %s, GPU: %s",
        w, h, fps, CalcBitrate(w, h, fps) / 1e6, GetEncName(cc, vendor), VendorName(vendor));
}

VideoEncoder::~VideoEncoder() {
    LOG("VideoEncoder: Destroying (encoded %llu frames, %llu failed)",
        totalFrames.load(), failedFrames.load());
    av_packet_free(&pkt);
    av_frame_free(&hwFr);
    av_buffer_unref(&hwFrCtx);
    av_buffer_unref(&hwDev);
    if (cctx) avcodec_free_context(&cctx);
    if (fEvt) CloseHandle(fEvt);
    SafeRelease(fence, c4, d5, mt, ctx, dev);
}

bool VideoEncoder::UpdateFPS(int fps) {
    if (fps == curFps || fps < 1 || fps > 240) return false;
    int64_t br = CalcBitrate(w, h, fps);
    cctx->bit_rate = br;
    cctx->rc_max_rate = br * 2;
    cctx->rc_buffer_size = static_cast<int>(br * 2);
    cctx->time_base = {1, fps};
    cctx->framerate = {fps, 1};
    cctx->gop_size = -1;
    LOG("VideoEncoder: FPS updated %d -> %d (bitrate: %.2f Mbps)", curFps, fps, br / 1e6);
    curFps = fps;
    lastKey = steady_clock::now() - KEY_INT;
    return true;
}

void VideoEncoder::Flush() {
    DBG("VideoEncoder: Flushing");
    avcodec_send_frame(cctx, nullptr);
    int ret;
    while ((ret = avcodec_receive_packet(cctx, pkt)) == 0) av_packet_unref(pkt);
    avcodec_flush_buffers(cctx);
    lastKey = steady_clock::now() - KEY_INT;
}

EncodedFrame* VideoEncoder::Encode(ID3D11Texture2D* tex, int64_t ts, bool forceKey) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    out.Clear();

    if (!tex) { WARN("VideoEncoder: Null texture"); return nullptr; }

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    if (desc.Width != static_cast<UINT>(w) || desc.Height != static_cast<UINT>(h)) {
        WARN("VideoEncoder: Size mismatch: %ux%u vs %dx%d", desc.Width, desc.Height, w, h);
        return nullptr;
    }

    bool needKey = forceKey || frameNum == 0;
    const char* keyReason = nullptr;
    if (needKey) keyReason = forceKey ? "client-requested" : "first-frame";

    if (av_hwframe_get_buffer(cctx->hw_frames_ctx, hwFr, 0) < 0) {
        ERR("VideoEncoder: av_hwframe_get_buffer failed");
        failedFrames++;
        return nullptr;
    }

    uint64_t sig;
    {
        MTLock lk(mt);
        ctx->CopySubresourceRegion(reinterpret_cast<ID3D11Texture2D*>(hwFr->data[0]),
            static_cast<UINT>(reinterpret_cast<intptr_t>(hwFr->data[1])), 0, 0, 0, tex, 0, nullptr);
        ctx->Flush();
        sig = Signal();
    }

    if (!WaitGPU(sig, 16)) {
        av_frame_unref(hwFr);
        failedFrames++;
        return nullptr;
    }

    hwFr->pts = frameNum++;
    hwFr->pict_type = needKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    hwFr->flags = needKey ? (hwFr->flags | AV_FRAME_FLAG_KEY) : (hwFr->flags & ~AV_FRAME_FLAG_KEY);

    if (needKey) {
        lastKey = steady_clock::now();
        LOG("VideoEncoder: Keyframe requested (%s, frame=%d)", keyReason, frameNum);
        DBG("VideoEncoder: Encoding keyframe (frame %d)", frameNum - 1);
    }

    bool gotKey = false;
    int ret = avcodec_send_frame(cctx, hwFr);
    if (ret == AVERROR(EAGAIN)) {
        DrainPackets(gotKey);
        ret = avcodec_send_frame(cctx, hwFr);
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        ERR("VideoEncoder: avcodec_send_frame failed: %s", AvErr(ret));
        av_frame_unref(hwFr);
        failedFrames++;
        return nullptr;
    }

    DrainPackets(gotKey);
    av_frame_unref(hwFr);

    if (out.data.empty()) return nullptr;

    QueryPerformanceCounter(&t1);
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();

    out.ts = ts;
    out.encUs = ((t1.QuadPart - t0.QuadPart) * 1000000) / freq;
    out.isKey = gotKey;
    totalFrames++;
    return &out;
}
