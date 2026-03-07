#pragma once
#include "host/core/common.hpp"
#include "host/media/encoder.hpp"
#include "host/io/input.hpp"
#include <array>
#include <unordered_set>

#pragma pack(push,1)
struct PacketHeader {
    int64_t timestamp;
    int64_t sourceTimestamp;
    int64_t encodeEndTimestamp;
    int64_t enqueueTimestamp;
    uint32_t encodeTimeUs, frameId;
    uint32_t frameSize;
    uint16_t chunkIndex, totalChunks;
    uint16_t chunkBytes;
    uint16_t dataChunkSize;
    uint8_t frameType;
    uint8_t packetType;
    uint8_t fecGroupSize;
};
#pragma pack(pop)

struct WebRTCCallbacks {
    InputHandler* input = nullptr;
    std::function<void(int, uint8_t)> onFpsChange;
    std::function<int()> getHostFps, getMonitor;
    std::function<bool(int)> onMonitorChange;
    std::function<void()> onDisconnect, onConnected;
    std::function<bool(CodecType)> onCodecChange;
    std::function<bool(bool)> onSoftwareEncodeChange;
    std::function<CodecType()> getCodec;
    std::function<uint8_t()> getCodecCaps;
    std::function<uint8_t()> getHostInfoFlags;
    std::function<std::string()> getEncoderName;
    std::function<std::string()> getClipboard;
    std::function<bool(const std::string&)> setClipboard;
    std::function<void(bool)> onCursorCapture, onAudioEnable, onMicEnable;
    std::function<void(const uint8_t*, size_t)> onMicData;
};

class WebRTCServer {
    struct MicFecGroupState {
        uint8_t groupSize = 4;
        std::unordered_map<uint32_t, std::vector<uint8_t>> dataPackets;
        std::vector<uint8_t> fecPayload;
        bool hasFec = false;
        int64_t updatedMs = 0;
    };

    std::shared_ptr<rtc::PeerConnection> peerConnection_;
    std::shared_ptr<rtc::DataChannel> controlDataChannel_, videoDataChannel_, audioDataChannel_, inputDataChannel_, micDataChannel_;
    std::mutex channelMutex_;

    std::atomic<bool> conn{false}, needsKey{true}, fpsRecv{false}, gathered{false}, hasDesc{false};
    std::atomic<int> chRdy{0}, overflow{0};
    std::atomic<int64_t> lastPing{0}, lastStatLog{0}, lastKeyReqMs{0};
    std::atomic<uint32_t> frmId{0}, audioPktId{0};
    std::atomic<CodecType> curCodec{CODEC_AV1};

    std::string localDescription_;
    std::mutex descriptionMutex_, sendMutex_, micFecMutex_;
    std::condition_variable descriptionCv_;
    rtc::Configuration rtcConfig_;
    WebRTCCallbacks callbacks_;
    std::queue<std::vector<uint8_t>> videoPacketQueue_, audioPacketQueue_;
    std::unordered_map<uint32_t, MicFecGroupState> micFecGroups_;
    std::unordered_set<uint32_t> micSeenPacketIds_;

    static constexpr size_t VID_BUF=262144, AUD_BUF=131072, CHUNK=1400;
    static constexpr size_t HDR_SZ=sizeof(PacketHeader), DATA_CHUNK=CHUNK-HDR_SZ, BUF_LOW=CHUNK*16;
    static constexpr int NUM_CH=5;
    static constexpr uint8_t AUDIO_FEC_GROUP_SIZE = 10;
    static constexpr uint8_t MIC_FEC_GROUP_SIZE = 10;

    std::array<std::vector<uint8_t>, AUDIO_FEC_GROUP_SIZE> audioFecPackets_;
    uint8_t audioFecCount_ = 0;
    uint32_t audioFecGroupStart_ = 0;

    std::atomic<uint64_t> videoSent{0}, audioSent{0}, videoErr{0}, audioErr{0};
    std::atomic<uint64_t> ctrlSent{0}, ctrlRecv{0}, inputRecv{0}, micRecv{0}, connCount{0};
    std::atomic<uint64_t> peerEpoch{0};
    std::atomic<bool> videoDrainActive_{false}, audioDrainActive_{false};

    bool SendCtrl(const void* d, size_t len);
    void SendHostInfo();
    void SendEncoderInfo();
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
    [[nodiscard]] uint16_t GetIcePortRangeBegin() const { return rtcConfig_.portRangeBegin; }
    [[nodiscard]] uint16_t GetIcePortRangeEnd() const { return rtcConfig_.portRangeEnd; }
    [[nodiscard]] bool IsIceTcpEnabled() const { return rtcConfig_.enableIceTcp; }

    [[nodiscard]] bool IsStreaming() const { return conn && fpsRecv && chRdy == NUM_CH; }
    [[nodiscard]] bool NeedsKey() { return needsKey.exchange(false); }
    [[nodiscard]] bool IsCongested() const;
    [[nodiscard]] bool SendCursorShape(CursorType ct);
    [[nodiscard]] bool Send(const EncodedFrame& f);
    [[nodiscard]] bool SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples);
    void GetStats(uint64_t& vS, uint64_t& vE, uint64_t& aS, uint64_t& aE, uint64_t& c);
};
