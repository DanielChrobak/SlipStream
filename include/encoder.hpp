#pragma once
#include "common.hpp"

enum class GPUVendor : uint8_t { NVIDIA=0, INTEL=1, AMD=2, UNKNOWN=255 };

struct EncodedFrame {
    std::vector<uint8_t> data;
    int64_t ts=0, encUs=0;
    bool isKey=false;
    void Clear();
};

const char* AvErr(int err);

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
    int w, h, frameNum=0, curFps=60;
    CodecType codec=CODEC_AV1;
    GPUVendor vendor=GPUVendor::UNKNOWN;

    steady_clock::time_point lastKey;
    static constexpr auto KEY_INT = 2000ms;
    EncodedFrame out;
    std::atomic<uint64_t> totalFrames{0}, failedFrames{0};

    static int64_t CalcBR(int w, int h, int fps);
    static const char* EncName(CodecType c, GPUVendor v);
    static const char* VendorName(GPUVendor v);
    static const char* CodecName(CodecType c);
    static GPUVendor DetectGPUVendor(ID3D11Device* device);
    static std::vector<GPUVendor> GetVendorPriorityList(GPUVendor detected);

public:
    static uint8_t ProbeEncoderSupport(ID3D11Device* device);

private:
    bool InitHwCtx(const AVCodec* c);
    void InitSync();
    uint64_t Signal();
    bool WaitGPU(uint64_t v, DWORD ms=16);
    void Configure();
    bool TryInitEncoder(GPUVendor v, CodecType cc);
    bool DrainPackets(bool& gotKey);

public:
    VideoEncoder(int width, int height, int fps, ID3D11Device* d, ID3D11DeviceContext* c,
                 ID3D11Multithread* m, CodecType cc=CODEC_AV1);
    ~VideoEncoder();

    GPUVendor GetVendor() const;
    bool UpdateFPS(int fps);
    void Flush();
    bool IsEncodeComplete() const;
    EncodedFrame* Encode(ID3D11Texture2D* tex, int64_t ts, bool forceKey=false);
};
