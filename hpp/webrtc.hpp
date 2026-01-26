#pragma once

#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"
#include <chrono>

using namespace std::chrono;

// ============================================================================
// Packet Header Structures
// ============================================================================

#pragma pack(push, 1)

struct PacketHeader {
    int64_t timestamp;
    uint32_t encodeTimeUs;
    uint32_t frameId;
    uint16_t chunkIndex;
    uint16_t totalChunks;
    uint8_t frameType;
};

struct AudioPacketHeader {
    uint32_t magic;
    int64_t timestamp;
    uint16_t samples;
    uint16_t dataLength;
};

#pragma pack(pop)

// ============================================================================
// WebRTC Callbacks
// ============================================================================

struct WebRTCCallbacks {
    InputHandler* inputHandler = nullptr;
    std::function<void(int, uint8_t)> onFpsChange;
    std::function<int()> getHostFps;
    std::function<bool(int)> onMonitorChange;
    std::function<int()> getCurrentMonitor;
    std::function<void()> onDisconnect;
    std::function<void()> onConnected;
};

// ============================================================================
// Paced Send Metrics
// ============================================================================

struct PacedSendMetrics {
    std::atomic<uint64_t> queueDepthSum{0};
    std::atomic<uint64_t> queueDepthSamples{0};
    std::atomic<uint64_t> maxQueueDepth{0};
    std::atomic<int64_t> frameSendTimeSum{0};
    std::atomic<int64_t> frameSendTimeSamples{0};
    std::atomic<int64_t> maxFrameSendTime{0};
    std::atomic<uint64_t> burstSum{0};
    std::atomic<uint64_t> burstSamples{0};
    std::atomic<uint64_t> maxBurst{0};
    std::atomic<uint64_t> pacedDrainCount{0};

    void RecordQueueDepth(uint64_t depth) {
        queueDepthSum += depth;
        queueDepthSamples++;
        for (uint64_t m = maxQueueDepth.load();
             depth > m && !maxQueueDepth.compare_exchange_weak(m, depth););
    }

    void RecordFrameSendTime(int64_t us) {
        frameSendTimeSum += us;
        frameSendTimeSamples++;
        for (int64_t m = maxFrameSendTime.load();
             us > m && !maxFrameSendTime.compare_exchange_weak(m, us););
    }

    void RecordBurst(uint64_t size) {
        burstSum += size;
        burstSamples++;
        for (uint64_t m = maxBurst.load();
             size > m && !maxBurst.compare_exchange_weak(m, size););
    }

    struct Snapshot {
        uint64_t avgQueueDepth;
        uint64_t maxQueueDepth;
        int64_t avgFrameSendTimeUs;
        int64_t maxFrameSendTimeUs;
        uint64_t avgBurst;
        uint64_t maxBurst;
        uint64_t drainEvents;
    };

    Snapshot GetAndReset() {
        uint64_t qds = queueDepthSamples.exchange(0);
        uint64_t qdsum = queueDepthSum.exchange(0);
        int64_t fss = frameSendTimeSamples.exchange(0);
        int64_t fssum = frameSendTimeSum.exchange(0);
        uint64_t bs = burstSamples.exchange(0);
        uint64_t bsum = burstSum.exchange(0);

        return {
            qds > 0 ? qdsum / qds : 0,
            maxQueueDepth.exchange(0),
            fss > 0 ? fssum / fss : 0,
            maxFrameSendTime.exchange(0),
            bs > 0 ? bsum / bs : 0,
            maxBurst.exchange(0),
            pacedDrainCount.exchange(0)
        };
    }
};

// ============================================================================
// WebRTC Server Class
// ============================================================================

class WebRTCServer {
private:
    // WebRTC resources
    std::shared_ptr<rtc::PeerConnection> peerConnection;
    std::shared_ptr<rtc::DataChannel> dataChannel;

    // State flags
    std::atomic<bool> connected{false};
    std::atomic<bool> needsKeyframe{true};
    std::atomic<bool> fpsReceived{false};
    std::atomic<bool> gatheringComplete{false};
    std::atomic<bool> authenticated{false};
    std::atomic<bool> hasLocalDescription{false};

    // Local description
    std::string localDescription;
    std::mutex descMutex;
    std::condition_variable descCondition;

    // Configuration
    rtc::Configuration rtcConfig;

    // Constants
    static constexpr size_t BUFFER_THRESHOLD = 262144;
    static constexpr size_t CHUNK_SIZE = 1400;
    static constexpr size_t HEADER_SIZE = sizeof(PacketHeader);
    static constexpr size_t DATA_CHUNK_SIZE = CHUNK_SIZE - HEADER_SIZE;
    static constexpr size_t BUFFER_LOW_THRESHOLD = CHUNK_SIZE * 16;
    static constexpr size_t MAX_QUEUE_FRAMES = 3;

    // Buffers
    std::vector<uint8_t> packetBuffer;
    std::vector<uint8_t> audioBuffer;

    // Metrics
    std::atomic<uint64_t> sentCount{0};
    std::atomic<uint64_t> byteCount{0};
    std::atomic<uint64_t> dropCount{0};
    std::atomic<uint64_t> audioSentCount{0};
    std::atomic<uint32_t> frameId{0};
    std::atomic<int> currentFps{60};
    std::atomic<int> overflowCount{0};
    std::atomic<uint8_t> currentFpsMode{0};
    std::atomic<int64_t> lastPingTime{0};
    std::atomic<bool> pingTimeout{false};
    std::atomic<int> candidateCount{0};

    // Send queue
    struct QueuedPacket {
        std::vector<uint8_t> data;
        uint32_t frameId;
        bool isLastChunk;
        int64_t queuedAt;
    };

    std::queue<QueuedPacket> sendQueue;
    std::mutex sendQueueMutex;
    std::atomic<bool> drainScheduled{false};

    // Frame send tracking
    struct FrameSendState {
        int64_t startTime{0};
        uint32_t frameId{0};
        bool active{false};
    };

    FrameSendState currentFrameSend;
    std::mutex frameSendMutex;
    PacedSendMetrics pacedMetrics;

    // Callbacks
    WebRTCCallbacks cb;

    // ========================================================================
    // Safe Send
    // ========================================================================

    bool SafeSend(const void* data, size_t len) {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) {
            return false;
        }

        try {
            ch->send(reinterpret_cast<const std::byte*>(data), len);
            return true;
        } catch (const std::exception& e) {
            LOG("[WebRTC] SafeSend exception: %s", e.what());
            return false;
        }
    }

    // ========================================================================
    // Drain Send Queue
    // ========================================================================

    void DrainSendQueue() {
        std::unique_lock<std::mutex> lock(sendQueueMutex);

        auto ch = dataChannel;
        if (!ch || !ch->isOpen() || sendQueue.empty()) {
            return;
        }

        size_t packetsSent = 0;
        size_t bytesSent = 0;

        while (!sendQueue.empty()) {
            if (ch->bufferedAmount() > BUFFER_THRESHOLD) {
                break;
            }

            QueuedPacket& pkt = sendQueue.front();

            if (!SafeSend(pkt.data.data(), pkt.data.size())) {
                overflowCount++;
                dropCount++;
                needsKeyframe = true;
                sendQueue.pop();
                continue;
            }

            bytesSent += pkt.data.size();
            packetsSent++;

            bool wasLastChunk = pkt.isLastChunk;
            uint32_t pktFrameId = pkt.frameId;
            sendQueue.pop();

            if (wasLastChunk) {
                std::lock_guard<std::mutex> flock(frameSendMutex);
                if (currentFrameSend.active && currentFrameSend.frameId == pktFrameId) {
                    pacedMetrics.RecordFrameSendTime(GetTimestamp() - currentFrameSend.startTime);
                    currentFrameSend.active = false;
                }
            }
        }

        if (packetsSent > 0) {
            byteCount += bytesSent;
            pacedMetrics.RecordBurst(packetsSent);
            pacedMetrics.pacedDrainCount++;
        }

        pacedMetrics.RecordQueueDepth(sendQueue.size());
    }

    // ========================================================================
    // Send Host Info
    // ========================================================================

    void SendHostInfo() {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return;

        uint8_t buf[6];
        *reinterpret_cast<uint32_t*>(buf) = MSG_HOST_INFO;
        *reinterpret_cast<uint16_t*>(buf + 4) = static_cast<uint16_t>(
            cb.getHostFps ? cb.getHostFps() : 60
        );

        SafeSend(buf, sizeof(buf));
    }

    // ========================================================================
    // Send Monitor List
    // ========================================================================

    void SendMonitorList() {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return;

        std::lock_guard<std::mutex> lock(g_monitorsMutex);

        std::vector<uint8_t> buf(6 + g_monitors.size() * 74);
        size_t off = 0;

        *reinterpret_cast<uint32_t*>(&buf[off]) = MSG_MONITOR_LIST;
        off += 4;

        buf[off++] = static_cast<uint8_t>(g_monitors.size());
        buf[off++] = static_cast<uint8_t>(cb.getCurrentMonitor ? cb.getCurrentMonitor() : 0);

        for (const auto& m : g_monitors) {
            buf[off++] = static_cast<uint8_t>(m.index);
            *reinterpret_cast<uint16_t*>(&buf[off]) = static_cast<uint16_t>(m.width);
            *reinterpret_cast<uint16_t*>(&buf[off + 2]) = static_cast<uint16_t>(m.height);
            *reinterpret_cast<uint16_t*>(&buf[off + 4]) = static_cast<uint16_t>(m.refreshRate);
            off += 6;

            buf[off++] = m.isPrimary ? 1 : 0;

            size_t nl = std::min(m.name.size(), size_t(63));
            buf[off++] = static_cast<uint8_t>(nl);
            memcpy(&buf[off], m.name.c_str(), nl);
            off += nl;
        }

        SafeSend(buf.data(), off);
    }

    // ========================================================================
    // Force Disconnect
    // ========================================================================

    void ForceDisconnect(const char* reason) {
        if (!connected) return;

        WARN("Disconnect: %s", reason);

        connected = fpsReceived = authenticated = false;
        overflowCount = 0;
        pingTimeout = false;

        {
            std::lock_guard<std::mutex> lock(sendQueueMutex);
            while (!sendQueue.empty()) {
                sendQueue.pop();
            }
        }

        try {
            if (dataChannel) dataChannel->close();
        } catch (...) {}

        try {
            if (peerConnection) peerConnection->close();
        } catch (...) {}

        if (cb.onDisconnect) {
            cb.onDisconnect();
        }
    }

    // ========================================================================
    // Handle Message
    // ========================================================================

    void HandleMessage(const rtc::binary& msg) {
        if (msg.size() < 4 || !authenticated) return;

        uint32_t magic = *reinterpret_cast<const uint32_t*>(msg.data());

        // Handle input messages
        if (cb.inputHandler &&
            (magic == MSG_MOUSE_MOVE || magic == MSG_MOUSE_BTN ||
             magic == MSG_MOUSE_WHEEL || magic == MSG_KEY)) {
            cb.inputHandler->HandleMessage(
                reinterpret_cast<const uint8_t*>(msg.data()),
                msg.size()
            );
            return;
        }

        switch (magic) {
            case MSG_PING:
                if (msg.size() == 16) {
                    lastPingTime = GetTimestamp() / 1000;
                    overflowCount = 0;
                    pingTimeout = false;

                    uint8_t resp[24];
                    memcpy(resp, msg.data(), 16);
                    *reinterpret_cast<uint64_t*>(resp + 16) = GetTimestamp();

                    SafeSend(resp, sizeof(resp));
                }
                break;

            case MSG_FPS_SET:
                if (msg.size() == 7) {
                    uint16_t fps = *reinterpret_cast<const uint16_t*>(
                        reinterpret_cast<const uint8_t*>(msg.data()) + 4
                    );
                    uint8_t mode = static_cast<uint8_t>(msg[6]);

                    if (fps >= 1 && fps <= 240 && mode <= 2) {
                        int actual = (mode == 1 && cb.getHostFps) ? cb.getHostFps() : fps;
                        currentFps = actual;
                        currentFpsMode = mode;
                        fpsReceived = true;

                        if (cb.onFpsChange) {
                            cb.onFpsChange(actual, mode);
                        }

                        uint8_t ack[7];
                        *reinterpret_cast<uint32_t*>(ack) = MSG_FPS_ACK;
                        *reinterpret_cast<uint16_t*>(ack + 4) = static_cast<uint16_t>(actual);
                        ack[6] = mode;

                        SafeSend(ack, sizeof(ack));
                    }
                }
                break;

            case MSG_REQUEST_KEY:
                needsKeyframe = true;
                break;

            case MSG_MONITOR_SET:
                if (msg.size() == 5 && cb.onMonitorChange) {
                    if (cb.onMonitorChange(static_cast<int>(static_cast<uint8_t>(msg[4])))) {
                        needsKeyframe = true;
                        SendMonitorList();
                        SendHostInfo();
                    }
                }
                break;
        }
    }

    // ========================================================================
    // Setup Data Channel Callbacks
    // ========================================================================

    void SetupDataChannelCallbacks() {
        if (!dataChannel) return;

        dataChannel->setBufferedAmountLowThreshold(BUFFER_LOW_THRESHOLD);

        dataChannel->onBufferedAmountLow([this]() {
            DrainSendQueue();
        });

        dataChannel->onOpen([this] {
            connected = needsKeyframe = authenticated = true;
            lastPingTime = GetTimestamp() / 1000;
            overflowCount = 0;

            LOG("Data channel opened");

            SendHostInfo();
            SendMonitorList();

            if (cb.onConnected) {
                cb.onConnected();
            }
        });

        dataChannel->onClosed([this] {
            connected = fpsReceived = authenticated = false;
            overflowCount = 0;

            std::lock_guard<std::mutex> lock(sendQueueMutex);
            while (!sendQueue.empty()) {
                sendQueue.pop();
            }
        });

        dataChannel->onMessage([this](auto data) {
            if (auto* b = std::get_if<rtc::binary>(&data)) {
                HandleMessage(*b);
            }
        });
    }

    // ========================================================================
    // Setup Peer Connection
    // ========================================================================

    void SetupPeerConnection() {
        if (peerConnection) {
            if (dataChannel && dataChannel->isOpen()) {
                dataChannel->close();
            }
            dataChannel.reset();
            peerConnection->close();
        }

        connected = needsKeyframe = true;
        fpsReceived = gatheringComplete = authenticated = hasLocalDescription = false;
        overflowCount = 0;
        lastPingTime = 0;
        pingTimeout = false;
        candidateCount = 0;

        {
            std::lock_guard<std::mutex> lock(descMutex);
            localDescription.clear();
        }

        {
            std::lock_guard<std::mutex> lock(sendQueueMutex);
            while (!sendQueue.empty()) {
                sendQueue.pop();
            }
        }

        peerConnection = std::make_shared<rtc::PeerConnection>(rtcConfig);

        peerConnection->onLocalDescription([this](rtc::Description d) {
            std::lock_guard<std::mutex> lock(descMutex);
            localDescription = std::string(d);
            hasLocalDescription = true;
            descCondition.notify_all();
            LOG("Local description ready");
        });

        peerConnection->onLocalCandidate([this](rtc::Candidate) {
            if (++candidateCount >= 2) {
                descCondition.notify_all();
            }
        });

        peerConnection->onStateChange([this](auto state) {
            bool was = connected.load();
            connected = (state == rtc::PeerConnection::State::Connected);

            if (connected && !was) {
                needsKeyframe = true;
                lastPingTime = GetTimestamp() / 1000;
                LOG("Peer connected");
            }

            if (!connected && was) {
                fpsReceived = authenticated = false;
                overflowCount = 0;
                if (cb.onDisconnect) {
                    cb.onDisconnect();
                }
            }
        });

        peerConnection->onGatheringStateChange([this](auto state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                gatheringComplete = true;
                descCondition.notify_all();
                LOG("ICE gathering complete (%d candidates)", candidateCount.load());
            }
        });

        peerConnection->onDataChannel([this](auto ch) {
            if (ch->label() != "screen") return;
            dataChannel = ch;
            SetupDataChannelCallbacks();
        });
    }

    // ========================================================================
    // Connection Staleness Check
    // ========================================================================

    bool IsConnectionStale() {
        if (!connected) return false;

        int64_t lastPing = lastPingTime.load();
        int64_t now = GetTimestamp() / 1000;

        if (lastPing > 0 && (now - lastPing) > 3000) {
            if (!pingTimeout.exchange(true)) {
                WARN("Ping timeout");
            }
            return true;
        }

        return overflowCount >= 10;
    }

public:
    // ========================================================================
    // Constructor
    // ========================================================================

    WebRTCServer() {
        rtcConfig.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
        rtcConfig.portRangeBegin = 50000;
        rtcConfig.portRangeEnd = 50020;
        rtcConfig.enableIceTcp = false;

        packetBuffer.resize(CHUNK_SIZE);
        audioBuffer.resize(4096);

        SetupPeerConnection();
        LOG("WebRTC initialized");
    }

    // ========================================================================
    // Initialization
    // ========================================================================

    void Init(WebRTCCallbacks callbacks) {
        cb = std::move(callbacks);
    }

    // ========================================================================
    // SDP Handling
    // ========================================================================

    std::string GetLocal() {
        std::unique_lock<std::mutex> lock(descMutex);

        if (!descCondition.wait_for(lock, 200ms, [this] {
            return hasLocalDescription.load();
        })) {
            WARN("Timeout waiting for description");
            return localDescription;
        }

        descCondition.wait_for(lock, 150ms, [this] {
            return gatheringComplete.load() || candidateCount.load() >= 2;
        });

        LOG("Returning answer with %d candidates (gathering %s)",
            candidateCount.load(),
            gatheringComplete.load() ? "complete" : "partial");

        return localDescription;
    }

    void SetRemote(const std::string& sdp, const std::string& type) {
        if (type == "offer") {
            SetupPeerConnection();
        }

        peerConnection->setRemoteDescription(rtc::Description(sdp, type));

        if (type == "offer") {
            peerConnection->setLocalDescription();
        }
    }

    // ========================================================================
    // State Queries
    // ========================================================================

    bool IsConnected() const { return connected; }
    bool IsAuthenticated() const { return authenticated; }
    bool IsFpsReceived() const { return fpsReceived; }
    bool IsStreaming() const { return connected && authenticated && fpsReceived; }
    int GetCurrentFps() const { return currentFps; }
    bool NeedsKey() { return needsKeyframe.exchange(false); }

    // ========================================================================
    // Send Video Frame
    // ========================================================================

    void Send(const EncodedFrame& frame) {
        if (!IsStreaming()) return;

        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) {
            if (connected) {
                ForceDisconnect("Channel closed");
            }
            return;
        }

        if (IsConnectionStale()) {
            ForceDisconnect("Stale connection");
            return;
        }

        size_t dataSize = frame.data.size();
        size_t numChunks = (dataSize + DATA_CHUNK_SIZE - 1) / DATA_CHUNK_SIZE;

        if (numChunks > 65535 || !dataSize) return;

        int64_t now = GetTimestamp();
        uint32_t currentFrameId = frameId++;
        size_t maxQueueSize = numChunks * MAX_QUEUE_FRAMES;

        // Drop old frames if queue is too large
        {
            std::lock_guard<std::mutex> lock(sendQueueMutex);

            if (sendQueue.size() > maxQueueSize) {
                size_t toDrop = sendQueue.size() - numChunks;
                while (toDrop > 0 && !sendQueue.empty()) {
                    sendQueue.pop();
                    toDrop--;
                }
                dropCount++;
                needsKeyframe = true;
            }
        }

        // Track frame send timing
        {
            std::lock_guard<std::mutex> flock(frameSendMutex);
            currentFrameSend = {now, currentFrameId, true};
        }

        // Build packet header
        PacketHeader hdr = {
            frame.ts,
            static_cast<uint32_t>(frame.encUs),
            currentFrameId,
            0,
            static_cast<uint16_t>(numChunks),
            frame.isKey ? uint8_t(1) : uint8_t(0)
        };

        // Queue all chunks
        {
            std::lock_guard<std::mutex> lock(sendQueueMutex);

            for (size_t i = 0; i < numChunks; i++) {
                hdr.chunkIndex = static_cast<uint16_t>(i);

                size_t off = i * DATA_CHUNK_SIZE;
                size_t len = std::min(DATA_CHUNK_SIZE, dataSize - off);

                QueuedPacket pkt;
                pkt.data.resize(HEADER_SIZE + len);
                memcpy(pkt.data.data(), &hdr, HEADER_SIZE);
                memcpy(pkt.data.data() + HEADER_SIZE, frame.data.data() + off, len);
                pkt.frameId = currentFrameId;
                pkt.isLastChunk = (i == numChunks - 1);
                pkt.queuedAt = now;

                sendQueue.push(std::move(pkt));
            }
        }

        sentCount++;
        DrainSendQueue();
    }

    // ========================================================================
    // Send Audio
    // ========================================================================

    void SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples) {
        if (!IsStreaming() || data.empty() || data.size() > 4000 || overflowCount >= 5) {
            return;
        }

        auto ch = dataChannel;
        if (!ch || !ch->isOpen() || ch->bufferedAmount() > BUFFER_THRESHOLD / 2) {
            return;
        }

        try {
            size_t total = sizeof(AudioPacketHeader) + data.size();

            if (audioBuffer.size() < total) {
                audioBuffer.resize(total);
            }

            auto* hdr = reinterpret_cast<AudioPacketHeader*>(audioBuffer.data());
            hdr->magic = MSG_AUDIO_DATA;
            hdr->timestamp = ts;
            hdr->samples = static_cast<uint16_t>(samples);
            hdr->dataLength = static_cast<uint16_t>(data.size());

            memcpy(audioBuffer.data() + sizeof(AudioPacketHeader), data.data(), data.size());

            if (SafeSend(audioBuffer.data(), total)) {
                byteCount += total;
                audioSentCount++;
            }
        } catch (...) {}
    }

    // ========================================================================
    // Statistics
    // ========================================================================

    struct Stats {
        uint64_t sent;
        uint64_t bytes;
        uint64_t dropped;
        bool connected;
    };

    Stats GetStats() {
        return {
            sentCount.exchange(0),
            byteCount.exchange(0),
            dropCount.exchange(0),
            connected.load()
        };
    }

    uint64_t GetAudioSent() {
        return audioSentCount.exchange(0);
    }

    PacedSendMetrics::Snapshot GetPacedMetrics() {
        return pacedMetrics.GetAndReset();
    }

    size_t GetSendQueueDepth() {
        std::lock_guard<std::mutex> lock(sendQueueMutex);
        return sendQueue.size();
    }
};
