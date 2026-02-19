#pragma once
#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"

#pragma pack(push,1)
struct PacketHeader {
    int64_t timestamp;
    uint32_t encodeTimeUs, frameId;
    uint16_t chunkIndex, totalChunks;
    uint8_t frameType;
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

    std::atomic<bool> conn{false}, needsKey{true}, fpsRecv{false}, gathered{false}, hasDesc{false};
    std::atomic<int> chRdy{0}, overflow{0};
    std::atomic<int64_t> lastPing{0};
    std::atomic<uint32_t> frmId{0};
    std::atomic<CodecType> curCodec{CODEC_AV1};

    std::string localDesc;
    std::mutex descMtx, sendMtx;
    std::condition_variable descCv;
    rtc::Configuration cfg;
    WebRTCCallbacks cb;

    static constexpr size_t VID_BUF = 262144, AUD_BUF = 131072, CHUNK = 1400, HDR_SZ = sizeof(PacketHeader);
    static constexpr size_t DATA_CHUNK = CHUNK - HDR_SZ, BUF_LOW = CHUNK * 16;
    static constexpr int NUM_CH = 5;

    std::atomic<uint64_t> totalVideoFramesSent{0}, totalAudioPacketsSent{0};
    std::atomic<uint64_t> videoSendErrors{0}, audioSendErrors{0};
    std::atomic<uint64_t> ctrlMsgsSent{0}, ctrlMsgsReceived{0};
    std::atomic<uint64_t> inputMsgsReceived{0}, micPacketsReceived{0};
    std::atomic<uint64_t> droppedVideoFrames{0}, droppedAudioPackets{0};
    std::atomic<uint64_t> connectionCount{0};
    std::atomic<int64_t> lastStatLog{0};

    std::queue<std::vector<uint8_t>> vidQ, audQ;

    bool ChOpen(const std::shared_ptr<rtc::DataChannel>& ch) const;
    bool AllOpen() const;
    bool SendCtrl(const void* d, size_t len);
    void SendHostInfo();
    void SendMonitorList();
    void SendCodecCaps();
    void SendVersion();
    void HandleCtrl(const rtc::binary& m);
    void HandleInput(const rtc::binary& m);
    void HandleMic(const rtc::binary& m);
    void OnChOpen();
    void OnChClose();
    void SetupCh(std::shared_ptr<rtc::DataChannel>& ch, bool drain, bool msg,
                 std::function<void(const rtc::binary&)> h = nullptr);
    void DrainVid();
    void DrainAud();
    void Reset();
    void SetupPC();
    bool IsStale();
    void LogStats();

public:
    WebRTCServer();
    ~WebRTCServer();

    void Init(WebRTCCallbacks c);
    std::string GetLocal();
    void SetRemote(const std::string& sdp, const std::string& type);

    bool IsStreaming() const;
    bool NeedsKey();
    bool SendCursorShape(CursorType ct);
    bool Send(const EncodedFrame& f);
    bool SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples);

    void GetStats(uint64_t& vidSent, uint64_t& vidErr, uint64_t& audSent,
                  uint64_t& audErr, uint64_t& conns);
};
