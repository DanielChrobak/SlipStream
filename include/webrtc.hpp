#pragma once
#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"

#pragma pack(push, 1)
struct PacketHeader { int64_t timestamp; uint32_t encodeTimeUs, frameId; uint16_t chunkIndex, totalChunks; uint8_t frameType; };
struct AudioPacketHeader { uint32_t magic; int64_t timestamp; uint16_t samples, dataLength; };
#pragma pack(pop)

struct WebRTCCallbacks {
    InputHandler* input = nullptr;
    std::function<void(int, uint8_t)> onFpsChange;
    std::function<int()> getHostFps;
    std::function<bool(int)> onMonitorChange;
    std::function<int()> getMonitor;
    std::function<void()> onDisconnect, onConnected;
    std::function<bool(CodecType)> onCodecChange;
    std::function<CodecType()> getCodec;
};

class WebRTCServer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dcCtrl, dcVid, dcAud, dcIn;
    std::atomic<bool> connected{false}, needsKey{true}, fpsRecv{false};
    std::atomic<bool> gathered{false}, authed{false}, hasDesc{false}, disconnecting{false};
    std::atomic<bool> dcCtrlOpen{false}, dcVidOpen{false}, dcAudOpen{false}, dcInOpen{false};
    std::atomic<int> chReady{0};
    std::string localDesc;
    std::mutex descMtx, vidSendMtx, audSendMtx, ctrlMtx;
    mutable std::mutex setupMtx;
    std::condition_variable descCv;
    rtc::Configuration cfg;

    static constexpr size_t VID_BUF = 262144, AUD_BUF = 131072, CHUNK = 1400, HDR_SZ = sizeof(PacketHeader);
    static constexpr size_t DATA_CHUNK = CHUNK - HDR_SZ, BUF_LOW = CHUNK * 16, MAX_Q = 3, MAX_AUD_Q = 4;

    std::atomic<uint32_t> frmId{0};
    std::atomic<int> curFps{60}, overflow{0};
    std::atomic<int64_t> lastPing{0};
    std::atomic<CodecType> curCodec{CODEC_H264};

    struct QPkt { std::vector<uint8_t> data; };
    std::queue<QPkt> sendQ;
    std::queue<std::vector<uint8_t>> audQ;
    WebRTCCallbacks cb;

    bool AllChannelsOpen() const { return dcCtrlOpen && dcVidOpen && dcAudOpen && dcInOpen; }

    bool SafeSendCtrl(const void* d, size_t len) {
        if (disconnecting) return false;
        std::lock_guard<std::mutex> lk(ctrlMtx);
        if (!dcCtrl || !dcCtrlOpen) return false;
        try { dcCtrl->send((const std::byte*)d, len); return true; } catch (...) { return false; }
    }

    void DrainVideo() {
        if (disconnecting || !dcVidOpen) return;
        std::vector<QPkt> toSend;
        {
            std::lock_guard<std::mutex> lk(vidSendMtx);
            while (!sendQ.empty()) {
                try { if (!dcVid || dcVid->bufferedAmount() > VID_BUF) break; } catch (...) { break; }
                toSend.push_back(std::move(sendQ.front())); sendQ.pop();
            }
        }
        for (auto& pkt : toSend) {
            if (!dcVidOpen) { overflow++; needsKey = true; break; }
            try { dcVid->send((const std::byte*)pkt.data.data(), pkt.data.size()); }
            catch (...) { overflow++; needsKey = true; }
        }
    }

    void DrainAudio() {
        if (disconnecting || !dcAudOpen) return;
        std::vector<std::vector<uint8_t>> toSend;
        {
            std::lock_guard<std::mutex> lk(audSendMtx);
            while (!audQ.empty()) {
                try { if (!dcAud || dcAud->bufferedAmount() > AUD_BUF) break; } catch (...) { break; }
                toSend.push_back(std::move(audQ.front())); audQ.pop();
            }
        }
        for (auto& pkt : toSend) {
            if (!dcAudOpen) break;
            try { dcAud->send((const std::byte*)pkt.data(), pkt.size()); } catch (...) {}
        }
    }

    void SendHostInfo() {
        if (!dcCtrlOpen) return;
        uint8_t buf[6]; *(uint32_t*)buf = MSG_HOST_INFO;
        *(uint16_t*)(buf + 4) = (uint16_t)(cb.getHostFps ? cb.getHostFps() : 60);
        SafeSendCtrl(buf, sizeof(buf));
    }

    void SendMonitorList() {
        if (!dcCtrlOpen) return;
        std::vector<uint8_t> buf;
        {
            std::lock_guard<std::mutex> lk(g_monitorsMutex);
            buf.resize(6 + g_monitors.size() * 74);
            size_t o = 0;
            *(uint32_t*)&buf[o] = MSG_MONITOR_LIST; o += 4;
            buf[o++] = (uint8_t)g_monitors.size();
            buf[o++] = (uint8_t)(cb.getMonitor ? cb.getMonitor() : 0);
            for (const auto& m : g_monitors) {
                buf[o++] = (uint8_t)m.index;
                *(uint16_t*)&buf[o] = (uint16_t)m.width;
                *(uint16_t*)&buf[o + 2] = (uint16_t)m.height;
                *(uint16_t*)&buf[o + 4] = (uint16_t)m.refreshRate;
                o += 6; buf[o++] = m.isPrimary ? 1 : 0;
                size_t nl = std::min(m.name.size(), (size_t)63);
                buf[o++] = (uint8_t)nl;
                memcpy(&buf[o], m.name.c_str(), nl); o += nl;
            }
            buf.resize(o);
        }
        SafeSendCtrl(buf.data(), buf.size());
    }

    void Disconnect(const char*) {
        if (disconnecting.exchange(true)) return;
        if (!connected.exchange(false)) { disconnecting = false; return; }
        fpsRecv = authed = false; overflow = 0; chReady = 0;
        dcCtrlOpen = dcVidOpen = dcAudOpen = dcInOpen = false;
        { std::lock_guard<std::mutex> lk(vidSendMtx); while (!sendQ.empty()) sendQ.pop(); }
        { std::lock_guard<std::mutex> lk(audSendMtx); while (!audQ.empty()) audQ.pop(); }
        {
            std::lock_guard<std::mutex> lk(setupMtx);
            try { if (dcCtrl && dcCtrl->isOpen()) dcCtrl->close(); } catch (...) {}
            try { if (dcVid && dcVid->isOpen()) dcVid->close(); } catch (...) {}
            try { if (dcAud && dcAud->isOpen()) dcAud->close(); } catch (...) {}
            try { if (dcIn && dcIn->isOpen()) dcIn->close(); } catch (...) {}
            dcCtrl.reset(); dcVid.reset(); dcAud.reset(); dcIn.reset();
            try { if (pc) pc->close(); } catch (...) {}
        }
        if (cb.onDisconnect) try { cb.onDisconnect(); } catch (...) {}
        disconnecting = false;
    }

    void HandleCtrl(const rtc::binary& m) {
        if (m.size() < 4 || !authed || disconnecting) return;
        uint32_t magic = *(const uint32_t*)m.data();
        switch (magic) {
            case MSG_PING:
                if (m.size() == 16) {
                    lastPing = GetTimestamp() / 1000; overflow = 0;
                    uint8_t r[24]; memcpy(r, m.data(), 16);
                    *(uint64_t*)(r + 16) = GetTimestamp();
                    SafeSendCtrl(r, sizeof(r));
                }
                break;
            case MSG_FPS_SET:
                if (m.size() == 7) {
                    uint16_t fps = *(const uint16_t*)((const uint8_t*)m.data() + 4);
                    uint8_t mode = (uint8_t)m[6];
                    if (fps >= 1 && fps <= 240 && mode <= 2) {
                        int act = (mode == 1 && cb.getHostFps) ? cb.getHostFps() : fps;
                        curFps = act; fpsRecv = true;
                        if (cb.onFpsChange) cb.onFpsChange(act, mode);
                        uint8_t a[7]; *(uint32_t*)a = MSG_FPS_ACK;
                        *(uint16_t*)(a + 4) = (uint16_t)act; a[6] = mode;
                        SafeSendCtrl(a, sizeof(a));
                    }
                }
                break;
            case MSG_CODEC_SET:
                if (m.size() == 5) {
                    uint8_t c = (uint8_t)m[4];
                    if (c <= 1) {
                        CodecType nc = (CodecType)c;
                        bool ok = !cb.onCodecChange || cb.onCodecChange(nc);
                        if (ok) { curCodec = nc; needsKey = true; }
                        uint8_t a[5]; *(uint32_t*)a = MSG_CODEC_ACK; a[4] = (uint8_t)curCodec.load();
                        SafeSendCtrl(a, sizeof(a));
                    }
                }
                break;
            case MSG_REQUEST_KEY: needsKey = true; break;
            case MSG_MONITOR_SET:
                if (m.size() == 5 && cb.onMonitorChange && cb.onMonitorChange((int)(uint8_t)m[4])) {
                    needsKey = true; SendMonitorList(); SendHostInfo();
                }
                break;
        }
    }

    void HandleInput(const rtc::binary& m) {
        if (m.size() < 4 || !authed || disconnecting || !cb.input) return;
        cb.input->HandleMessage((const uint8_t*)m.data(), m.size());
    }

    void OnAllOpen() {
        if (disconnecting) return;
        connected = needsKey = authed = true;
        lastPing = GetTimestamp() / 1000; overflow = 0;
        SendHostInfo(); SendMonitorList();
        if (cb.onConnected) cb.onConnected();
    }

    void OnClose() {
        connected = fpsRecv = authed = false; overflow = 0; chReady = 0;
        dcCtrlOpen = dcVidOpen = dcAudOpen = dcInOpen = false;
        { std::lock_guard<std::mutex> lk(vidSendMtx); while (!sendQ.empty()) sendQ.pop(); }
        { std::lock_guard<std::mutex> lk(audSendMtx); while (!audQ.empty()) audQ.pop(); }
    }

    void SetupCtrl() {
        if (!dcCtrl) return;
        dcCtrl->setBufferedAmountLowThreshold(BUF_LOW);
        dcCtrl->onOpen([this] { dcCtrlOpen = true; if (!disconnecting && ++chReady == 4) OnAllOpen(); });
        dcCtrl->onClosed([this] { dcCtrlOpen = false; OnClose(); });
        dcCtrl->onMessage([this](auto d) { if (auto* b = std::get_if<rtc::binary>(&d)) HandleCtrl(*b); });
    }

    void SetupVid() {
        if (!dcVid) return;
        dcVid->setBufferedAmountLowThreshold(BUF_LOW);
        dcVid->onBufferedAmountLow([this] { if (!disconnecting) DrainVideo(); });
        dcVid->onOpen([this] { dcVidOpen = true; if (!disconnecting && ++chReady == 4) OnAllOpen(); });
        dcVid->onClosed([this] { dcVidOpen = false; OnClose(); });
    }

    void SetupAud() {
        if (!dcAud) return;
        dcAud->setBufferedAmountLowThreshold(BUF_LOW);
        dcAud->onBufferedAmountLow([this] { if (!disconnecting) DrainAudio(); });
        dcAud->onOpen([this] { dcAudOpen = true; if (!disconnecting && ++chReady == 4) OnAllOpen(); });
        dcAud->onClosed([this] { dcAudOpen = false; OnClose(); });
    }

    void SetupIn() {
        if (!dcIn) return;
        dcIn->onOpen([this] { dcInOpen = true; if (!disconnecting && ++chReady == 4) OnAllOpen(); });
        dcIn->onClosed([this] { dcInOpen = false; OnClose(); });
        dcIn->onMessage([this](auto d) { if (auto* b = std::get_if<rtc::binary>(&d)) HandleInput(*b); });
    }

    void SetupPC() {
        std::lock_guard<std::mutex> lk(setupMtx);
        if (pc) {
            dcCtrlOpen = dcVidOpen = dcAudOpen = dcInOpen = false;
            try { if (dcCtrl && dcCtrl->isOpen()) dcCtrl->close(); } catch (...) {}
            try { if (dcVid && dcVid->isOpen()) dcVid->close(); } catch (...) {}
            try { if (dcAud && dcAud->isOpen()) dcAud->close(); } catch (...) {}
            try { if (dcIn && dcIn->isOpen()) dcIn->close(); } catch (...) {}
            dcCtrl.reset(); dcVid.reset(); dcAud.reset(); dcIn.reset();
            try { pc->close(); } catch (...) {}
        }
        connected = needsKey = true; fpsRecv = gathered = authed = hasDesc = false;
        overflow = 0; lastPing = 0; chReady = 0; disconnecting = false;
        { std::lock_guard<std::mutex> lk2(descMtx); localDesc.clear(); }
        { std::lock_guard<std::mutex> lk2(vidSendMtx); while (!sendQ.empty()) sendQ.pop(); }
        { std::lock_guard<std::mutex> lk2(audSendMtx); while (!audQ.empty()) audQ.pop(); }

        pc = std::make_shared<rtc::PeerConnection>(cfg);
        pc->onLocalDescription([this](rtc::Description d) {
            std::lock_guard<std::mutex> lk(descMtx);
            localDesc = std::string(d); hasDesc = true; descCv.notify_all();
        });
        pc->onLocalCandidate([this](rtc::Candidate) { descCv.notify_all(); });
        pc->onStateChange([this](auto s) {
            if (disconnecting) return;
            bool was = connected.load();
            connected = (s == rtc::PeerConnection::State::Connected);
            if (connected && !was) { needsKey = true; lastPing = GetTimestamp() / 1000; }
            if (!connected && was) { fpsRecv = authed = false; overflow = 0; chReady = 0; if (cb.onDisconnect) cb.onDisconnect(); }
        });
        pc->onGatheringStateChange([this](auto s) {
            if (s == rtc::PeerConnection::GatheringState::Complete) { gathered = true; descCv.notify_all(); }
        });
        pc->onDataChannel([this](auto ch) {
            if (disconnecting) return;
            std::string l = ch->label();
            std::lock_guard<std::mutex> lk(setupMtx);
            if (l == "control") { dcCtrl = ch; SetupCtrl(); }
            else if (l == "video") { dcVid = ch; SetupVid(); }
            else if (l == "audio") { dcAud = ch; SetupAud(); }
            else if (l == "input") { dcIn = ch; SetupIn(); }
        });
    }

    bool IsStale() {
        if (!connected || disconnecting) return false;
        int64_t lp = lastPing, now = GetTimestamp() / 1000;
        return (lp > 0 && (now - lp) > 3000) || overflow >= 10;
    }

public:
    WebRTCServer() {
        cfg.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
        cfg.portRangeBegin = 50000; cfg.portRangeEnd = 50020; cfg.enableIceTcp = false;
        SetupPC();
    }

    void Init(WebRTCCallbacks c) { cb = std::move(c); }

    std::string GetLocal() {
        std::unique_lock<std::mutex> lk(descMtx);
        if (!descCv.wait_for(lk, 200ms, [this] { return hasDesc.load(); })) return localDesc;
        descCv.wait_for(lk, 150ms, [this] { return gathered.load(); });
        return localDesc;
    }

    void SetRemote(const std::string& sdp, const std::string& type) {
        if (type == "offer") SetupPC();
        pc->setRemoteDescription(rtc::Description(sdp, type));
        if (type == "offer") pc->setLocalDescription();
    }

    bool IsStreaming() const { return connected && authed && fpsRecv && !disconnecting && AllChannelsOpen(); }
    int GetCurrentFps() const { return curFps; }
    bool NeedsKey() { return needsKey.exchange(false); }

    void Send(const EncodedFrame& f) {
        if (!IsStreaming()) return;
        if (IsStale()) { Disconnect("stale"); return; }
        size_t sz = f.data.size(), nch = (sz + DATA_CHUNK - 1) / DATA_CHUNK;
        if (nch > 65535 || !sz) return;
        uint32_t fid = frmId++;
        PacketHeader h = {f.ts, (uint32_t)f.encUs, fid, 0, (uint16_t)nch, f.isKey ? (uint8_t)1 : (uint8_t)0};
        {
            std::lock_guard<std::mutex> lk(vidSendMtx);
            if (sendQ.size() > nch * MAX_Q) {
                size_t drop = sendQ.size() - nch;
                while (drop-- > 0 && !sendQ.empty()) sendQ.pop();
                needsKey = true;
            }
            for (size_t i = 0; i < nch; i++) {
                h.chunkIndex = (uint16_t)i;
                size_t off = i * DATA_CHUNK, len = std::min(DATA_CHUNK, sz - off);
                QPkt p; p.data.resize(HDR_SZ + len);
                memcpy(p.data.data(), &h, HDR_SZ);
                memcpy(p.data.data() + HDR_SZ, f.data.data() + off, len);
                sendQ.push(std::move(p));
            }
        }
        DrainVideo();
    }

    void SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples) {
        if (!connected || !authed || !fpsRecv || disconnecting || !dcAudOpen || data.empty() || data.size() > 4000) return;
        size_t total = sizeof(AudioPacketHeader) + data.size();
        std::vector<uint8_t> pkt(total);
        auto* h = (AudioPacketHeader*)pkt.data();
        h->magic = MSG_AUDIO_DATA; h->timestamp = ts; h->samples = (uint16_t)samples; h->dataLength = (uint16_t)data.size();
        memcpy(pkt.data() + sizeof(AudioPacketHeader), data.data(), data.size());
        bool sentDirect = false;
        try { if (dcAud && dcAud->bufferedAmount() <= AUD_BUF / 2) { dcAud->send((const std::byte*)pkt.data(), total); sentDirect = true; } } catch (...) {}
        if (sentDirect) return;
        { std::lock_guard<std::mutex> lk(audSendMtx); while (audQ.size() >= MAX_AUD_Q) audQ.pop(); audQ.push(std::move(pkt)); }
        DrainAudio();
    }
};
