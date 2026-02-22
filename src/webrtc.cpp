#include "webrtc.hpp"
#include <cstring>

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
    switch (s) {
        case rtc::PeerConnection::State::New: return "new";
        case rtc::PeerConnection::State::Connecting: return "connecting";
        case rtc::PeerConnection::State::Connected: return "connected";
        case rtc::PeerConnection::State::Disconnected: return "disconnected";
        case rtc::PeerConnection::State::Failed: return "failed";
        case rtc::PeerConnection::State::Closed: return "closed";
        default: return "unknown";
    }
}

const char* ToGatherStateString(rtc::PeerConnection::GatheringState s) {
    switch (s) {
        case rtc::PeerConnection::GatheringState::New: return "new";
        case rtc::PeerConnection::GatheringState::InProgress: return "in-progress";
        case rtc::PeerConnection::GatheringState::Complete: return "complete";
        default: return "unknown";
    }
}
}

bool WebRTCServer::SendCtrl(const void* d, size_t len) {
    std::shared_ptr<rtc::DataChannel> ctrl;
    {
        std::lock_guard<std::mutex> lk(chMtx);
        ctrl = dcCtrl;
    }
    if (!ctrl || !ctrl->isOpen()) return false;
    try {
        ctrl->send((const std::byte*)d, len);
        ctrlSent++;
        return true;
    } catch (...) {
        WARN("WebRTC: SendCtrl failed");
        return false;
    }
}

void WebRTCServer::SendHostInfo() {
    uint8_t buf[6];
    WritePod<uint32_t>(buf, MSG_HOST_INFO);
    WritePod<uint16_t>(buf + 4, static_cast<uint16_t>(cb.getHostFps ? cb.getHostFps() : 60));
    SendCtrl(buf, sizeof(buf));
}

void WebRTCServer::SendMonitorList() {
    std::vector<uint8_t> buf;
    std::lock_guard<std::mutex> lk(g_monitorsMutex);

    buf.resize(6 + g_monitors.size() * 74);
    size_t o = 0;
    WritePod<uint32_t>(buf.data() + o, MSG_MONITOR_LIST); o += 4;
    buf[o++] = static_cast<uint8_t>(g_monitors.size());
    buf[o++] = static_cast<uint8_t>(cb.getMonitor ? cb.getMonitor() : 0);

    for (const auto& m : g_monitors) {
        buf[o++] = static_cast<uint8_t>(m.index);
        WritePod<uint16_t>(buf.data() + o, static_cast<uint16_t>(m.width));
        WritePod<uint16_t>(buf.data() + o + 2, static_cast<uint16_t>(m.height));
        WritePod<uint16_t>(buf.data() + o + 4, static_cast<uint16_t>(m.refreshRate));
        o += 6;
        buf[o++] = m.isPrimary ? 1 : 0;
        size_t nl = std::min(m.name.size(), static_cast<size_t>(63));
        buf[o++] = static_cast<uint8_t>(nl);
        memcpy(&buf[o], m.name.c_str(), nl);
        o += nl;
    }
    buf.resize(o);
    SendCtrl(buf.data(), buf.size());
}

void WebRTCServer::SendCodecCaps() {
    uint8_t buf[5] = {};
    WritePod<uint32_t>(buf, MSG_CODEC_CAPS);
    buf[4] = cb.getCodecCaps ? cb.getCodecCaps() : 0x07;
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

void WebRTCServer::HandleCtrl(const rtc::binary& m) {
    if (m.size() < 4 || chRdy < NUM_CH) return;
    ctrlRecv++;

    uint32_t magic = ReadPod<uint32_t>(reinterpret_cast<const uint8_t*>(m.data()));
    switch (magic) {
        case MSG_PING:
            if (m.size() == 16) {
                lastPing = GetTimestamp() / 1000;
                overflow = 0;
                uint8_t r[24];
                memcpy(r, m.data(), 16);
                WritePod<uint64_t>(r + 16, static_cast<uint64_t>(GetTimestamp()));
                SendCtrl(r, sizeof(r));
            }
            break;

        case MSG_FPS_SET:
            if (m.size() == 7) {
                uint16_t fps = ReadPod<uint16_t>(reinterpret_cast<const uint8_t*>(m.data()) + 4);
                uint8_t mode = static_cast<uint8_t>(m[6]);
                if (fps >= 1 && fps <= 240 && mode <= 2) {
                    int act = (mode == 1 && cb.getHostFps) ? cb.getHostFps() : fps;
                    fpsRecv = true;
                    LOG("WebRTC: FPS set to %d (mode=%d)", act, mode);
                    if (cb.onFpsChange) cb.onFpsChange(act, mode);
                    uint8_t a[7];
                    WritePod<uint32_t>(a, MSG_FPS_ACK);
                    WritePod<uint16_t>(a + 4, static_cast<uint16_t>(act));
                    a[6] = mode;
                    SendCtrl(a, sizeof(a));
                }
            }
            break;

        case MSG_CODEC_SET:
            if (m.size() == 5 && static_cast<uint8_t>(m[4]) <= 2) {
                CodecType nc = static_cast<CodecType>(static_cast<uint8_t>(m[4]));
                bool ok = !cb.onCodecChange || cb.onCodecChange(nc);
                if (ok) { curCodec = nc; needsKey = true; }
                uint8_t a[5];
                WritePod<uint32_t>(a, MSG_CODEC_ACK);
                a[4] = static_cast<uint8_t>(curCodec.load());
                SendCtrl(a, sizeof(a));
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
            if (m.size() == 5 && cb.onMonitorChange) {
                if (cb.onMonitorChange(static_cast<int>(static_cast<uint8_t>(m[4])))) {
                    needsKey = true;
                    SendMonitorList();
                    SendHostInfo();
                }
            }
            break;

        case MSG_CLIPBOARD_DATA:
            if (m.size() >= 8 && cb.setClipboard) {
                uint32_t len = ReadPod<uint32_t>(reinterpret_cast<const uint8_t*>(m.data()) + 4);
                if (len > 0 && m.size() >= 8 + len && len <= 1048576)
                    cb.setClipboard(std::string(reinterpret_cast<const char*>(m.data()) + 8, len));
            }
            break;

        case MSG_CLIPBOARD_GET:
            if (cb.getClipboard) {
                std::string text = cb.getClipboard();
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
            if (m.size() == 5 && cb.onCursorCapture)
                cb.onCursorCapture(static_cast<uint8_t>(m[4]) != 0);
            break;

        case MSG_AUDIO_ENABLE:
            if (m.size() == 5 && cb.onAudioEnable)
                cb.onAudioEnable(static_cast<uint8_t>(m[4]) != 0);
            break;

        case MSG_MIC_ENABLE:
            if (m.size() == 5 && cb.onMicEnable)
                cb.onMicEnable(static_cast<uint8_t>(m[4]) != 0);
            break;
    }
}

void WebRTCServer::HandleInput(const rtc::binary& m) {
    if (m.size() < 4 || chRdy < NUM_CH || !cb.input) return;
    inputRecv++;
    [[maybe_unused]] const bool handled = cb.input->HandleMessage(reinterpret_cast<const uint8_t*>(m.data()), m.size());
}

void WebRTCServer::HandleMic(const rtc::binary& m) {
    if (m.size() < sizeof(MicPacketHeader) || chRdy < NUM_CH) return;
    if (ReadPod<uint32_t>(reinterpret_cast<const uint8_t*>(m.data())) == MSG_MIC_DATA) {
        micRecv++;
        if (cb.onMicData) cb.onMicData(reinterpret_cast<const uint8_t*>(m.data()), m.size());
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
        if (cb.onConnected) cb.onConnected();
    }
}

void WebRTCServer::OnChannelClose(const std::string& label, uint64_t epoch) {
    chRdy = 0;
    bool wasConn = conn.exchange(false);
    fpsRecv = false;
    overflow = 0;
    LOG("WebRTC: Channel '%s' closed (epoch=%llu active=%llu wasConn=%d)",
        label.c_str(), epoch, peerEpoch.load(), wasConn ? 1 : 0);
    if (epoch != peerEpoch.load()) {
        WARN("WebRTC: Stale channel close from previous peer (channel=%s epoch=%llu active=%llu)",
            label.c_str(), epoch, peerEpoch.load());
    }
    if (wasConn) LOG("WebRTC: Connection closed (epoch=%llu)", epoch);
    if (cb.onDisconnect) cb.onDisconnect();
}

void WebRTCServer::SetupChannel(std::shared_ptr<rtc::DataChannel>& ch, bool drain,
                                 std::function<void(const rtc::binary&)> handler,
                                 uint64_t epoch) {
    if (!ch) return;
    std::string label = ch->label();
    LOG("WebRTC: Setup channel '%s' (epoch=%llu)", label.c_str(), epoch);

    ch->setBufferedAmountLowThreshold(BUF_LOW);
    ch->onOpen([this, label, epoch] { OnChannelOpen(label, epoch); });
    ch->onClosed([this, label, epoch] { OnChannelClose(label, epoch); });
    ch->onError([label](std::string e) { ERR("WebRTC: Channel '%s' error: %s", label.c_str(), e.c_str()); });

    if (handler) {
        ch->onMessage([handler](auto d) {
            if (auto* b = std::get_if<rtc::binary>(&d)) handler(*b);
        });
    }

    if (drain) {
        ch->onBufferedAmountLow([this, label] {
            if (label == "video") DrainVideo();
            else if (label == "audio") DrainAudio();
        });
    }
}

void WebRTCServer::DrainVideo() {
    std::shared_ptr<rtc::DataChannel> video;
    {
        std::lock_guard<std::mutex> lk(chMtx);
        video = dcVid;
    }
    if (!video || !video->isOpen()) return;
    std::lock_guard<std::mutex> lk(sendMtx);
    while (!vidQ.empty() && video->bufferedAmount() <= VID_BUF) {
        try {
            video->send((const std::byte*)vidQ.front().data(), vidQ.front().size());
        } catch (...) {
            videoErr++;
            overflow++;
            needsKey = true;
        }
        vidQ.pop();
    }
}

void WebRTCServer::DrainAudio() {
    std::shared_ptr<rtc::DataChannel> audio;
    {
        std::lock_guard<std::mutex> lk(chMtx);
        audio = dcAud;
    }
    if (!audio || !audio->isOpen()) return;
    std::lock_guard<std::mutex> lk(sendMtx);
    while (!audQ.empty() && audio->bufferedAmount() <= AUD_BUF) {
        try {
            audio->send((const std::byte*)audQ.front().data(), audQ.front().size());
        } catch (...) {
            audioErr++;
        }
        audQ.pop();
    }
}

void WebRTCServer::Reset() {
    std::shared_ptr<rtc::DataChannel> ctrl, vid, aud, in, mic;
    std::shared_ptr<rtc::PeerConnection> localPc;
    {
        std::lock_guard<std::mutex> lk(chMtx);
        ctrl = std::move(dcCtrl);
        vid = std::move(dcVid);
        aud = std::move(dcAud);
        in = std::move(dcIn);
        mic = std::move(dcMic);
        localPc = std::move(pc);
    }

    auto close = [](auto& ch) {
        try { if (ch && ch->isOpen()) ch->close(); } catch (...) {}
        ch.reset();
    };
    close(ctrl); close(vid); close(aud); close(in); close(mic);
    try { if (localPc) localPc->close(); } catch (...) {}

    conn = false; fpsRecv = false; gathered = false; hasDesc = false;
    chRdy = 0; overflow = 0; lastPing = 0;

    { std::lock_guard<std::mutex> lk(descMtx); localDesc.clear(); }
    { std::lock_guard<std::mutex> lk(sendMtx); while (!vidQ.empty()) vidQ.pop(); while (!audQ.empty()) audQ.pop(); }
}

void WebRTCServer::SetupPeerConnection() {
    std::shared_ptr<rtc::DataChannel> ctrl;
    std::shared_ptr<rtc::PeerConnection> existingPc;
    {
        std::lock_guard<std::mutex> lk(chMtx);
        ctrl = dcCtrl;
        existingPc = pc;
    }

    if (existingPc && ctrl && ctrl->isOpen()) {
        try {
            uint8_t k[4]; WritePod<uint32_t>(k, MSG_KICKED);
            ctrl->send((const std::byte*)k, 4);
            std::this_thread::sleep_for(50ms);
        } catch (...) {}
    }

    Reset();
    needsKey = true;
    uint64_t epoch = peerEpoch.fetch_add(1) + 1;
    LOG("WebRTC: Creating peer connection (epoch=%llu)", epoch);
    auto newPc = std::make_shared<rtc::PeerConnection>(cfg);
    {
        std::lock_guard<std::mutex> lk(chMtx);
        pc = newPc;
    }

    newPc->onLocalDescription([this, epoch](rtc::Description d) {
        LOG("WebRTC: Local description ready (epoch=%llu)", epoch);
        std::lock_guard<std::mutex> lk(descMtx);
        localDesc = std::string(d);
        hasDesc = true;
        descCv.notify_all();
    });

    newPc->onLocalCandidate([this, epoch](rtc::Candidate) {
        DBG("WebRTC: Local candidate gathered (epoch=%llu)", epoch);
        descCv.notify_all();
    });

    newPc->onStateChange([this, epoch](auto s) {
        LOG("WebRTC: Peer state=%s (epoch=%llu active=%llu ch=%d fpsRecv=%d conn=%d)",
            ToPeerStateString(s), epoch, peerEpoch.load(), chRdy.load(), fpsRecv.load() ? 1 : 0, conn.load() ? 1 : 0);
        bool now = s == rtc::PeerConnection::State::Connected;
        if (now && !conn) { needsKey = true; lastPing = GetTimestamp() / 1000; }
        if (!now && conn) { fpsRecv = false; chRdy = 0; if (cb.onDisconnect) cb.onDisconnect(); }
        conn = now;
    });

    newPc->onGatheringStateChange([this, epoch](auto s) {
        LOG("WebRTC: Gathering state=%s (epoch=%llu)", ToGatherStateString(s), epoch);
        if (s == rtc::PeerConnection::GatheringState::Complete) {
            gathered = true;
            descCv.notify_all();
        }
    });

    newPc->onDataChannel([this, epoch](auto ch) {
        std::string l = ch->label();
        LOG("WebRTC: Data channel announced '%s' (epoch=%llu active=%llu)", l.c_str(), epoch, peerEpoch.load());
        if (l == "control") {
            std::lock_guard<std::mutex> lk(chMtx);
            dcCtrl = ch;
            SetupChannel(dcCtrl, false, [this](auto& m) { HandleCtrl(m); }, epoch);
        } else if (l == "video") {
            std::lock_guard<std::mutex> lk(chMtx);
            dcVid = ch;
            SetupChannel(dcVid, true, nullptr, epoch);
        } else if (l == "audio") {
            std::lock_guard<std::mutex> lk(chMtx);
            dcAud = ch;
            SetupChannel(dcAud, true, nullptr, epoch);
        } else if (l == "input") {
            std::lock_guard<std::mutex> lk(chMtx);
            dcIn = ch;
            SetupChannel(dcIn, false, [this](auto& m) { HandleInput(m); }, epoch);
        } else if (l == "mic") {
            std::lock_guard<std::mutex> lk(chMtx);
            dcMic = ch;
            SetupChannel(dcMic, false, [this](auto& m) { HandleMic(m); }, epoch);
        }
    });
}

bool WebRTCServer::IsStale() {
    if (!conn) return false;
    int64_t now = GetTimestamp() / 1000;
    if (lastPing > 0 && now - lastPing > 3000) return true;
    return overflow.load() >= 10;
}

void WebRTCServer::LogStats() {
    int64_t now = GetTimestamp() / 1000;
    if (now - lastStatLog.load() < 60000) return;
    lastStatLog.store(now);
    if (conn || videoSent > 0)
        LOG("WebRTC Stats: v=%llu/%llu a=%llu/%llu ctrl=%llu/%llu in=%llu mic=%llu conn=%llu",
            videoSent.load(), videoErr.load(), audioSent.load(), audioErr.load(),
            ctrlSent.load(), ctrlRecv.load(), inputRecv.load(), micRecv.load(), connCount.load());
}

WebRTCServer::WebRTCServer() {
    cfg.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
    cfg.portRangeBegin = 50000;
    cfg.portRangeEnd = 50020;
    cfg.enableIceTcp = false;
    LOG("WebRTC: Server initialized");
    SetupPeerConnection();
}

WebRTCServer::~WebRTCServer() {
    LogStats();
    Reset();
}

void WebRTCServer::Init(WebRTCCallbacks c) { cb = std::move(c); }

void WebRTCServer::Shutdown() {
    Reset();
}

std::string WebRTCServer::GetLocal() {
    std::unique_lock<std::mutex> lk(descMtx);
    descCv.wait_for(lk, 200ms, [this] { return hasDesc.load(); });
    descCv.wait_for(lk, 150ms, [this] { return gathered.load(); });
    return localDesc;
}

void WebRTCServer::SetRemote(const std::string& sdp, const std::string& type) {
    LOG("WebRTC: SetRemote (type=%s)", type.c_str());
    if (type == "offer") SetupPeerConnection();

    std::shared_ptr<rtc::PeerConnection> localPc;
    {
        std::lock_guard<std::mutex> lk(chMtx);
        localPc = pc;
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

bool WebRTCServer::Send(const EncodedFrame& f) {
    if (!IsStreaming()) return false;
    if (IsStale()) { Reset(); if (cb.onDisconnect) cb.onDisconnect(); return false; }

    size_t sz = f.data.size();
    if (!sz || sz > DATA_CHUNK * 65535) return false;

    size_t nch = (sz + DATA_CHUNK - 1) / DATA_CHUNK;
    uint32_t fid = frmId++;
    if (nch > 65535) return false;

    constexpr uint8_t PKT_DATA = 0;
    constexpr uint8_t PKT_FEC = 1;
    constexpr uint8_t FEC_GROUP_SIZE = 4;
    size_t nfec = nch / FEC_GROUP_SIZE;

    PacketHeader h = {
        f.ts,
        static_cast<uint32_t>(f.encUs),
        fid,
        static_cast<uint32_t>(sz),
        0,
        static_cast<uint16_t>(nch),
        0,
        static_cast<uint16_t>(DATA_CHUNK),
        f.isKey ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
        PKT_DATA,
        FEC_GROUP_SIZE
    };

    {
        std::lock_guard<std::mutex> lk(sendMtx);
        while (vidQ.size() > (nch + nfec) * 3) {
            vidQ.pop();
            needsKey = true;
        }

        for (size_t g = 0; g * FEC_GROUP_SIZE < nch; g++) {
            size_t start = g * FEC_GROUP_SIZE;
            size_t end = std::min(start + FEC_GROUP_SIZE, nch);
            size_t parityLen = 0;

            std::vector<uint8_t> parity(DATA_CHUNK, 0);

            for (size_t i = start; i < end; i++) {
                h.chunkIndex = static_cast<uint16_t>(i);
                size_t off = i * DATA_CHUNK;
                size_t len = std::min(DATA_CHUNK, sz - off);

                h.chunkBytes = static_cast<uint16_t>(len);
                h.packetType = PKT_DATA;

                std::vector<uint8_t> pkt(HDR_SZ + len);
                memcpy(pkt.data(), &h, HDR_SZ);
                memcpy(pkt.data() + HDR_SZ, f.data.data() + off, len);
                vidQ.push(std::move(pkt));

                parityLen = std::max(parityLen, len);
                const uint8_t* src = f.data.data() + off;
                for (size_t j = 0; j < len; j++) parity[j] ^= src[j];
            }

            if (end - start == FEC_GROUP_SIZE && parityLen > 0) {
                h.chunkIndex = static_cast<uint16_t>(g);
                h.chunkBytes = static_cast<uint16_t>(parityLen);
                h.packetType = PKT_FEC;

                std::vector<uint8_t> pkt(HDR_SZ + parityLen);
                memcpy(pkt.data(), &h, HDR_SZ);
                memcpy(pkt.data() + HDR_SZ, parity.data(), parityLen);
                vidQ.push(std::move(pkt));
            }
        }
    }
    DrainVideo();
    videoSent++;
    LogStats();
    return true;
}

bool WebRTCServer::SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples) {
    if (!IsStreaming() || data.empty() || data.size() > 4000) return false;

    std::vector<uint8_t> pkt(sizeof(AudioPacketHeader) + data.size());
    auto* h = reinterpret_cast<AudioPacketHeader*>(pkt.data());
    h->magic = MSG_AUDIO_DATA;
    h->timestamp = ts;
    h->samples = static_cast<uint16_t>(samples);
    h->dataLength = static_cast<uint16_t>(data.size());
    memcpy(pkt.data() + sizeof(AudioPacketHeader), data.data(), data.size());

    std::shared_ptr<rtc::DataChannel> audio;
    {
        std::lock_guard<std::mutex> lk(chMtx);
        audio = dcAud;
    }

    if (audio && audio->isOpen() && audio->bufferedAmount() <= AUD_BUF / 2) {
        try {
            audio->send((const std::byte*)pkt.data(), pkt.size());
            audioSent++;
            return true;
        } catch (...) { audioErr++; }
    }

    std::lock_guard<std::mutex> lk(sendMtx);
    while (audQ.size() >= 3) audQ.pop();
    audQ.push(std::move(pkt));
    DrainAudio();
    return true;
}

void WebRTCServer::GetStats(uint64_t& vS, uint64_t& vE, uint64_t& aS, uint64_t& aE, uint64_t& c) {
    vS = videoSent.load(); vE = videoErr.load();
    aS = audioSent.load(); aE = audioErr.load();
    c = connCount.load();
}
