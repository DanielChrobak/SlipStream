#include "host/net/webrtc.hpp"
#include <array>
#include <cstring>
#include <utility>

using namespace std::chrono_literals;

namespace {
template <typename T>
void WritePod(uint8_t* dst, const T& value) {
    std::memcpy(dst, &value, sizeof(T));
}

template <typename T>
[[nodiscard]] T ReadPod(const uint8_t* src) {
    T value{};
    std::memcpy(&value, src, sizeof(T));
    return value;
}

const char* ToPeerStateString(rtc::PeerConnection::State s) {
    static constexpr const char* names[] = {"new", "connecting", "connected", "disconnected", "failed", "closed"};
    int i = static_cast<int>(s);
    return (i >= 0 && i < 6) ? names[i] : "unknown";
}

const char* ToGatherStateString(rtc::PeerConnection::GatheringState s) {
    static constexpr const char* names[] = {"new", "in-progress", "complete"};
    int i = static_cast<int>(s);
    return (i >= 0 && i < 3) ? names[i] : "unknown";
}

bool HasPublicIceCandidate(const std::string& sdp) {
    return sdp.find(" typ srflx") != std::string::npos ||
           sdp.find(" typ relay") != std::string::npos;
}

constexpr size_t kVideoQueueMaxPackets = 2048;
constexpr size_t kVideoQueueTrimTargetPackets = 512;
constexpr size_t kVideoQueueCongestionThreshold = 256;
constexpr size_t kVideoQueueFecBypassThreshold = 128;

void DrainQueuedChannel(
    const std::shared_ptr<rtc::DataChannel>& channel,
    std::queue<std::vector<uint8_t>>& queue,
    size_t bufferLimit,
    std::atomic<uint64_t>& errorCounter,
    std::atomic<int>* overflowCounter = nullptr,
    std::atomic<bool>* requestKey = nullptr) {
    if (!channel || !channel->isOpen()) {
        if (!queue.empty()) {
            DBG("WebRTC: DrainQueuedChannel skipped - channel %s (queue=%zu)",
                !channel ? "null" : "closed", queue.size());
        }
        return;
    }
    size_t sent = 0, failed = 0;
    const size_t queueBefore = queue.size();
    const size_t bufferedBefore = channel->bufferedAmount();
    while (!queue.empty() && channel->bufferedAmount() <= bufferLimit) {
        try {
            channel->send((const std::byte*)queue.front().data(), queue.front().size());
            sent++;
        } catch (const std::exception& e) {
            errorCounter++;
            failed++;
            ERR("WebRTC: DrainQueuedChannel send failed: %s (buffered=%zu, limit=%zu, queueRemaining=%zu)",
                e.what(), channel->bufferedAmount(), bufferLimit, queue.size());
            if (overflowCounter) (*overflowCounter)++;
            if (requestKey) requestKey->store(true, std::memory_order_release);
        } catch (...) {
            errorCounter++;
            failed++;
            ERR("WebRTC: DrainQueuedChannel send failed with unknown exception (buffered=%zu, limit=%zu)",
                channel->bufferedAmount(), bufferLimit);
            if (overflowCounter) (*overflowCounter)++;
            if (requestKey) requestKey->store(true, std::memory_order_release);
        }
        queue.pop();
    }
    const size_t bufferedAfter = channel->bufferedAmount();
    const size_t queueAfter = queue.size();
    if (failed > 0) {
        WARN("WebRTC: DrainQueuedChannel completed: sent=%zu failed=%zu remaining=%zu", sent, failed, queue.size());
    }
}

using ChannelHandler = std::function<void(const rtc::binary&)>;
}

bool WebRTCServer::SendCtrl(const void* data, size_t byteCount) {
    std::shared_ptr<rtc::DataChannel> ctrl;
    {
        std::lock_guard<std::mutex> lk(channelMutex_);
        ctrl = controlDataChannel_;
    }
    if (!ctrl || !ctrl->isOpen()) {
        DBG("WebRTC: SendCtrl failed - channel %s (size=%zu)", !ctrl ? "null" : "closed", byteCount);
        return false;
    }
    try {
        ctrl->send((const std::byte*)data, byteCount);
        ctrlSent++;
        return true;
    } catch (const std::exception& e) {
        WARN("WebRTC: SendCtrl failed: %s (size=%zu, buffered=%zu)",
            e.what(), byteCount, ctrl->bufferedAmount());
        return false;
    } catch (...) {
        WARN("WebRTC: SendCtrl failed with unknown exception (size=%zu)", byteCount);
        return false;
    }
}

void WebRTCServer::SendHostInfo() {
    uint8_t buf[6];
    WritePod<uint32_t>(buf, MSG_HOST_INFO);
    WritePod<uint16_t>(buf + 4, static_cast<uint16_t>(callbacks_.getHostFps ? callbacks_.getHostFps() : 60));
    SendCtrl(buf, sizeof(buf));
}

void WebRTCServer::SendMonitorList() {
    std::vector<uint8_t> payload;
    std::lock_guard<std::mutex> lk(g_monitorsMutex);

    payload.resize(6 + g_monitors.size() * 74);
    size_t offset = 0;
    WritePod<uint32_t>(payload.data() + offset, MSG_MONITOR_LIST); offset += 4;
    payload[offset++] = static_cast<uint8_t>(g_monitors.size());
    payload[offset++] = static_cast<uint8_t>(callbacks_.getMonitor ? callbacks_.getMonitor() : 0);

    for (const auto& monitor : g_monitors) {
        payload[offset++] = static_cast<uint8_t>(monitor.index);
        WritePod<uint16_t>(payload.data() + offset, static_cast<uint16_t>(monitor.width));
        WritePod<uint16_t>(payload.data() + offset + 2, static_cast<uint16_t>(monitor.height));
        WritePod<uint16_t>(payload.data() + offset + 4, static_cast<uint16_t>(monitor.refreshRate));
        offset += 6;
        payload[offset++] = monitor.isPrimary ? 1 : 0;
        size_t nameLength = std::min(monitor.name.size(), static_cast<size_t>(63));
        payload[offset++] = static_cast<uint8_t>(nameLength);
        memcpy(&payload[offset], monitor.name.c_str(), nameLength);
        offset += nameLength;
    }
    payload.resize(offset);
    SendCtrl(payload.data(), payload.size());
}

void WebRTCServer::SendCodecCaps() {
    uint8_t buf[5] = {};
    WritePod<uint32_t>(buf, MSG_CODEC_CAPS);
    buf[4] = callbacks_.getCodecCaps ? callbacks_.getCodecCaps() : 0x07;
    SendCtrl(buf, sizeof(buf));
}

void WebRTCServer::SendVersion() {
    std::string ver = SLIPSTREAM_VERSION;
    std::vector<uint8_t> buf(5 + ver.size());
    WritePod<uint32_t>(buf.data(), MSG_VERSION);
    buf[4] = static_cast<uint8_t>(ver.size());
    memcpy(buf.data() + 5, ver.c_str(), ver.size());
    SendCtrl(buf.data(), buf.size());
}

void WebRTCServer::HandleCtrl(const rtc::binary& message) {
    if (message.size() < 4 || chRdy < NUM_CH) {
        if (message.size() < 4) {
            WARN("WebRTC: HandleCtrl - message too small (%zu bytes)", message.size());
        }
        return;
    }
    ctrlRecv++;

    const auto* data = reinterpret_cast<const uint8_t*>(message.data());
    const uint32_t magic = ReadPod<uint32_t>(data);

    const auto handleToggle = [&](const auto& callback) {
        if (message.size() == 5 && callback) {
            callback(static_cast<uint8_t>(message[4]) != 0);
        }
    };

    const auto sendAckU16Mode = [&](uint32_t ackType, uint16_t value, uint8_t mode) {
        uint8_t ack[7];
        WritePod<uint32_t>(ack, ackType);
        WritePod<uint16_t>(ack + 4, value);
        ack[6] = mode;
        SendCtrl(ack, sizeof(ack));
    };

    const auto sendAckU8 = [&](uint32_t ackType, uint8_t value) {
        uint8_t ack[5]{};
        WritePod<uint32_t>(ack, ackType);
        ack[4] = value;
        SendCtrl(ack, sizeof(ack));
    };

    switch (magic) {
        case MSG_PING:
            if (message.size() == 16) {
                lastPing = GetTimestamp() / 1000;
                overflow = 0;
                uint8_t r[24];
                memcpy(r, message.data(), 16);
                WritePod<uint64_t>(r + 16, static_cast<uint64_t>(GetTimestamp()));
                SendCtrl(r, sizeof(r));
            }
            break;

        case MSG_FPS_SET:
            if (message.size() == 7) {
                const uint16_t fps = ReadPod<uint16_t>(data + 4);
                uint8_t mode = static_cast<uint8_t>(message[6]);
                if (fps >= 1 && fps <= 240 && mode <= 2) {
                    const int act = (mode == 1 && callbacks_.getHostFps) ? callbacks_.getHostFps() : fps;
                    fpsRecv = true;
                    LOG("WebRTC: FPS set to %d (mode=%d)", act, mode);
                    if (callbacks_.onFpsChange) callbacks_.onFpsChange(act, mode);
                    sendAckU16Mode(MSG_FPS_ACK, static_cast<uint16_t>(act), mode);
                }
            }
            break;

        case MSG_CODEC_SET:
            if (message.size() == 5 && static_cast<uint8_t>(message[4]) <= 2) {
                CodecType requestedCodec = static_cast<CodecType>(static_cast<uint8_t>(message[4]));
                bool accepted = !callbacks_.onCodecChange || callbacks_.onCodecChange(requestedCodec);
                if (accepted) { curCodec = requestedCodec; needsKey = true; }
                sendAckU8(MSG_CODEC_ACK, static_cast<uint8_t>(curCodec.load()));
            }
            break;

        case MSG_REQUEST_KEY:
            {
                constexpr int64_t KEY_REQ_MIN_INTERVAL_MS = 350;
                int64_t nowMs = GetTimestamp() / 1000;
                int64_t lastMs = lastKeyReqMs.load(std::memory_order_acquire);
                if (nowMs - lastMs >= KEY_REQ_MIN_INTERVAL_MS) {
                    lastKeyReqMs.store(nowMs, std::memory_order_release);
                    if (!needsKey.exchange(true, std::memory_order_acq_rel))
                        DBG("WebRTC: Keyframe request accepted");
                }
            }
            break;

        case MSG_MONITOR_SET:
            if (message.size() == 5 && callbacks_.onMonitorChange) {
                if (callbacks_.onMonitorChange(static_cast<int>(static_cast<uint8_t>(message[4])))) {
                    needsKey = true;
                    SendMonitorList();
                    SendHostInfo();
                }
            }
            break;

        case MSG_CLIPBOARD_DATA:
            if (message.size() >= 8 && callbacks_.setClipboard) {
                uint32_t len = ReadPod<uint32_t>(data + 4);
                if (len > 0 && message.size() >= 8 + len && len <= 1048576)
                    callbacks_.setClipboard(std::string(reinterpret_cast<const char*>(message.data()) + 8, len));
            }
            break;

        case MSG_CLIPBOARD_GET:
            if (callbacks_.getClipboard) {
                std::string text = callbacks_.getClipboard();
                if (!text.empty() && text.size() <= 1048576) {
                    std::vector<uint8_t> buf(8 + text.size());
                    WritePod<uint32_t>(buf.data(), MSG_CLIPBOARD_DATA);
                    WritePod<uint32_t>(buf.data() + 4, static_cast<uint32_t>(text.size()));
                    memcpy(buf.data() + 8, text.data(), text.size());
                    SendCtrl(buf.data(), buf.size());
                }
            }
            break;

        case MSG_CURSOR_CAPTURE:
            handleToggle(callbacks_.onCursorCapture);
            break;

        case MSG_AUDIO_ENABLE:
            handleToggle(callbacks_.onAudioEnable);
            break;

        case MSG_MIC_ENABLE:
            handleToggle(callbacks_.onMicEnable);
            break;
    }
}

void WebRTCServer::HandleInput(const rtc::binary& message) {
    if (message.size() < 4 || chRdy < NUM_CH || !callbacks_.input) {
        if (message.size() < 4) {
            WARN("WebRTC: HandleInput - message too small (%zu bytes)", message.size());
        } else if (chRdy < NUM_CH) {
            DBG("WebRTC: HandleInput - channels not ready (%d/%d)", chRdy.load(), NUM_CH);
        }
        return;
    }
    inputRecv++;
    bool handled = callbacks_.input->HandleMessage(reinterpret_cast<const uint8_t*>(message.data()), message.size());
    if (!handled) {
        uint32_t msgType = ReadPod<uint32_t>(reinterpret_cast<const uint8_t*>(message.data()));
        WARN("WebRTC: HandleInput - unhandled message type 0x%08X (size=%zu)", msgType, message.size());
    }
}

void WebRTCServer::HandleMic(const rtc::binary& message) {
    if (message.size() < sizeof(MicPacketHeader) || chRdy < NUM_CH) {
        if (message.size() < sizeof(MicPacketHeader)) {
            WARN("WebRTC: HandleMic - message too small (%zu < %zu)", message.size(), sizeof(MicPacketHeader));
        }
        return;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(message.data());
    const auto* header = reinterpret_cast<const MicPacketHeader*>(bytes);
    if (header->magic != MSG_MIC_DATA) {
        WARN("WebRTC: HandleMic - unexpected magic 0x%08X (expected 0x%08X, size=%zu)",
            header->magic, MSG_MIC_DATA, message.size());
        return;
    }

    const size_t expectedLen = sizeof(MicPacketHeader) + header->dataLength;
    if (header->dataLength == 0 || expectedLen != message.size()) {
        WARN("WebRTC: HandleMic - invalid packet size (type=%u id=%u expected=%zu got=%zu)",
            static_cast<unsigned>(header->packetType), header->packetId, expectedLen, message.size());
        return;
    }

    if (header->packetType != PKT_DATA && header->packetType != PKT_FEC) {
        WARN("WebRTC: HandleMic - unknown packet type=%u (id=%u)",
            static_cast<unsigned>(header->packetType), header->packetId);
        return;
    }

    if (!callbacks_.onMicData) {
        DBG("WebRTC: HandleMic - no mic callback registered, dropping packet (%zu bytes)", message.size());
        return;
    }

    std::vector<uint8_t> dataPacketToDeliver;
    std::vector<uint8_t> recoveredPacketToDeliver;
    const uint8_t groupSize = std::clamp<uint8_t>(
        header->fecGroupSize ? header->fecGroupSize : MIC_FEC_GROUP_SIZE,
        1,
        16);
    const int64_t nowMs = GetTimestamp() / 1000;

    {
        std::lock_guard<std::mutex> lk(micFecMutex_);

        auto tryRecoverGroup = [&](uint32_t groupStart, MicFecGroupState& group) -> std::vector<uint8_t> {
            if (!group.hasFec || group.fecPayload.empty()) return {};

            std::vector<uint32_t> missing;
            missing.reserve(group.groupSize);
            for (uint32_t i = 0; i < group.groupSize; i++) {
                uint32_t packetId = groupStart + i;
                if (!group.dataPackets.contains(packetId)) missing.push_back(packetId);
            }

            if (missing.size() != 1 || group.dataPackets.size() < static_cast<size_t>(group.groupSize - 1)) {
                return {};
            }

            std::vector<uint8_t> recovered = group.fecPayload;
            for (uint32_t i = 0; i < group.groupSize; i++) {
                uint32_t packetId = groupStart + i;
                auto it = group.dataPackets.find(packetId);
                if (it == group.dataPackets.end()) continue;
                const auto& pkt = it->second;
                const size_t limit = std::min(recovered.size(), pkt.size());
                for (size_t j = 0; j < limit; j++) recovered[j] ^= pkt[j];
            }

            if (recovered.size() < sizeof(MicPacketHeader)) return {};

            const auto* recHeader = reinterpret_cast<const MicPacketHeader*>(recovered.data());
            if (recHeader->magic != MSG_MIC_DATA || recHeader->packetType != PKT_DATA) return {};
            if (recHeader->packetId != missing[0]) return {};

            const size_t recLen = sizeof(MicPacketHeader) + recHeader->dataLength;
            if (recHeader->dataLength == 0 || recLen > recovered.size()) return {};

            recovered.resize(recLen);
            return recovered;
        };

        if (header->packetType == PKT_DATA) {
            if (micSeenPacketIds_.contains(header->packetId)) return;

            micSeenPacketIds_.insert(header->packetId);
            if (micSeenPacketIds_.size() > 4096) {
                while (micSeenPacketIds_.size() > 2048) {
                    auto it = micSeenPacketIds_.begin();
                    if (it == micSeenPacketIds_.end()) break;
                    micSeenPacketIds_.erase(it);
                }
            }

            const uint32_t groupStart = header->packetId - (header->packetId % groupSize);
            auto& group = micFecGroups_[groupStart];
            group.groupSize = groupSize;
            group.updatedMs = nowMs;
            group.dataPackets[header->packetId] = std::vector<uint8_t>(bytes, bytes + message.size());

            dataPacketToDeliver = group.dataPackets[header->packetId];

            auto recovered = tryRecoverGroup(groupStart, group);
            if (!recovered.empty()) {
                const auto* recHeader = reinterpret_cast<const MicPacketHeader*>(recovered.data());
                if (!micSeenPacketIds_.contains(recHeader->packetId)) {
                    micSeenPacketIds_.insert(recHeader->packetId);
                    recoveredPacketToDeliver = std::move(recovered);
                    LOG("WebRTC: Mic FEC recovered packet id=%u group=%u", recHeader->packetId, groupStart);
                }
                micFecGroups_.erase(groupStart);
            }
        } else {
            const uint32_t groupStart = header->packetId;
            auto& group = micFecGroups_[groupStart];
            group.groupSize = groupSize;
            group.updatedMs = nowMs;
            group.hasFec = true;
            group.fecPayload.assign(bytes + sizeof(MicPacketHeader), bytes + message.size());

            auto recovered = tryRecoverGroup(groupStart, group);
            if (!recovered.empty()) {
                const auto* recHeader = reinterpret_cast<const MicPacketHeader*>(recovered.data());
                if (!micSeenPacketIds_.contains(recHeader->packetId)) {
                    micSeenPacketIds_.insert(recHeader->packetId);
                    recoveredPacketToDeliver = std::move(recovered);
                    LOG("WebRTC: Mic FEC recovered packet id=%u group=%u", recHeader->packetId, groupStart);
                }
                micFecGroups_.erase(groupStart);
            }
        }

        if (micFecGroups_.size() > 128) {
            for (auto it = micFecGroups_.begin(); it != micFecGroups_.end();) {
                if (nowMs - it->second.updatedMs > 2000) it = micFecGroups_.erase(it);
                else ++it;
            }
        }
    }

    if (!dataPacketToDeliver.empty()) {
        callbacks_.onMicData(dataPacketToDeliver.data(), dataPacketToDeliver.size());
        micRecv++;
    }
    if (!recoveredPacketToDeliver.empty()) {
        callbacks_.onMicData(recoveredPacketToDeliver.data(), recoveredPacketToDeliver.size());
        micRecv++;
    }
}

void WebRTCServer::OnChannelOpen(const std::string& label, uint64_t epoch) {
    int ready = ++chRdy;
    LOG("WebRTC: Channel '%s' open (epoch=%llu active=%llu ready=%d/%d conn=%d fpsRecv=%d)",
        label.c_str(), epoch, peerEpoch.load(), ready, NUM_CH, conn.load() ? 1 : 0, fpsRecv.load() ? 1 : 0);
    if (ready == NUM_CH) {
        conn = true;
        needsKey = true;
        lastPing = GetTimestamp() / 1000;
        overflow = 0;
        connCount++;
        LOG("WebRTC: Connection #%llu established (epoch=%llu)", connCount.load(), epoch);
        SendHostInfo();
        SendCodecCaps();
        SendMonitorList();
        SendVersion();
        if (callbacks_.onConnected) callbacks_.onConnected();
    }
}

void WebRTCServer::OnChannelClose(const std::string& label, uint64_t epoch) {
    LOG("WebRTC: Channel '%s' closed (epoch=%llu active=%llu)",
        label.c_str(), epoch, peerEpoch.load());
    if (epoch != peerEpoch.load()) {
        WARN("WebRTC: Stale channel close from previous peer (channel=%s epoch=%llu active=%llu) - ignoring",
            label.c_str(), epoch, peerEpoch.load());
        return;
    }
    chRdy = 0;
    bool wasConn = conn.exchange(false);
    fpsRecv = false;
    overflow = 0;
    if (wasConn) LOG("WebRTC: Connection closed (epoch=%llu)", epoch);
    if (callbacks_.onDisconnect) callbacks_.onDisconnect();
}

void WebRTCServer::SetupChannel(std::shared_ptr<rtc::DataChannel>& channel, bool enableBufferedDrain,
                                 std::function<void(const rtc::binary&)> handler,
                                 uint64_t epoch) {
    if (!channel) return;
    std::string label = channel->label();
    LOG("WebRTC: Setup channel '%s' (epoch=%llu)", label.c_str(), epoch);

    channel->setBufferedAmountLowThreshold(BUF_LOW);
    channel->onOpen([this, label, epoch] { OnChannelOpen(label, epoch); });
    channel->onClosed([this, label, epoch] { OnChannelClose(label, epoch); });
    channel->onError([label](std::string e) { ERR("WebRTC: Channel '%s' error: %s", label.c_str(), e.c_str()); });

    if (handler) {
        channel->onMessage([handler](auto payload) {
            if (auto* binary = std::get_if<rtc::binary>(&payload)) handler(*binary);
        });
    }

    if (enableBufferedDrain) {
        channel->onBufferedAmountLow([this, label] {
            if (label == "video") DrainVideo();
            else if (label == "audio") DrainAudio();
        });
    }
}

void WebRTCServer::DrainVideo() {
    std::shared_ptr<rtc::DataChannel> videoChannel;
    { std::lock_guard<std::mutex> lk(channelMutex_); videoChannel = videoDataChannel_; }
    std::lock_guard<std::mutex> lk(sendMutex_);
    DrainQueuedChannel(videoChannel, videoPacketQueue_, VID_BUF, videoErr, &overflow, &needsKey);
}

void WebRTCServer::DrainAudio() {
    std::shared_ptr<rtc::DataChannel> audioChannel;
    { std::lock_guard<std::mutex> lk(channelMutex_); audioChannel = audioDataChannel_; }
    std::lock_guard<std::mutex> lk(sendMutex_);
    DrainQueuedChannel(audioChannel, audioPacketQueue_, AUD_BUF, audioErr);
}

void WebRTCServer::Reset() {
    std::shared_ptr<rtc::DataChannel> controlChannel, videoChannel, audioChannel, inputChannel, micChannel;
    std::shared_ptr<rtc::PeerConnection> localPc;
    {
        std::lock_guard<std::mutex> lk(channelMutex_);
        controlChannel = std::move(controlDataChannel_);
        videoChannel = std::move(videoDataChannel_);
        audioChannel = std::move(audioDataChannel_);
        inputChannel = std::move(inputDataChannel_);
        micChannel = std::move(micDataChannel_);
        localPc = std::move(peerConnection_);
    }

    conn = false; fpsRecv = false; gathered = false; hasDesc = false;
    chRdy = 0; overflow = 0; lastPing = 0;
    audioPktId = 0;

    { std::lock_guard<std::mutex> lk(descriptionMutex_); localDescription_.clear(); }
    {
        std::lock_guard<std::mutex> lk(sendMutex_);
        while (!videoPacketQueue_.empty()) videoPacketQueue_.pop();
        while (!audioPacketQueue_.empty()) audioPacketQueue_.pop();
        audioFecCount_ = 0;
        audioFecGroupStart_ = 0;
        for (auto& pkt : audioFecPackets_) pkt.clear();
    }
    {
        std::lock_guard<std::mutex> lk(micFecMutex_);
        micFecGroups_.clear();
        micSeenPacketIds_.clear();
    }

    // Close old channels/PC in a detached thread to avoid blocking the caller
    std::thread([controlChannel = std::move(controlChannel),
                 videoChannel = std::move(videoChannel),
                 audioChannel = std::move(audioChannel),
                 inputChannel = std::move(inputChannel),
                 micChannel = std::move(micChannel),
                 localPc = std::move(localPc)]() mutable {
        auto close = [](auto& ch) {
            try { if (ch && ch->isOpen()) ch->close(); } catch (...) { DBG("WebRTC: Channel close exception during reset"); }
            ch.reset();
        };
        close(controlChannel); close(videoChannel); close(audioChannel); close(inputChannel); close(micChannel);
        try { if (localPc) localPc->close(); } catch (...) { DBG("WebRTC: PeerConnection close exception during reset"); }
        localPc.reset();
    }).detach();
}

void WebRTCServer::SetupPeerConnection() {
    uint64_t epoch = peerEpoch.fetch_add(1) + 1;

    std::shared_ptr<rtc::DataChannel> ctrl;
    std::shared_ptr<rtc::PeerConnection> existingPc;
    {
        std::lock_guard<std::mutex> lk(channelMutex_);
        ctrl = controlDataChannel_;
        existingPc = peerConnection_;
    }

    if (existingPc && ctrl && ctrl->isOpen()) {
        try {
            uint8_t k[4]; WritePod<uint32_t>(k, MSG_KICKED);
            ctrl->send((const std::byte*)k, 4);
            std::this_thread::sleep_for(50ms);
        } catch (const std::exception& e) {
            DBG("WebRTC: Failed to send KICKED message: %s", e.what());
        } catch (...) {
            DBG("WebRTC: Failed to send KICKED message");
        }
    }

    Reset();
    needsKey = true;
    LOG("WebRTC: Creating peer connection (epoch=%llu)", epoch);
    auto newPc = std::make_shared<rtc::PeerConnection>(rtcConfig_);
    {
        std::lock_guard<std::mutex> lk(channelMutex_);
        peerConnection_ = newPc;
    }

    newPc->onLocalDescription([this, epoch](rtc::Description d) {
        LOG("WebRTC: Local description ready (epoch=%llu)", epoch);
        std::lock_guard<std::mutex> lk(descriptionMutex_);
        localDescription_ = std::string(d);
        hasDesc = true;
        descriptionCv_.notify_all();
    });

    newPc->onLocalCandidate([this, epoch](rtc::Candidate) {
        DBG("WebRTC: Local candidate gathered (epoch=%llu)", epoch);
        descriptionCv_.notify_all();
    });

    newPc->onStateChange([this, epoch](auto s) {
        LOG("WebRTC: Peer state=%s (epoch=%llu active=%llu ch=%d fpsRecv=%d conn=%d)",
            ToPeerStateString(s), epoch, peerEpoch.load(), chRdy.load(), fpsRecv.load() ? 1 : 0, conn.load() ? 1 : 0);
        if (epoch != peerEpoch.load()) {
            DBG("WebRTC: Stale state change from previous peer (epoch=%llu active=%llu state=%s) - ignoring",
                epoch, peerEpoch.load(), ToPeerStateString(s));
            return;
        }
        bool now = s == rtc::PeerConnection::State::Connected;
        if (now && !conn) { needsKey = true; lastPing = GetTimestamp() / 1000; }
        if (!now && conn) { fpsRecv = false; chRdy = 0; if (callbacks_.onDisconnect) callbacks_.onDisconnect(); }
        conn = now;
    });

    newPc->onGatheringStateChange([this, epoch](auto s) {
        LOG("WebRTC: Gathering state=%s (epoch=%llu)", ToGatherStateString(s), epoch);
        if (s == rtc::PeerConnection::GatheringState::Complete) {
            gathered = true;
            descriptionCv_.notify_all();
        }
    });

    newPc->onDataChannel([this, epoch](const std::shared_ptr<rtc::DataChannel>& ch) {
        const std::string label = ch->label();
        LOG("WebRTC: Data channel announced '%s' (epoch=%llu active=%llu)", label.c_str(), epoch, peerEpoch.load());

        const auto bind = [&](std::shared_ptr<rtc::DataChannel>& slot, bool drain, ChannelHandler handler = nullptr) {
            std::lock_guard<std::mutex> lk(channelMutex_);
            slot = ch;
            SetupChannel(slot, drain, std::move(handler), epoch);
        };

        if (label == "control") bind(controlDataChannel_, false, [this](const rtc::binary& m) { HandleCtrl(m); });
        else if (label == "video") bind(videoDataChannel_, true);
        else if (label == "audio") bind(audioDataChannel_, true);
        else if (label == "input") bind(inputDataChannel_, false, [this](const rtc::binary& m) { HandleInput(m); });
        else if (label == "mic") bind(micDataChannel_, false, [this](const rtc::binary& m) { HandleMic(m); });
    });
}

bool WebRTCServer::IsStale() {
    if (!conn) return false;
    int64_t now = GetTimestamp() / 1000;
    if (lastPing > 0 && now - lastPing > 3000) {
        WARN("WebRTC: Connection stale - no ping for %lld ms (overflow=%d)",
            now - lastPing.load(), overflow.load());
        return true;
    }
    if (overflow.load() >= 10) {
        WARN("WebRTC: Connection stale - overflow count %d >= 10 (lastPing=%lld ms ago)",
            overflow.load(), lastPing > 0 ? now - lastPing.load() : -1);
        return true;
    }
    return false;
}

bool WebRTCServer::IsCongested() const {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(sendMutex_));
    return videoPacketQueue_.size() > kVideoQueueCongestionThreshold;
}

void WebRTCServer::LogStats() {
    int64_t now = GetTimestamp() / 1000;
    if (now - lastStatLog.load() < 60000) return;
    lastStatLog.store(now);
    if (conn || videoSent > 0)
        LOG("WebRTC Stats: v=%llu/%llu a=%llu/%llu ctrl=%llu/%llu in=%llu mic=%llu conn=%llu overflow=%d",
            videoSent.load(), videoErr.load(), audioSent.load(), audioErr.load(),
            ctrlSent.load(), ctrlRecv.load(), inputRecv.load(), micRecv.load(), connCount.load(),
            overflow.load());
}

WebRTCServer::WebRTCServer() {
    for (const char* s : {"stun:stun.l.google.com:19302", "stun:stun1.l.google.com:19302",
            "stun:stun2.l.google.com:19302", "stun:stun3.l.google.com:19302",
            "stun:stun4.l.google.com:19302", "stun:stun.cloudflare.com:3478",
            "stun:stun.services.mozilla.com:3478"})
        rtcConfig_.iceServers.push_back(rtc::IceServer(s));
    rtcConfig_.portRangeBegin = 50000;
    rtcConfig_.portRangeEnd = 50020;
    rtcConfig_.enableIceTcp = false;
    LOG("WebRTC: Server initialized");
    SetupPeerConnection();
}

WebRTCServer::~WebRTCServer() {
    LogStats();
    Reset();
}

void WebRTCServer::Init(WebRTCCallbacks c) { callbacks_ = std::move(c); }

void WebRTCServer::Shutdown() {
    Reset();
}

std::string WebRTCServer::GetLocal() {
    std::unique_lock<std::mutex> lk(descriptionMutex_);
    descriptionCv_.wait_for(lk, 200ms, [this] { return hasDesc.load(); });
    descriptionCv_.wait_for(lk, 1500ms, [this] { return gathered.load(); });
    std::string sdp = localDescription_;
    if (!sdp.empty() && !HasPublicIceCandidate(sdp)) {
        WARN("WebRTC: SDP has no srflx/relay candidate. WAN may fail without UDP port-forwarding or TURN.");
    }
    return sdp;
}

void WebRTCServer::SetRemote(const std::string& sdp, const std::string& type) {
    LOG("WebRTC: SetRemote (type=%s)", type.c_str());
    if (type == "offer") SetupPeerConnection();

    std::shared_ptr<rtc::PeerConnection> localPc;
    {
        std::lock_guard<std::mutex> lk(channelMutex_);
        localPc = peerConnection_;
    }
    if (!localPc) return;

    localPc->setRemoteDescription(rtc::Description(sdp, type));
    if (type == "offer") localPc->setLocalDescription();
}

bool WebRTCServer::SendCursorShape(CursorType ct) {
    if (!IsStreaming()) return false;
    uint8_t buf[5];
    WritePod<uint32_t>(buf, MSG_CURSOR_SHAPE);
    buf[4] = static_cast<uint8_t>(ct);
    return SendCtrl(buf, sizeof(buf));
}

bool WebRTCServer::Send(const EncodedFrame& frame) {
    if (!IsStreaming()) {
        DBG("WebRTC: Send skipped - not streaming (conn=%d fpsRecv=%d chRdy=%d)",
            conn.load() ? 1 : 0, fpsRecv.load() ? 1 : 0, chRdy.load());
        return false;
    }
    if (IsStale()) {
        WARN("WebRTC: Connection stale (lastPing=%lld overflow=%d) - resetting",
            lastPing.load(), overflow.load());
        Reset();
        if (callbacks_.onDisconnect) callbacks_.onDisconnect();
        return false;
    }

    size_t frameSizeBytes = frame.data.size();
    if (!frameSizeBytes || frameSizeBytes > DATA_CHUNK * 65535) {
        ERR("WebRTC: Send invalid frame size: %zu (ts=%lld, key=%d)", frameSizeBytes, frame.ts, frame.isKey ? 1 : 0);
        return false;
    }

    size_t chunkCount = (frameSizeBytes + DATA_CHUNK - 1) / DATA_CHUNK;
    uint32_t frameId = frmId++;
    if (chunkCount > 65535) {
        ERR("WebRTC: Send too many chunks: %zu for frame %u (size=%zu)", chunkCount, frameId, frameSizeBytes);
        return false;
    }

    constexpr uint8_t PKT_DATA = 0;
    constexpr uint8_t PKT_FEC = 1;

    std::shared_ptr<rtc::DataChannel> videoChannel;
    {
        std::lock_guard<std::mutex> lk(channelMutex_);
        videoChannel = videoDataChannel_;
    }

    size_t queuedBefore = 0;
    {
        std::lock_guard<std::mutex> lk(sendMutex_);
        queuedBefore = videoPacketQueue_.size();
    }

    const size_t bufferedNow = (videoChannel && videoChannel->isOpen()) ? videoChannel->bufferedAmount() : 0;
    const bool bypassFec = !frame.isKey &&
        (queuedBefore >= kVideoQueueFecBypassThreshold || bufferedNow >= VID_BUF / 2);
    const uint8_t fecGroupSize = static_cast<uint8_t>(10);

    DBG("WebRTC: Send frame=%u ts=%lld key=%d size=%zu chunks=%zu encUs=%lld q=%zu buf=%zu fec=%s gsz=%u",
        frameId, frame.ts, frame.isKey ? 1 : 0, frameSizeBytes, chunkCount, frame.encUs,
        queuedBefore, bufferedNow, bypassFec ? "off" : "on", static_cast<unsigned>(fecGroupSize));

    PacketHeader header = {
        frame.ts,
        static_cast<uint32_t>(frame.encUs),
        frameId,
        static_cast<uint32_t>(frameSizeBytes),
        0,
        static_cast<uint16_t>(chunkCount),
        0,
        static_cast<uint16_t>(DATA_CHUNK),
        frame.isKey ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
        PKT_DATA,
        fecGroupSize
    };

    {
        std::lock_guard<std::mutex> lk(sendMutex_);
        size_t droppedPackets = 0;
        while (videoPacketQueue_.size() > kVideoQueueMaxPackets) {
            videoPacketQueue_.pop();
            droppedPackets++;
        }
        if (droppedPackets > 0) {
            while (videoPacketQueue_.size() > kVideoQueueTrimTargetPackets) {
                videoPacketQueue_.pop();
                droppedPackets++;
            }

            if (!needsKey.exchange(true, std::memory_order_acq_rel)) {
                WARN("WebRTC: Trimmed video queue (%zu packets dropped, %zu remaining); requesting recovery keyframe",
                    droppedPackets, videoPacketQueue_.size());
            }
            ERR("WebRTC: VIDEO QUEUE OVERFLOW - dropped %zu packets (frame=%u, key=%d, size=%zu)",
                droppedPackets, frameId, frame.isKey ? 1 : 0, frameSizeBytes);
        }

        for (size_t groupIndex = 0; groupIndex * static_cast<size_t>(fecGroupSize) < chunkCount; groupIndex++) {
            size_t startChunkIndex = groupIndex * static_cast<size_t>(fecGroupSize);
            size_t endChunkIndex = std::min(startChunkIndex + static_cast<size_t>(fecGroupSize), chunkCount);
            size_t parityLen = 0;
            std::array<uint8_t, DATA_CHUNK> parity{};

            for (size_t chunkIndex = startChunkIndex; chunkIndex < endChunkIndex; chunkIndex++) {
                header.chunkIndex = static_cast<uint16_t>(chunkIndex);
                size_t chunkOffset = chunkIndex * DATA_CHUNK;
                size_t chunkLength = std::min(DATA_CHUNK, frameSizeBytes - chunkOffset);

                header.chunkBytes = static_cast<uint16_t>(chunkLength);
                header.packetType = PKT_DATA;

                std::vector<uint8_t> packet(HDR_SZ + chunkLength);
                memcpy(packet.data(), &header, HDR_SZ);
                memcpy(packet.data() + HDR_SZ, frame.data.data() + chunkOffset, chunkLength);
                videoPacketQueue_.push(std::move(packet));

                parityLen = std::max(parityLen, chunkLength);
                const uint8_t* source = frame.data.data() + chunkOffset;
                for (size_t j = 0; j < chunkLength; j++) parity[j] ^= source[j];
            }

            if (!bypassFec && endChunkIndex - startChunkIndex == static_cast<size_t>(fecGroupSize) && parityLen > 0) {
                header.chunkIndex = static_cast<uint16_t>(groupIndex);
                header.chunkBytes = static_cast<uint16_t>(parityLen);
                header.packetType = PKT_FEC;

                std::vector<uint8_t> packet(HDR_SZ + parityLen);
                memcpy(packet.data(), &header, HDR_SZ);
                memcpy(packet.data() + HDR_SZ, parity.data(), parityLen);
                videoPacketQueue_.push(std::move(packet));
            }
        }
    }
    DrainVideo();
    videoSent++;
    LogStats();
    return true;
}

bool WebRTCServer::SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples) {
    if (!IsStreaming() || data.empty() || data.size() > 4000) {
        if (data.empty()) {
            DBG("WebRTC: SendAudio skipped - empty data");
        } else if (data.size() > 4000) {
            WARN("WebRTC: SendAudio skipped - data too large: %zu bytes", data.size());
        }
        return false;
    }

    std::vector<std::vector<uint8_t>> outgoing;
    outgoing.reserve(2);

    {
        std::lock_guard<std::mutex> lk(sendMutex_);

        AudioPacketHeader dataHeader{};
        dataHeader.magic = MSG_AUDIO_DATA;
        dataHeader.timestamp = ts;
        dataHeader.packetId = audioPktId.fetch_add(1, std::memory_order_acq_rel);
        dataHeader.samples = static_cast<uint16_t>(samples);
        dataHeader.dataLength = static_cast<uint16_t>(data.size());
        dataHeader.packetType = PKT_DATA;
        dataHeader.fecGroupSize = AUDIO_FEC_GROUP_SIZE;
        dataHeader.reserved = 0;

        std::vector<uint8_t> dataPacket(sizeof(AudioPacketHeader) + data.size());
        memcpy(dataPacket.data(), &dataHeader, sizeof(AudioPacketHeader));
        memcpy(dataPacket.data() + sizeof(AudioPacketHeader), data.data(), data.size());
        outgoing.push_back(dataPacket);

        static_assert(AUDIO_FEC_GROUP_SIZE > 0, "AUDIO_FEC_GROUP_SIZE must be > 0");

        if (audioFecCount_ == 0) audioFecGroupStart_ = dataHeader.packetId;
        audioFecPackets_[audioFecCount_++] = std::move(dataPacket);

        if (audioFecCount_ == AUDIO_FEC_GROUP_SIZE) {
            size_t parityLen = 0;
            for (const auto& packet : audioFecPackets_) parityLen = std::max(parityLen, packet.size());

            if (parityLen > 0 && parityLen <= 65535) {
                std::vector<uint8_t> parity(parityLen, 0);
                for (const auto& packet : audioFecPackets_) {
                    const size_t limit = std::min(parity.size(), packet.size());
                    for (size_t i = 0; i < limit; i++) parity[i] ^= packet[i];
                }

                AudioPacketHeader fecHeader{};
                fecHeader.magic = MSG_AUDIO_DATA;
                fecHeader.timestamp = 0;
                fecHeader.packetId = audioFecGroupStart_;
                fecHeader.samples = 0;
                fecHeader.dataLength = static_cast<uint16_t>(parity.size());
                fecHeader.packetType = PKT_FEC;
                fecHeader.fecGroupSize = AUDIO_FEC_GROUP_SIZE;
                fecHeader.reserved = 0;

                std::vector<uint8_t> fecPacket(sizeof(AudioPacketHeader) + parity.size());
                memcpy(fecPacket.data(), &fecHeader, sizeof(AudioPacketHeader));
                memcpy(fecPacket.data() + sizeof(AudioPacketHeader), parity.data(), parity.size());
                outgoing.push_back(std::move(fecPacket));
            }

            for (auto& packet : audioFecPackets_) packet.clear();
            audioFecCount_ = 0;
        }

        for (auto& packet : outgoing) {
            PushBoundedQueue(audioPacketQueue_, static_cast<size_t>(6), std::move(packet));
        }
    }

    DrainAudio();
    audioSent++;
    return true;
}

void WebRTCServer::GetStats(uint64_t& vS, uint64_t& vE, uint64_t& aS, uint64_t& aE, uint64_t& c) {
    vS = videoSent.load(); vE = videoErr.load();
    aS = audioSent.load(); aE = audioErr.load();
    c = connCount.load();
}
