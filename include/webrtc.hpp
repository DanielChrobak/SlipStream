#pragma once
#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"

#pragma pack(push,1)
struct PacketHeader {
    int64_t timestamp;
    uint32_t encodeTimeUs, frameId;
    uint32_t frameSize;
    uint16_t chunkIndex, totalChunks;
    uint16_t chunkBytes;
    uint16_t dataChunkSize;
    uint8_t frameType;
    uint8_t packetType;
    uint8_t fecGroupSize;
};
struct AudioPacketHeader {
    uint32_t magic;
    int64_t timestamp;
    uint16_t samples, dataLength;
};
#pragma pack(pop)

struct WebRTCCallbacks {
    InputHandler* input = nullptr;
    std::function<void(int, uint8_t)> onFpsChange;
    std::function<int()> getHostFps, getMonitor;
    std::function<bool(int)> onMonitorChange;
    std::function<void()> onDisconnect, onConnected;
    std::function<bool(CodecType)> onCodecChange;
    std::function<CodecType()> getCodec;
    std::function<uint8_t()> getCodecCaps;
    std::function<std::string()> getClipboard;
    std::function<bool(const std::string&)> setClipboard;
    std::function<void(bool)> onCursorCapture, onAudioEnable, onMicEnable;
    std::function<void(const uint8_t*, size_t)> onMicData;
};

class WebRTCServer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dcCtrl, dcVid, dcAud, dcIn, dcMic;
    std::mutex chMtx;

    std::atomic<bool> conn{false}, needsKey{true}, fpsRecv{false}, gathered{false}, hasDesc{false};
    std::atomic<int> chRdy{0}, overflow{0};
    std::atomic<int64_t> lastPing{0}, lastStatLog{0}, lastKeyReqMs{0};
    std::atomic<uint32_t> frmId{0};
    std::atomic<CodecType> curCodec{CODEC_AV1};

    std::string localDesc;
    std::mutex descMtx, sendMtx;
    std::condition_variable descCv;
    rtc::Configuration cfg;
    WebRTCCallbacks cb;
    std::queue<std::vector<uint8_t>> vidQ, audQ;

    static constexpr size_t VID_BUF=262144, AUD_BUF=131072, CHUNK=1400;
    static constexpr size_t HDR_SZ=sizeof(PacketHeader), DATA_CHUNK=CHUNK-HDR_SZ, BUF_LOW=CHUNK*16;
    static constexpr int NUM_CH=5;

    std::atomic<uint64_t> videoSent{0}, audioSent{0}, videoErr{0}, audioErr{0};
    std::atomic<uint64_t> ctrlSent{0}, ctrlRecv{0}, inputRecv{0}, micRecv{0}, connCount{0};
    std::atomic<uint64_t> peerEpoch{0};

    bool SendCtrl(const void* d, size_t len);
    void SendHostInfo();
    void SendMonitorList();
    void SendCodecCaps();
    void SendVersion();
    void HandleCtrl(const rtc::binary& m);
    void HandleInput(const rtc::binary& m);
    void HandleMic(const rtc::binary& m);
    void OnChannelOpen(const std::string& label, uint64_t epoch);
    void OnChannelClose(const std::string& label, uint64_t epoch);
    void SetupChannel(std::shared_ptr<rtc::DataChannel>& ch, bool drain,
                      std::function<void(const rtc::binary&)> handler = nullptr,
                      uint64_t epoch = 0);
    void DrainVideo();
    void DrainAudio();
    void Reset();
    void SetupPeerConnection();
    bool IsStale();
    void LogStats();

public:
    WebRTCServer();
    ~WebRTCServer();

    void Init(WebRTCCallbacks c);
    void Shutdown();
    std::string GetLocal();
    void SetRemote(const std::string& sdp, const std::string& type);

    [[nodiscard]] bool IsStreaming() const { return conn && fpsRecv && chRdy == NUM_CH; }
    [[nodiscard]] bool NeedsKey() { return needsKey.exchange(false); }
    [[nodiscard]] bool SendCursorShape(CursorType ct);
    [[nodiscard]] bool Send(const EncodedFrame& f);
    [[nodiscard]] bool SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples);
    void GetStats(uint64_t& vS, uint64_t& vE, uint64_t& aS, uint64_t& aE, uint64_t& c);
};
