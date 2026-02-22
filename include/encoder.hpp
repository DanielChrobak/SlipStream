#pragma once
#include "common.hpp"

enum class GPUVendor : uint8_t { NVIDIA=0, INTEL=1, AMD=2, UNKNOWN=255 };

struct EncodedFrame {
    std::vector<uint8_t> data;
    int64_t ts=0, encUs=0;
    bool isKey=false;
    void Clear() { data.clear(); ts = encUs = 0; isKey = false; }
};

inline const char* AvErr(int err) {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    return av_strerror(err, buf, sizeof(buf)), buf;
}

class VideoEncoder {
    AVCodecContext* cctx=nullptr;
    AVFrame* hwFr=nullptr;
    AVPacket* pkt=nullptr;
    AVBufferRef* hwDev=nullptr, *hwFrCtx=nullptr;
    ID3D11Device* dev=nullptr;
    ID3D11DeviceContext* ctx=nullptr;
    ID3D11Multithread* mt=nullptr;
    ID3D11Device5* d5=nullptr;
    ID3D11DeviceContext4* c4=nullptr;
    ID3D11Fence* fence=nullptr;
    HANDLE fEvt=nullptr;

    uint64_t fVal=0, lastSig=0;
    bool useFence=false;
    int w, h, frameNum=0, curFps;
    CodecType codec;
    GPUVendor vendor=GPUVendor::UNKNOWN;
    steady_clock::time_point lastKey;
    EncodedFrame out;
    std::atomic<uint64_t> totalFrames{0}, failedFrames{0};

    static constexpr auto KEY_INT = 2000ms;

    bool InitHwCtx();
    void InitSync();
    uint64_t Signal();
    bool WaitGPU(uint64_t v, DWORD ms=16);
    void Configure();
    bool TryInit(GPUVendor v, CodecType cc);
    bool DrainPackets(bool& gotKey);

public:
    [[nodiscard]] static uint8_t ProbeSupport(ID3D11Device* d);
    [[nodiscard]] static GPUVendor DetectGPU(ID3D11Device* d);
    [[nodiscard]] static const char* VendorName(GPUVendor v);
    [[nodiscard]] static const char* CodecName(CodecType c);

    VideoEncoder(int w, int h, int fps, ID3D11Device* d, ID3D11DeviceContext* c,
                 ID3D11Multithread* m, CodecType cc=CODEC_AV1);
    ~VideoEncoder();

    [[nodiscard]] GPUVendor GetVendor() const { return vendor; }
    bool UpdateFPS(int fps);
    void Flush();
    [[nodiscard]] bool IsEncodeComplete() const { return !useFence || !fence || fence->GetCompletedValue() >= lastSig; }
    [[nodiscard]] EncodedFrame* Encode(ID3D11Texture2D* tex, int64_t ts, bool forceKey=false);
};
