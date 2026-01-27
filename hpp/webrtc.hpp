#pragma once

#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"

#pragma pack(push, 1)
struct PacketHeader { int64_t timestamp; uint32_t encodeTimeUs, frameId; uint16_t chunkIndex, totalChunks; uint8_t frameType; };
struct AudioPacketHeader { uint32_t magic; int64_t timestamp; uint16_t samples, dataLength; };
#pragma pack(pop)

struct WebRTCCallbacks {
    InputHandler* inputHandler = nullptr;
    std::function<void(int, uint8_t)> onFpsChange;
    std::function<int()> getHostFps;
    std::function<bool(int)> onMonitorChange;
    std::function<int()> getCurrentMonitor;
    std::function<void()> onDisconnect;
    std::function<void()> onConnected;
    std::function<bool(CodecType)> onCodecChange;
    std::function<CodecType()> getCodec;
};

class WebRTCServer {
    std::shared_ptr<rtc::PeerConnection> peerConnection;
    std::shared_ptr<rtc::DataChannel> dataChannel;

    std::atomic<bool> connected{false}, needsKeyframe{true}, fpsReceived{false};
    std::atomic<bool> gatheringComplete{false}, authenticated{false}, hasLocalDescription{false};

    std::string localDescription;
    std::mutex descMutex;
    std::condition_variable descCondition;
    rtc::Configuration rtcConfig;

    static constexpr size_t BUFFER_THRESHOLD = 262144, CHUNK_SIZE = 1400;
    static constexpr size_t HEADER_SIZE = sizeof(PacketHeader), DATA_CHUNK_SIZE = CHUNK_SIZE - HEADER_SIZE;
    static constexpr size_t BUFFER_LOW_THRESHOLD = CHUNK_SIZE * 16, MAX_QUEUE_FRAMES = 3;

    std::vector<uint8_t> audioBuffer;

    std::atomic<uint64_t> sentCount{0}, byteCount{0}, dropCount{0}, audioSentCount{0};
    std::atomic<uint32_t> frameId{0};
    std::atomic<int> currentFps{60}, overflowCount{0};
    std::atomic<int64_t> lastPingTime{0};
    std::atomic<bool> pingTimeout{false};
    std::atomic<int> candidateCount{0};

    struct QueuedPacket { std::vector<uint8_t> data; uint32_t frameId; bool isLastChunk; };
    std::queue<QueuedPacket> sendQueue;
    std::mutex sendQueueMutex;

    std::atomic<CodecType> currentCodec{CODEC_H264};
    WebRTCCallbacks cb;
    PacedSendMetrics pacedMetrics;

    bool SafeSend(const void* data, size_t len) {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return false;
        try { ch->send(reinterpret_cast<const std::byte*>(data), len); return true; }
        catch (...) { return false; }
    }

    void DrainSendQueue() {
        std::unique_lock<std::mutex> lock(sendQueueMutex);
        auto ch = dataChannel;
        if (!ch || !ch->isOpen() || sendQueue.empty()) return;

        size_t bytesSent = 0, packetsSent = 0;
        size_t startQueueSize = sendQueue.size();
        while (!sendQueue.empty()) {
            if (ch->bufferedAmount() > BUFFER_THRESHOLD) break;
            QueuedPacket& pkt = sendQueue.front();
            if (!SafeSend(pkt.data.data(), pkt.data.size())) { overflowCount++; dropCount++; needsKeyframe = true; sendQueue.pop(); continue; }
            bytesSent += pkt.data.size();
            packetsSent++;
            sendQueue.pop();
        }
        if (bytesSent > 0) byteCount += bytesSent;
        if (packetsSent > 0) pacedMetrics.RecordDrain(startQueueSize, packetsSent);
    }

    void SendHostInfo() {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return;
        uint8_t buf[6];
        *reinterpret_cast<uint32_t*>(buf) = MSG_HOST_INFO;
        *reinterpret_cast<uint16_t*>(buf + 4) = static_cast<uint16_t>(cb.getHostFps ? cb.getHostFps() : 60);
        SafeSend(buf, sizeof(buf));
    }

    void SendMonitorList() {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return;
        std::lock_guard<std::mutex> lock(g_monitorsMutex);
        std::vector<uint8_t> buf(6 + g_monitors.size() * 74);
        size_t off = 0;
        *reinterpret_cast<uint32_t*>(&buf[off]) = MSG_MONITOR_LIST; off += 4;
        buf[off++] = static_cast<uint8_t>(g_monitors.size());
        buf[off++] = static_cast<uint8_t>(cb.getCurrentMonitor ? cb.getCurrentMonitor() : 0);
        for (const auto& m : g_monitors) {
            buf[off++] = static_cast<uint8_t>(m.index);
            *reinterpret_cast<uint16_t*>(&buf[off]) = static_cast<uint16_t>(m.width);
            *reinterpret_cast<uint16_t*>(&buf[off + 2]) = static_cast<uint16_t>(m.height);
            *reinterpret_cast<uint16_t*>(&buf[off + 4]) = static_cast<uint16_t>(m.refreshRate);
            off += 6; buf[off++] = m.isPrimary ? 1 : 0;
            size_t nl = std::min(m.name.size(), size_t(63));
            buf[off++] = static_cast<uint8_t>(nl);
            memcpy(&buf[off], m.name.c_str(), nl); off += nl;
        }
        SafeSend(buf.data(), off);
    }

    void ForceDisconnect(const char* reason) {
        if (!connected) return;
        WARN("Disconnect: %s", reason);
        connected = fpsReceived = authenticated = false; overflowCount = 0; pingTimeout = false;
        { std::lock_guard<std::mutex> lock(sendQueueMutex); while (!sendQueue.empty()) sendQueue.pop(); }
        try { if (dataChannel) dataChannel->close(); } catch (...) {}
        try { if (peerConnection) peerConnection->close(); } catch (...) {}
        if (cb.onDisconnect) cb.onDisconnect();
    }

    void HandleMessage(const rtc::binary& msg) {
        if (msg.size() < 4 || !authenticated) return;
        uint32_t magic = *reinterpret_cast<const uint32_t*>(msg.data());

        if (cb.inputHandler && (magic == MSG_MOUSE_MOVE || magic == MSG_MOUSE_BTN || magic == MSG_MOUSE_WHEEL || magic == MSG_KEY)) {
            cb.inputHandler->HandleMessage(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
            return;
        }

        switch (magic) {
            case MSG_PING:
                if (msg.size() == 16) {
                    lastPingTime = GetTimestamp() / 1000; overflowCount = 0; pingTimeout = false;
                    uint8_t resp[24]; memcpy(resp, msg.data(), 16);
                    *reinterpret_cast<uint64_t*>(resp + 16) = GetTimestamp();
                    SafeSend(resp, sizeof(resp));
                }
                break;
            case MSG_FPS_SET:
                if (msg.size() == 7) {
                    uint16_t fps = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(msg.data()) + 4);
                    uint8_t mode = static_cast<uint8_t>(msg[6]);
                    if (fps >= 1 && fps <= 240 && mode <= 2) {
                        int actual = (mode == 1 && cb.getHostFps) ? cb.getHostFps() : fps;
                        currentFps = actual; fpsReceived = true;
                        if (cb.onFpsChange) cb.onFpsChange(actual, mode);
                        uint8_t ack[7]; *reinterpret_cast<uint32_t*>(ack) = MSG_FPS_ACK;
                        *reinterpret_cast<uint16_t*>(ack + 4) = static_cast<uint16_t>(actual); ack[6] = mode;
                        SafeSend(ack, sizeof(ack));
                    }
                }
                break;
            case MSG_CODEC_SET:
                if (msg.size() == 5) {
                    uint8_t codecId = static_cast<uint8_t>(msg[4]);
                    if (codecId <= 1) {
                        CodecType newCodec = static_cast<CodecType>(codecId);
                        bool success = !cb.onCodecChange || cb.onCodecChange(newCodec);
                        if (success) { currentCodec = newCodec; needsKeyframe = true; }
                        uint8_t ack[5]; *reinterpret_cast<uint32_t*>(ack) = MSG_CODEC_ACK;
                        ack[4] = static_cast<uint8_t>(currentCodec.load());
                        SafeSend(ack, sizeof(ack));
                    }
                }
                break;
            case MSG_REQUEST_KEY: needsKeyframe = true; break;
            case MSG_MONITOR_SET:
                if (msg.size() == 5 && cb.onMonitorChange && cb.onMonitorChange(static_cast<int>(static_cast<uint8_t>(msg[4])))) {
                    needsKeyframe = true; SendMonitorList(); SendHostInfo();
                }
                break;
        }
    }

    void SetupDataChannelCallbacks() {
        if (!dataChannel) return;
        dataChannel->setBufferedAmountLowThreshold(BUFFER_LOW_THRESHOLD);
        dataChannel->onBufferedAmountLow([this]() { DrainSendQueue(); });
        dataChannel->onOpen([this] {
            connected = needsKeyframe = authenticated = true;
            lastPingTime = GetTimestamp() / 1000; overflowCount = 0;
            LOG("Data channel opened");
            SendHostInfo(); SendMonitorList();
            if (cb.onConnected) cb.onConnected();
        });
        dataChannel->onClosed([this] {
            connected = fpsReceived = authenticated = false; overflowCount = 0;
            std::lock_guard<std::mutex> lock(sendQueueMutex);
            while (!sendQueue.empty()) sendQueue.pop();
        });
        dataChannel->onMessage([this](auto data) { if (auto* b = std::get_if<rtc::binary>(&data)) HandleMessage(*b); });
    }

    void SetupPeerConnection() {
        if (peerConnection) {
            if (dataChannel && dataChannel->isOpen()) dataChannel->close();
            dataChannel.reset(); peerConnection->close();
        }
        connected = needsKeyframe = true;
        fpsReceived = gatheringComplete = authenticated = hasLocalDescription = false;
        overflowCount = 0; lastPingTime = 0; pingTimeout = false; candidateCount = 0;
        { std::lock_guard<std::mutex> lock(descMutex); localDescription.clear(); }
        { std::lock_guard<std::mutex> lock(sendQueueMutex); while (!sendQueue.empty()) sendQueue.pop(); }

        peerConnection = std::make_shared<rtc::PeerConnection>(rtcConfig);

        peerConnection->onLocalDescription([this](rtc::Description d) {
            std::lock_guard<std::mutex> lock(descMutex);
            localDescription = std::string(d); hasLocalDescription = true; descCondition.notify_all();
        });

        peerConnection->onLocalCandidate([this](rtc::Candidate) { if (++candidateCount >= 2) descCondition.notify_all(); });

        peerConnection->onStateChange([this](auto state) {
            bool was = connected.load();
            connected = (state == rtc::PeerConnection::State::Connected);
            if (connected && !was) { needsKeyframe = true; lastPingTime = GetTimestamp() / 1000; LOG("Peer connected"); }
            if (!connected && was) { fpsReceived = authenticated = false; overflowCount = 0; if (cb.onDisconnect) cb.onDisconnect(); }
        });

        peerConnection->onGatheringStateChange([this](auto state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) { gatheringComplete = true; descCondition.notify_all(); }
        });

        peerConnection->onDataChannel([this](auto ch) { if (ch->label() != "screen") return; dataChannel = ch; SetupDataChannelCallbacks(); });
    }

    bool IsConnectionStale() {
        if (!connected) return false;
        int64_t lastPing = lastPingTime.load(), now = GetTimestamp() / 1000;
        if (lastPing > 0 && (now - lastPing) > 3000) { if (!pingTimeout.exchange(true)) WARN("Ping timeout"); return true; }
        return overflowCount >= 10;
    }

public:
    WebRTCServer() {
        rtcConfig.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
        rtcConfig.portRangeBegin = 50000; rtcConfig.portRangeEnd = 50020; rtcConfig.enableIceTcp = false;
        audioBuffer.resize(4096);
        SetupPeerConnection();
        LOG("WebRTC initialized");
    }

    void Init(WebRTCCallbacks callbacks) { cb = std::move(callbacks); }

    std::string GetLocal() {
        std::unique_lock<std::mutex> lock(descMutex);
        if (!descCondition.wait_for(lock, 200ms, [this] { return hasLocalDescription.load(); })) return localDescription;
        descCondition.wait_for(lock, 150ms, [this] { return gatheringComplete.load() || candidateCount.load() >= 2; });
        return localDescription;
    }

    void SetRemote(const std::string& sdp, const std::string& type) {
        if (type == "offer") SetupPeerConnection();
        peerConnection->setRemoteDescription(rtc::Description(sdp, type));
        if (type == "offer") peerConnection->setLocalDescription();
    }

    bool IsConnected() const { return connected; }
    bool IsStreaming() const { return connected && authenticated && fpsReceived; }
    int GetCurrentFps() const { return currentFps; }
    bool NeedsKey() { return needsKeyframe.exchange(false); }
    CodecType GetCurrentCodec() const { return currentCodec.load(); }
    void SetCodec(CodecType codec) { currentCodec = codec; }

    void Send(const EncodedFrame& frame) {
        if (!IsStreaming()) return;
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) { if (connected) ForceDisconnect("Channel closed"); return; }
        if (IsConnectionStale()) { ForceDisconnect("Stale connection"); return; }

        size_t dataSize = frame.data.size();
        size_t numChunks = (dataSize + DATA_CHUNK_SIZE - 1) / DATA_CHUNK_SIZE;
        if (numChunks > 65535 || !dataSize) return;

        uint32_t currentFrameId = frameId++;
        size_t maxQueueSize = numChunks * MAX_QUEUE_FRAMES;

        LARGE_INTEGER sendStart; QueryPerformanceCounter(&sendStart);

        {
            std::lock_guard<std::mutex> lock(sendQueueMutex);
            if (sendQueue.size() > maxQueueSize) {
                size_t toDrop = sendQueue.size() - numChunks;
                while (toDrop > 0 && !sendQueue.empty()) { sendQueue.pop(); toDrop--; }
                dropCount++; needsKeyframe = true;
            }
        }

        PacketHeader hdr = {frame.ts, static_cast<uint32_t>(frame.encUs), currentFrameId, 0, static_cast<uint16_t>(numChunks), frame.isKey ? uint8_t(1) : uint8_t(0)};

        {
            std::lock_guard<std::mutex> lock(sendQueueMutex);
            for (size_t i = 0; i < numChunks; i++) {
                hdr.chunkIndex = static_cast<uint16_t>(i);
                size_t off = i * DATA_CHUNK_SIZE, len = std::min(DATA_CHUNK_SIZE, dataSize - off);
                QueuedPacket pkt;
                pkt.data.resize(HEADER_SIZE + len);
                memcpy(pkt.data.data(), &hdr, HEADER_SIZE);
                memcpy(pkt.data.data() + HEADER_SIZE, frame.data.data() + off, len);
                pkt.frameId = currentFrameId; pkt.isLastChunk = (i == numChunks - 1);
                sendQueue.push(std::move(pkt));
            }
        }
        sentCount++;
        DrainSendQueue();

        LARGE_INTEGER sendEnd; QueryPerformanceCounter(&sendEnd);
        static const int64_t qpcFreq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
        pacedMetrics.RecordFrameSend((sendEnd.QuadPart - sendStart.QuadPart) * 1000000 / qpcFreq);
    }

    void SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples) {
        if (!IsStreaming() || data.empty() || data.size() > 4000 || overflowCount >= 5) return;
        auto ch = dataChannel;
        if (!ch || !ch->isOpen() || ch->bufferedAmount() > BUFFER_THRESHOLD / 2) return;
        try {
            size_t total = sizeof(AudioPacketHeader) + data.size();
            if (audioBuffer.size() < total) audioBuffer.resize(total);
            auto* hdr = reinterpret_cast<AudioPacketHeader*>(audioBuffer.data());
            hdr->magic = MSG_AUDIO_DATA; hdr->timestamp = ts; hdr->samples = static_cast<uint16_t>(samples); hdr->dataLength = static_cast<uint16_t>(data.size());
            memcpy(audioBuffer.data() + sizeof(AudioPacketHeader), data.data(), data.size());
            if (SafeSend(audioBuffer.data(), total)) { byteCount += total; audioSentCount++; }
        } catch (...) {}
    }

    struct Stats { uint64_t sent, bytes, dropped; bool connected; };
    Stats GetStats() { return {sentCount.exchange(0), byteCount.exchange(0), dropCount.exchange(0), connected.load()}; }
    uint64_t GetAudioSent() { return audioSentCount.exchange(0); }
    PacedSendMetrics::Snapshot GetPacedMetrics() { return pacedMetrics.GetAndReset(); }
    size_t GetSendQueueDepth() { std::lock_guard<std::mutex> lock(sendQueueMutex); return sendQueue.size(); }
};
