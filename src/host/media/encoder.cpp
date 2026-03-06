#include "host/media/encoder.hpp"

using std::chrono::steady_clock;

namespace {
    constexpr const char* ENC_NAMES[3][3] = {
        {"av1_nvenc", "hevc_nvenc", "h264_nvenc"},
        {"av1_qsv", "hevc_qsv", "h264_qsv"},
        {"av1_amf", "hevc_amf", "h264_amf"}
    };
    constexpr const char* SW_AV1_ENC_NAMES[] = {"libsvtav1", "libaom-av1", "librav1e"};
    constexpr const char* SW_H265_ENC_NAMES[] = {"libx265"};
    constexpr const char* SW_H264_ENC_NAMES[] = {"libx264"};

    inline int CalcEffectiveFps(int fps) {
        if (fps <= 60) return fps;
        if (fps <= 90) return 60 + ((fps - 60) * 2) / 3;
        return 80 + ((fps - 90) / 3);
    }

    inline double CalcCodecBitrateFactor(CodecType codec) {
        switch (codec) {
            case CODEC_AV1: return 0.112;
            case CODEC_H265: return 0.138;
            case CODEC_H264: return 0.165;
            default: return 0.145;
        }
    }

    inline int64_t CalcBitrate(CodecType codec, int w, int h, int fps) {
        const int effectiveFps = CalcEffectiveFps(fps);
        const int64_t pixels = static_cast<int64_t>(w) * h;
        const double factor = CalcCodecBitrateFactor(codec);
        const int64_t bitrate = static_cast<int64_t>(factor * pixels * effectiveFps);
        return std::max<int64_t>(6'000'000, bitrate);
    }

    inline int64_t CalcMaxRate(int64_t bitrate) {
        return std::max<int64_t>(bitrate, (bitrate * 115) / 100);
    }

    inline int CalcBufferSize(int64_t bitrate) {
        return static_cast<int>(std::max<int64_t>(4'000'000, bitrate / 3));
    }

    inline int CalcQualityValue(CodecType codec, int w, int h, int fps) {
        int value = codec == CODEC_H264 ? 25 : codec == CODEC_H265 ? 28 : 31;
        if (fps > 90) value += 2;
        else if (fps > 60) value += 1;
        if (static_cast<int64_t>(w) * h >= 2560LL * 1440LL) value += 1;
        return value;
    }

    inline const char* GetEncName(CodecType c, GPUVendor v) {
        return v <= GPUVendor::AMD ? ENC_NAMES[static_cast<int>(v)][static_cast<int>(c)] : nullptr;
    }

    inline AVCodecID GetCodecId(CodecType codec) {
        switch (codec) {
            case CODEC_AV1: return AV_CODEC_ID_AV1;
            case CODEC_H265: return AV_CODEC_ID_HEVC;
            case CODEC_H264: return AV_CODEC_ID_H264;
            default: return AV_CODEC_ID_NONE;
        }
    }

    inline bool IsKnownHardwareEncoder(const char* name) {
        if (!name) return false;
        return strstr(name, "_nvenc") || strstr(name, "_qsv") || strstr(name, "_amf");
    }

    const char* const* GetSoftwareEncoderNames(CodecType codec, size_t& count) {
        switch (codec) {
            case CODEC_AV1:
                count = std::size(SW_AV1_ENC_NAMES);
                return SW_AV1_ENC_NAMES;
            case CODEC_H265:
                count = std::size(SW_H265_ENC_NAMES);
                return SW_H265_ENC_NAMES;
            case CODEC_H264:
                count = std::size(SW_H264_ENC_NAMES);
                return SW_H264_ENC_NAMES;
            default:
                count = 0;
                return nullptr;
        }
    }

    const AVCodec* FindSoftwareEncoder(CodecType codec, std::string& encoderName) {
        size_t count = 0;
        if (const char* const* names = GetSoftwareEncoderNames(codec, count)) {
            for (size_t i = 0; i < count; ++i) {
                if (const AVCodec* enc = avcodec_find_encoder_by_name(names[i])) {
                    encoderName = names[i];
                    return enc;
                }
            }
        }

        if (const AVCodec* enc = avcodec_find_encoder(GetCodecId(codec))) {
            if (!IsKnownHardwareEncoder(enc->name)) {
                encoderName = enc->name ? enc->name : "software";
                return enc;
            }
        }

        encoderName.clear();
        return nullptr;
    }

    AVPixelFormat SelectSoftwarePixelFormat(const AVCodec* enc) {
        if (!enc) return AV_PIX_FMT_YUV420P;

        const void* rawFormats = nullptr;
        int formatCount = 0;
        if (avcodec_get_supported_config(nullptr, enc, AV_CODEC_CONFIG_PIX_FORMAT, 0, &rawFormats, &formatCount) < 0 ||
            !rawFormats || formatCount <= 0) {
            return AV_PIX_FMT_YUV420P;
        }

        const auto* formats = static_cast<const AVPixelFormat*>(rawFormats);

        for (const AVPixelFormat preferred : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12}) {
            for (int i = 0; i < formatCount; ++i) {
                if (formats[i] == preferred) return preferred;
            }
        }

        return AV_PIX_FMT_NONE;
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

    for (int c = 0; c <= 2; c++) {
        const CodecType codec = static_cast<CodecType>(c);

        for (GPUVendor v : GetVendorPriority(detected)) {
            if (auto* name = GetEncName(codec, v)) {
                if (avcodec_find_encoder_by_name(name)) {
                    support |= (1 << c);
                    DBG("VideoEncoder: Found hardware encoder %s for %s", name, CodecName(codec));
                    break;
                }
            }
        }

        if (!(support & (1 << c))) {
            std::string encoderName;
            if (FindSoftwareEncoder(codec, encoderName)) {
                support |= (1 << c);
                DBG("VideoEncoder: Found software encoder %s for %s", encoderName.c_str(), CodecName(codec));
            }
        }
    }

    LOG("VideoEncoder: Codec support: AV1=%d H265=%d H264=%d",
        (support&1)?1:0, (support&2)?1:0, (support&4)?1:0);
    return support;
}

uint8_t VideoEncoder::ProbeHardwareSupport(ID3D11Device* device) {
    uint8_t support = 0;
    GPUVendor detected = DetectGPU(device);

    for (int c = 0; c <= 2; c++) {
        const CodecType codec = static_cast<CodecType>(c);
        for (GPUVendor v : GetVendorPriority(detected)) {
            if (auto* name = GetEncName(codec, v)) {
                if (avcodec_find_encoder_by_name(name)) {
                    support |= (1 << c);
                    break;
                }
            }
        }
    }

    LOG("VideoEncoder: Hardware codec support: AV1=%d H265=%d H264=%d",
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

void VideoEncoder::Configure() {
    auto set = [this](const char* k, const char* v) {
        if (av_opt_set(cctx->priv_data, k, v, 0) < 0)
            DBG("VideoEncoder: av_opt_set(%s=%s) failed", k, v);
    };

    if (!usingHardware) {
        DBG("VideoEncoder: Configuring software encoder %s", activeEncoderName.c_str());

        if (activeEncoderName == "libx264") {
            set("preset", "ultrafast");
            set("tune", "zerolatency");
            set("x264-params", "scenecut=0:open-gop=0");
            set("annexb", "1");
        } else if (activeEncoderName == "libx265") {
            set("preset", "ultrafast");
            set("tune", "zerolatency");
            set("x265-params", "scenecut=0:open-gop=0:repeat-headers=1");
            set("annexb", "1");
        } else if (activeEncoderName == "libsvtav1") {
            set("preset", "12");
            set("tune", "0");
        } else if (activeEncoderName == "libaom-av1") {
            set("usage", "realtime");
            set("cpu-used", "8");
            set("lag-in-frames", "0");
            set("row-mt", "1");
        } else if (activeEncoderName == "librav1e") {
            set("speed", "10");
        }
        return;
    }

    DBG("VideoEncoder: Configuring for %s", VendorName(vendor));
    const std::string qualityValue = std::to_string(CalcQualityValue(codec, w, h, curFps));
    const char* quality = qualityValue.c_str();

    switch (vendor) {
        case GPUVendor::NVIDIA:
            set("preset", "p2"); set("tune", "ull"); set("zerolatency", "1");
            set("rc-lookahead", "0"); set("rc", "vbr"); set("multipass", "disabled");
            set("delay", "0"); set("surfaces", "3"); set("cq", quality);
            set("no-scenecut", "1");
            if (codec != CODEC_AV1) { set("forced-idr", "1"); }
            break;
        case GPUVendor::INTEL:
            set("preset", "veryfast"); set("look_ahead", "0");
            set("async_depth", "1"); set("low_power", "1"); set("global_quality", quality);
            set("forced_idr", "1"); set("adaptive_i", "0"); set("adaptive_b", "0");
            break;
        case GPUVendor::AMD:
            set("usage", "ultralowlatency"); set("quality", "balanced");
            set("rc", "vbr_latency"); set("header_insertion_mode", "gop");
            set("enforce_hrd", "0"); set("qp_i", quality); set("qp_p", quality);
            set("forced_idr", "1");
            break;
        default:
            WARN("VideoEncoder: Unknown GPU vendor");
    }
}

bool VideoEncoder::InitSwFrame(const AVCodec* enc) {
    swPixFmt = SelectSoftwarePixelFormat(enc);
    if (swPixFmt == AV_PIX_FMT_NONE) {
        ERR("VideoEncoder: No supported 8-bit software pixel format for %s", enc && enc->name ? enc->name : "unknown");
        return false;
    }

    swFr = av_frame_alloc();
    if (!swFr) {
        ERR("VideoEncoder: av_frame_alloc failed for software frame");
        return false;
    }

    swFr->format = swPixFmt;
    swFr->width = w;
    swFr->height = h;
    if (av_frame_get_buffer(swFr, 32) < 0) {
        ERR("VideoEncoder: av_frame_get_buffer failed for software frame");
        av_frame_free(&swFr);
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(dev->CreateTexture2D(&td, nullptr, &stagingTex))) {
        ERR("VideoEncoder: CreateTexture2D failed for software staging texture");
        av_frame_free(&swFr);
        return false;
    }

    swsCtx = sws_getCachedContext(
        nullptr,
        w,
        h,
        AV_PIX_FMT_BGRA,
        w,
        h,
        swPixFmt,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (!swsCtx) {
        ERR("VideoEncoder: sws_getCachedContext failed");
        SafeRelease(stagingTex);
        av_frame_free(&swFr);
        return false;
    }

    return true;
}

bool VideoEncoder::UploadSoftwareFrame(ID3D11Texture2D* tex, AVFrame* frame) {
    if (!tex || !frame || !stagingTex || !swsCtx) return false;

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        ERR("VideoEncoder: Unsupported software input texture format: %d", static_cast<int>(desc.Format));
        return false;
    }

    int writableRet = av_frame_make_writable(frame);
    if (writableRet < 0) {
        ERR("VideoEncoder: av_frame_make_writable failed: %s", AvErr(writableRet));
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    {
        MTLock lk(mt);
        ctx->CopyResource(stagingTex, tex);
        HRESULT hr = ctx->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            ERR("VideoEncoder: Failed to map staging texture for software encode (hr=0x%08X)", hr);
            return false;
        }
    }

    const uint8_t* srcData[4] = {static_cast<const uint8_t*>(mapped.pData), nullptr, nullptr, nullptr};
    const int srcLinesize[4] = {static_cast<int>(mapped.RowPitch), 0, 0, 0};
    const int scaled = sws_scale(swsCtx, srcData, srcLinesize, 0, h, frame->data, frame->linesize);

    {
        MTLock lk(mt);
        ctx->Unmap(stagingTex, 0);
    }

    if (scaled != h) {
        ERR("VideoEncoder: sws_scale returned %d (expected %d)", scaled, h);
        return false;
    }

    return true;
}

bool VideoEncoder::TryInitHardware(GPUVendor v, CodecType cc) {
    const char* encName = GetEncName(cc, v);
    if (!encName) return false;

    const AVCodec* enc = avcodec_find_encoder_by_name(encName);
    if (!enc) { DBG("VideoEncoder: Encoder %s not found", encName); return false; }

    LOG("VideoEncoder: Trying %s (%s on %s)", encName, CodecName(cc), VendorName(v));

    cctx = avcodec_alloc_context3(enc);
    if (!cctx) { ERR("VideoEncoder: avcodec_alloc_context3 failed"); return false; }

    usingHardware = true;
    activeEncoderName = encName;
    if (!InitHwCtx()) { avcodec_free_context(&cctx); return false; }

    int64_t br = CalcBitrate(codec, w, h, curFps);
    cctx->width = w;
    cctx->height = h;
    cctx->time_base = {1, curFps};
    cctx->framerate = {curFps, 1};
    cctx->bit_rate = br;
    cctx->rc_max_rate = CalcMaxRate(br);
    cctx->rc_buffer_size = CalcBufferSize(br);
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
        activeEncoderName.clear();
        usingHardware = false;
        return false;
    }

    LOG("VideoEncoder: Successfully initialized %s", encName);
    return true;
}

bool VideoEncoder::TryInitSoftware(CodecType cc) {
    std::string encoderName;
    const AVCodec* enc = FindSoftwareEncoder(cc, encoderName);
    if (!enc) return false;

    LOG("VideoEncoder: Trying software encoder %s (%s)", encoderName.c_str(), CodecName(cc));

    cctx = avcodec_alloc_context3(enc);
    if (!cctx) {
        ERR("VideoEncoder: avcodec_alloc_context3 failed for %s", encoderName.c_str());
        return false;
    }

    usingHardware = false;
    vendor = GPUVendor::UNKNOWN;
    activeEncoderName = encoderName;

    int64_t br = CalcBitrate(codec, w, h, curFps);
    cctx->width = w;
    cctx->height = h;
    cctx->time_base = {1, curFps};
    cctx->framerate = {curFps, 1};
    cctx->bit_rate = br;
    cctx->rc_max_rate = CalcMaxRate(br);
    cctx->rc_buffer_size = CalcBufferSize(br);
    cctx->gop_size = -1;
    cctx->max_b_frames = 0;
    cctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    cctx->flags2 |= AV_CODEC_FLAG2_FAST;
    cctx->delay = 0;
    cctx->thread_count = 0;
    cctx->color_range = AVCOL_RANGE_JPEG;
    cctx->colorspace = AVCOL_SPC_BT709;
    cctx->color_primaries = AVCOL_PRI_BT709;
    cctx->color_trc = AVCOL_TRC_BT709;

    if (!InitSwFrame(enc)) {
        avcodec_free_context(&cctx);
        activeEncoderName.clear();
        return false;
    }

    cctx->pix_fmt = swPixFmt;
    Configure();

    if (avcodec_open2(cctx, enc, nullptr) < 0) {
        ERR("VideoEncoder: avcodec_open2 failed for software encoder %s", encoderName.c_str());
        sws_freeContext(swsCtx);
        swsCtx = nullptr;
        SafeRelease(stagingTex);
        av_frame_free(&swFr);
        avcodec_free_context(&cctx);
        activeEncoderName.clear();
        return false;
    }

    LOG("VideoEncoder: Successfully initialized software encoder %s", encoderName.c_str());
    return true;
}

bool VideoEncoder::DrainPackets(bool& gotKey) {
    int ret;
    int packetCount = 0;
    while ((ret = avcodec_receive_packet(cctx, pkt)) == 0) {
        packetCount++;
        if (pkt->flags & AV_PKT_FLAG_KEY) gotKey = true;
        if (!pkt->data || pkt->size <= 0) {
            ERR("VideoEncoder: DrainPackets got empty/null packet (pkt #%d, size=%d)", packetCount, pkt->size);
            av_packet_unref(pkt);
            continue;
        }
        DBG("VideoEncoder: DrainPackets pkt #%d size=%d key=%d pts=%lld dts=%lld",
            packetCount, pkt->size, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0, pkt->pts, pkt->dts);
        out.data.insert(out.data.end(), pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        ERR("VideoEncoder: DrainPackets unexpected error: %s (after %d packets)", AvErr(ret), packetCount);
    }
    if (packetCount == 0) {
        DBG("VideoEncoder: DrainPackets produced no packets (ret=%s)", AvErr(ret));
    }
    return !out.data.empty();
}

VideoEncoder::VideoEncoder(int width, int height, int fps, ID3D11Device* d,
                           ID3D11DeviceContext* c, ID3D11Multithread* m, CodecType cc, bool preferSoftware)
    : w(width), h(height), curFps(fps), dev(d), ctx(c), mt(m), codec(cc) {
    LOG("VideoEncoder: Creating %dx%d @ %dfps, codec: %s, preferSoftware=%d", w, h, fps, CodecName(cc), preferSoftware ? 1 : 0);

    dev->AddRef();
    if (ctx) ctx->AddRef(); else dev->GetImmediateContext(&ctx);
    if (mt) mt->AddRef();

    lastKey = steady_clock::now() - KEY_INT;
    sync.Init(dev, ctx);

    if (!preferSoftware) {
        for (GPUVendor v : GetVendorPriority(DetectGPU(dev))) {
            if (TryInitHardware(v, cc)) break;
        }
    }

    if (!cctx) TryInitSoftware(cc);

    if (!cctx && preferSoftware) {
        for (GPUVendor v : GetVendorPriority(DetectGPU(dev))) {
            if (TryInitHardware(v, cc)) break;
        }
    }

    if (!cctx) throw std::runtime_error("No encoder available for requested codec");

    pkt = av_packet_alloc();
    if (!pkt) throw std::runtime_error("Frame/packet alloc failed");

    if (usingHardware) {
        hwFr = av_frame_alloc();
        if (!hwFr) throw std::runtime_error("Frame/packet alloc failed");
        hwFr->format = cctx->pix_fmt;
        hwFr->width = w;
        hwFr->height = h;
    }

    LOG("Encoder: %dx%d @ %dfps, %.2f Mbps, codec: %s, encoder: %s, backend: %s",
        w, h, fps, CalcBitrate(cc, w, h, fps) / 1e6, CodecName(cc),
        activeEncoderName.empty() ? "unknown" : activeEncoderName.c_str(),
        usingHardware ? VendorName(vendor) : "Software");
}

VideoEncoder::~VideoEncoder() {
    LOG("VideoEncoder: Destroying (encoded %llu frames, %llu failed)",
        totalFrames.load(), failedFrames.load());
    av_packet_free(&pkt);
    av_frame_free(&hwFr);
    av_frame_free(&swFr);
    sws_freeContext(swsCtx);
    av_buffer_unref(&hwFrCtx);
    av_buffer_unref(&hwDev);
    if (cctx) avcodec_free_context(&cctx);
    SafeRelease(stagingTex);
    SafeRelease(mt, ctx, dev);
}

bool VideoEncoder::UpdateFPS(int fps) {
    if (fps == curFps || fps < 1 || fps > 240) return false;
    int64_t br = CalcBitrate(codec, w, h, fps);
    cctx->bit_rate = br;
    cctx->rc_max_rate = CalcMaxRate(br);
    cctx->rc_buffer_size = CalcBufferSize(br);
    cctx->time_base = {1, fps};
    cctx->framerate = {fps, 1};
    cctx->gop_size = -1;
    LOG("VideoEncoder: FPS updated %d -> %d (bitrate: %.2f Mbps)", curFps, fps, br / 1e6);
    curFps = fps;
    lastKey = steady_clock::now() - KEY_INT;
    return true;
}

void VideoEncoder::Flush() {
    LOG("VideoEncoder: Flushing encoder (frame=%d, total=%llu, failed=%llu)",
        frameNum, totalFrames.load(), failedFrames.load());
    int flushRet = avcodec_send_frame(cctx, nullptr);
    if (flushRet < 0 && flushRet != AVERROR_EOF) {
        WARN("VideoEncoder: Flush send_frame error: %s", AvErr(flushRet));
    }
    int ret;
    int flushedPackets = 0;
    while ((ret = avcodec_receive_packet(cctx, pkt)) == 0) {
        flushedPackets++;
        av_packet_unref(pkt);
    }
    DBG("VideoEncoder: Flush drained %d packets", flushedPackets);
    avcodec_flush_buffers(cctx);
    lastKey = steady_clock::now() - KEY_INT;
    LOG("VideoEncoder: Flush complete");
}

EncodedFrame* VideoEncoder::Encode(ID3D11Texture2D* tex, int64_t ts, int64_t sourceTs, bool forceKey) {
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

    const bool needKey = forceKey;
    const char* keyReason = needKey ? "client-requested" : nullptr;
    const int64_t encodeStartTs = GetTimestamp();

    DBG("VideoEncoder: Encode start frame=%d ts=%lld sourceTs=%lld forceKey=%d needKey=%d",
        frameNum, ts, sourceTs, forceKey ? 1 : 0, needKey ? 1 : 0);

    AVFrame* encodeFrame = nullptr;
    if (usingHardware) {
        int hwBufRet = av_hwframe_get_buffer(cctx->hw_frames_ctx, hwFr, 0);
        if (hwBufRet < 0) {
            ERR("VideoEncoder: av_hwframe_get_buffer failed: %s (frame=%d)", AvErr(hwBufRet), frameNum);
            failedFrames++;
            return nullptr;
        }

        uint64_t sig;
        {
            MTLock lk(mt);
            ctx->CopySubresourceRegion(reinterpret_cast<ID3D11Texture2D*>(hwFr->data[0]),
                static_cast<UINT>(reinterpret_cast<intptr_t>(hwFr->data[1])), 0, 0, 0, tex, 0, nullptr);
            sig = sync.Signal();
        }

        if (!sync.Wait(sig, ctx, mt, 16)) {
            WARN("VideoEncoder: GPU sync timeout (frame=%d, sig=%llu, ts=%lld) - frame dropped", frameNum, sig, ts);
            av_frame_unref(hwFr);
            failedFrames++;
            return nullptr;
        }

        encodeFrame = hwFr;
    } else {
        if (!UploadSoftwareFrame(tex, swFr)) {
            failedFrames++;
            return nullptr;
        }
        encodeFrame = swFr;
    }

    encodeFrame->pts = frameNum++;
    encodeFrame->pict_type = needKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    encodeFrame->flags = needKey ? (encodeFrame->flags | AV_FRAME_FLAG_KEY) : (encodeFrame->flags & ~AV_FRAME_FLAG_KEY);

    if (needKey) {
        LOG("VideoEncoder: Keyframe requested (%s, frame=%d)", keyReason, frameNum);
        DBG("VideoEncoder: Encoding keyframe (frame %d)", frameNum - 1);
    }

    bool gotKey = false;
    int ret = avcodec_send_frame(cctx, encodeFrame);
    if (ret == AVERROR(EAGAIN)) {
        DrainPackets(gotKey);
        ret = avcodec_send_frame(cctx, encodeFrame);
    }

    if (ret < 0 && ret != AVERROR_EOF) {
        ERR("VideoEncoder: avcodec_send_frame failed: %s", AvErr(ret));
        av_frame_unref(encodeFrame);
        failedFrames++;
        return nullptr;
    }

    DrainPackets(gotKey);
    av_frame_unref(encodeFrame);

    if (out.data.empty()) {
        WARN("VideoEncoder: Encode produced empty output (frame=%d, ts=%lld, needKey=%d) - frame dropped", frameNum - 1, ts, needKey ? 1 : 0);
        failedFrames++;
        return nullptr;
    }

    QueryPerformanceCounter(&t1);
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();

    out.ts = ts;
    out.sourceTs = sourceTs > 0 ? sourceTs : ts;
    out.encodeEndTs = GetTimestamp();
    out.encUs = ((t1.QuadPart - t0.QuadPart) * 1000000) / freq;
    out.isKey = gotKey;
    totalFrames++;

    if (gotKey) {
        lastKey = steady_clock::now();
    }

    if (needKey && !gotKey) {
        WARN("VideoEncoder: Requested keyframe but encoder did not produce one (frame=%d, ts=%lld, size=%zu)",
            frameNum - 1, ts, out.data.size());
    }

    // Stream corruption check: verify frame starts with valid NAL/OBU header
    if (out.data.size() >= 4) {
        const uint8_t* d = out.data.data();
        bool validStart = false;
        if (codec == CODEC_H264 || codec == CODEC_H265) {
            // Check for Annex B start code (0x00000001 or 0x000001)
            validStart = (d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x00 && d[3] == 0x01) ||
                         (d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x01);
        } else if (codec == CODEC_AV1) {
            // AV1 OBU: check OBU type (top 4 bits of first byte should be 1-8 for valid types)
            uint8_t obuType = (d[0] >> 3) & 0x0F;
            validStart = (obuType >= 1 && obuType <= 8);
        }
        if (!validStart) {
            ERR("VideoEncoder: STREAM CORRUPTION - invalid bitstream header [%02X %02X %02X %02X] "
                "(frame=%d, key=%d, size=%zu, codec=%s)",
                d[0], d[1], d[2], d[3], frameNum - 1, gotKey ? 1 : 0, out.data.size(), CodecName(codec));
        }
    } else if (out.data.size() > 0) {
        WARN("VideoEncoder: Suspiciously small encoded frame: %zu bytes (frame=%d, key=%d)",
            out.data.size(), frameNum - 1, gotKey ? 1 : 0);
    }

    DBG("VideoEncoder: Encoded frame=%d ts=%lld sourceTs=%lld encodeStartTs=%lld encodeEndTs=%lld key=%d size=%zu encUs=%lld total=%llu failed=%llu",
        frameNum - 1, ts, out.sourceTs, encodeStartTs, out.encodeEndTs, gotKey ? 1 : 0, out.data.size(), out.encUs,
        totalFrames.load(), failedFrames.load());

    return &out;
}
