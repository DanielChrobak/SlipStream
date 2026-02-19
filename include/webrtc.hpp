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
    InputHandler* input=nullptr;
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

    static constexpr size_t VID_BUF=262144, AUD_BUF=131072, CHUNK=1400, HDR_SZ=sizeof(PacketHeader);
    static constexpr size_t DATA_CHUNK=CHUNK-HDR_SZ, BUF_LOW=CHUNK*16;
    static constexpr int NUM_CH=5;

    std::atomic<uint64_t> totalVideoFramesSent{0}, totalAudioPacketsSent{0};
    std::atomic<uint64_t> videoSendErrors{0}, audioSendErrors{0};
    std::atomic<uint64_t> ctrlMsgsSent{0}, ctrlMsgsReceived{0};
    std::atomic<uint64_t> inputMsgsReceived{0}, micPacketsReceived{0};
    std::atomic<uint64_t> droppedVideoFrames{0}, droppedAudioPackets{0};
    std::atomic<uint64_t> connectionCount{0};
    std::atomic<int64_t> lastStatLog{0};

    bool ChOpen(const std::shared_ptr<rtc::DataChannel>& ch) const { return ch && ch->isOpen(); }
    bool AllOpen() const { return ChOpen(dcCtrl) && ChOpen(dcVid) && ChOpen(dcAud) && ChOpen(dcIn) && ChOpen(dcMic); }

    bool SendCtrl(const void* d, size_t len) {
        if(!ChOpen(dcCtrl)) { DBG("WebRTC: SendCtrl failed - control channel not open"); return false; }
        try {
            dcCtrl->send((const std::byte*)d, len);
            ctrlMsgsSent++;
            return true;
        } catch(const std::exception& e) {
            WARN("WebRTC: SendCtrl exception: %s", e.what());
            return false;
        } catch(...) {
            WARN("WebRTC: SendCtrl unknown exception");
            return false;
        }
    }

    void SendHostInfo() {
        uint8_t buf[6];
        *(uint32_t*)buf = MSG_HOST_INFO;
        int fps = cb.getHostFps ? cb.getHostFps() : 60;
        *(uint16_t*)(buf+4) = (uint16_t)fps;
        if(!SendCtrl(buf, sizeof(buf))) {
            WARN("WebRTC: Failed to send host info (fps=%d)", fps);
        } else {
            DBG("WebRTC: Sent host info (fps=%d)", fps);
        }
    }

    void SendMonitorList() {
        std::vector<uint8_t> buf;
        std::lock_guard<std::mutex> lk(g_monitorsMutex);

        if(g_monitors.empty()) { WARN("WebRTC: SendMonitorList called with no monitors"); }

        buf.resize(6 + g_monitors.size() * 74);
        size_t o = 0;
        *(uint32_t*)&buf[o] = MSG_MONITOR_LIST; o += 4;
        buf[o++] = (uint8_t)g_monitors.size();
        buf[o++] = (uint8_t)(cb.getMonitor ? cb.getMonitor() : 0);

        for(const auto& m : g_monitors) {
            buf[o++] = (uint8_t)m.index;
            *(uint16_t*)&buf[o] = (uint16_t)m.width;
            *(uint16_t*)&buf[o+2] = (uint16_t)m.height;
            *(uint16_t*)&buf[o+4] = (uint16_t)m.refreshRate;
            o += 6;
            buf[o++] = m.isPrimary ? 1 : 0;
            size_t nl = std::min(m.name.size(), (size_t)63);
            buf[o++] = (uint8_t)nl;
            memcpy(&buf[o], m.name.c_str(), nl);
            o += nl;
        }

        buf.resize(o);
        if(!SendCtrl(buf.data(), buf.size())) {
            WARN("WebRTC: Failed to send monitor list (%zu monitors)", g_monitors.size());
        } else {
            DBG("WebRTC: Sent monitor list (%zu monitors, current=%d)",
                g_monitors.size(), cb.getMonitor ? cb.getMonitor() : 0);
        }
    }

    void SendCodecCaps() {
        uint8_t caps = cb.getCodecCaps ? cb.getCodecCaps() : 0x07;
        uint8_t buf[5];
        *(uint32_t*)buf = MSG_CODEC_CAPS;
        buf[4] = caps;
        if(!SendCtrl(buf, sizeof(buf))) {
            WARN("WebRTC: Failed to send codec caps (0x%02X)", caps);
        } else {
            DBG("WebRTC: Sent codec caps (AV1:%d H265:%d H264:%d)",
                (caps&1)?1:0, (caps&2)?1:0, (caps&4)?1:0);
        }
    }

    void SendVersion() {
        std::string ver = SLIPSTREAM_VERSION;
        std::vector<uint8_t> buf(5 + ver.size());
        *(uint32_t*)buf.data() = MSG_VERSION;
        buf[4] = (uint8_t)ver.size();
        memcpy(buf.data() + 5, ver.c_str(), ver.size());
        if(!SendCtrl(buf.data(), buf.size())) {
            WARN("WebRTC: Failed to send version");
        } else {
            DBG("WebRTC: Sent version %s", ver.c_str());
        }
    }

    void HandleCtrl(const rtc::binary& m) {
        if(m.size() < 4) {
            WARN("WebRTC: HandleCtrl received message too small (%zu bytes)", m.size());
            return;
        }
        if(chRdy < NUM_CH) {
            DBG("WebRTC: HandleCtrl called before all channels ready (%d/%d)", chRdy.load(), NUM_CH);
            return;
        }

        ctrlMsgsReceived++;
        uint32_t magic = *(const uint32_t*)m.data();

        switch(magic) {
            case MSG_PING:
                if(m.size() == 16) {
                    lastPing = GetTimestamp() / 1000;
                    overflow = 0;
                    uint8_t r[24];
                    memcpy(r, m.data(), 16);
                    *(uint64_t*)(r+16) = GetTimestamp();
                    SendCtrl(r, sizeof(r));
                } else {
                    WARN("WebRTC: MSG_PING invalid size (%zu, expected 16)", m.size());
                }
                break;

            case MSG_FPS_SET:
                if(m.size() == 7) {
                    uint16_t fps = *(const uint16_t*)((const uint8_t*)m.data()+4);
                    uint8_t mode = (uint8_t)m[6];
                    if(fps >= 1 && fps <= 240 && mode <= 2) {
                        int act = (mode==1 && cb.getHostFps) ? cb.getHostFps() : fps;
                        fpsRecv = true;
                        LOG("WebRTC: FPS set to %d (requested=%d, mode=%d)", act, fps, mode);
                        if(cb.onFpsChange) cb.onFpsChange(act, mode);
                        uint8_t a[7];
                        *(uint32_t*)a = MSG_FPS_ACK;
                        *(uint16_t*)(a+4) = (uint16_t)act;
                        a[6] = mode;
                        SendCtrl(a, sizeof(a));
                    } else {
                        WARN("WebRTC: MSG_FPS_SET invalid params (fps=%d, mode=%d)", fps, mode);
                    }
                } else {
                    WARN("WebRTC: MSG_FPS_SET invalid size (%zu, expected 7)", m.size());
                }
                break;

            case MSG_CODEC_SET:
                if(m.size() == 5) {
                    uint8_t codecVal = (uint8_t)m[4];
                    if(codecVal <= 2) {
                        CodecType nc = (CodecType)codecVal;
                        const char* codecNames[] = {"AV1", "H265", "H264"};
                        bool success = !cb.onCodecChange || cb.onCodecChange(nc);
                        if(success) {
                            curCodec = nc;
                            needsKey = true;
                            LOG("WebRTC: Codec changed to %s", codecNames[codecVal]);
                        } else {
                            WARN("WebRTC: Codec change to %s rejected", codecNames[codecVal]);
                        }
                        uint8_t a[5];
                        *(uint32_t*)a = MSG_CODEC_ACK;
                        a[4] = (uint8_t)curCodec.load();
                        SendCtrl(a, sizeof(a));
                    } else {
                        WARN("WebRTC: MSG_CODEC_SET invalid codec value %d", codecVal);
                    }
                } else {
                    WARN("WebRTC: MSG_CODEC_SET invalid size (%zu, expected 5)", m.size());
                }
                break;

            case MSG_REQUEST_KEY:
                needsKey = true;
                DBG("WebRTC: Keyframe requested by client");
                break;

            case MSG_MONITOR_SET:
                if(m.size() == 5) {
                    int monIdx = (int)(uint8_t)m[4];
                    if(cb.onMonitorChange) {
                        bool success = cb.onMonitorChange(monIdx);
                        if(success) {
                            needsKey = true;
                            SendMonitorList();
                            SendHostInfo();
                            LOG("WebRTC: Monitor switched to %d", monIdx);
                        } else {
                            WARN("WebRTC: Monitor switch to %d failed", monIdx);
                        }
                    } else {
                        WARN("WebRTC: MSG_MONITOR_SET but no handler configured");
                    }
                } else {
                    WARN("WebRTC: MSG_MONITOR_SET invalid size (%zu, expected 5)", m.size());
                }
                break;

            case MSG_CLIPBOARD_DATA:
                if(m.size() >= 8 && cb.setClipboard) {
                    uint32_t len = *(const uint32_t*)((const uint8_t*)m.data()+4);
                    if(len > 0 && m.size() >= 8+len && len <= 1048576) {
                        bool success = cb.setClipboard(std::string((const char*)m.data()+8, len));
                        DBG("WebRTC: Clipboard data received (%u bytes, set=%s)",
                            len, success?"yes":"no");
                    } else {
                        WARN("WebRTC: MSG_CLIPBOARD_DATA invalid (len=%u, msgSize=%zu)", len, m.size());
                    }
                } else if(m.size() < 8) {
                    WARN("WebRTC: MSG_CLIPBOARD_DATA too small (%zu bytes)", m.size());
                }
                break;

            case MSG_CLIPBOARD_GET:
                if(cb.getClipboard) {
                    std::string text = cb.getClipboard();
                    if(!text.empty() && text.size() <= 1048576) {
                        std::vector<uint8_t> buf(8 + text.size());
                        *(uint32_t*)buf.data() = MSG_CLIPBOARD_DATA;
                        *(uint32_t*)(buf.data()+4) = (uint32_t)text.size();
                        memcpy(buf.data()+8, text.data(), text.size());
                        if(SendCtrl(buf.data(), buf.size())) {
                            DBG("WebRTC: Sent clipboard data (%zu bytes)", text.size());
                        } else {
                            WARN("WebRTC: Failed to send clipboard data");
                        }
                    } else if(text.size() > 1048576) {
                        WARN("WebRTC: Clipboard too large (%zu bytes, max 1MB)", text.size());
                    }
                } else {
                    DBG("WebRTC: MSG_CLIPBOARD_GET but no handler configured");
                }
                break;

            case MSG_CURSOR_CAPTURE:
                if(m.size() == 5) {
                    bool enable = (uint8_t)m[4] != 0;
                    if(cb.onCursorCapture) {
                        cb.onCursorCapture(enable);
                        DBG("WebRTC: Cursor capture %s", enable?"enabled":"disabled");
                    }
                } else {
                    WARN("WebRTC: MSG_CURSOR_CAPTURE invalid size (%zu)", m.size());
                }
                break;

            case MSG_AUDIO_ENABLE:
                if(m.size() == 5) {
                    bool enable = (uint8_t)m[4] != 0;
                    if(cb.onAudioEnable) {
                        cb.onAudioEnable(enable);
                        LOG("WebRTC: Audio streaming %s", enable?"enabled":"disabled");
                    }
                } else {
                    WARN("WebRTC: MSG_AUDIO_ENABLE invalid size (%zu)", m.size());
                }
                break;

            case MSG_MIC_ENABLE:
                if(m.size() == 5) {
                    bool enable = (uint8_t)m[4] != 0;
                    if(cb.onMicEnable) {
                        cb.onMicEnable(enable);
                        LOG("WebRTC: Mic streaming %s", enable?"enabled":"disabled");
                    }
                } else {
                    WARN("WebRTC: MSG_MIC_ENABLE invalid size (%zu)", m.size());
                }
                break;

            default:
                DBG("WebRTC: Unknown control message type 0x%08X (%zu bytes)", magic, m.size());
                break;
        }
    }

    void HandleInput(const rtc::binary& m) {
        if(m.size() < 4) { WARN("WebRTC: HandleInput message too small (%zu bytes)", m.size()); return; }
        if(chRdy < NUM_CH) { DBG("WebRTC: HandleInput called before all channels ready"); return; }
        if(!cb.input) { DBG("WebRTC: HandleInput but no input handler configured"); return; }
        inputMsgsReceived++;
        cb.input->HandleMessage((const uint8_t*)m.data(), m.size());
    }

    void HandleMic(const rtc::binary& m) {
        if(m.size() < sizeof(MicPacketHeader)) {
            WARN("WebRTC: HandleMic packet too small (%zu bytes, need %zu)",
                 m.size(), sizeof(MicPacketHeader));
            return;
        }
        if(chRdy < NUM_CH) return;

        if(*(const uint32_t*)m.data() == MSG_MIC_DATA) {
            micPacketsReceived++;
            if(cb.onMicData) { cb.onMicData((const uint8_t*)m.data(), m.size()); }
        } else {
            WARN("WebRTC: HandleMic wrong magic 0x%08X", *(const uint32_t*)m.data());
        }
    }

    void OnChOpen() {
        int ready = ++chRdy;
        DBG("WebRTC: Channel opened (%d/%d ready)", ready, NUM_CH);
        if(ready == NUM_CH) {
            conn = true;
            needsKey = true;
            lastPing = GetTimestamp() / 1000;
            overflow = 0;
            connectionCount++;
            LOG("WebRTC: All channels ready, connection #%llu established", connectionCount.load());
            SendHostInfo();
            SendCodecCaps();
            SendMonitorList();
            SendVersion();
            if(cb.onConnected) cb.onConnected();
        }
    }

    void OnChClose() {
        int wasReady = chRdy.exchange(0);
        bool wasConn = conn.exchange(false);
        fpsRecv = false;
        overflow = 0;
        if(wasConn) {
            LOG("WebRTC: Connection closed (was ready: %d/%d)", wasReady, NUM_CH);
        } else {
            DBG("WebRTC: Channel closed (was ready: %d/%d)", wasReady, NUM_CH);
        }
        if(cb.onDisconnect) cb.onDisconnect();
    }

    void SetupCh(std::shared_ptr<rtc::DataChannel>& ch, bool drain, bool msg,
                 std::function<void(const rtc::binary&)> h=nullptr) {
        if(!ch) { WARN("WebRTC: SetupCh called with null channel"); return; }

        std::string label = ch->label();
        DBG("WebRTC: Setting up channel '%s' (drain=%d, msg=%d)", label.c_str(), drain, msg);

        ch->setBufferedAmountLowThreshold(BUF_LOW);

        ch->onOpen([this, label] {
            DBG("WebRTC: Channel '%s' opened", label.c_str());
            OnChOpen();
        });

        ch->onClosed([this, label] {
            DBG("WebRTC: Channel '%s' closed", label.c_str());
            OnChClose();
        });

        ch->onError([this, label](std::string error) {
            ERR("WebRTC: Channel '%s' error: %s", label.c_str(), error.c_str());
        });

        if(msg && h) {
            ch->onMessage([this, h, label](auto d) {
                if(auto* b = std::get_if<rtc::binary>(&d)) { h(*b); }
                else { DBG("WebRTC: Channel '%s' received non-binary message", label.c_str()); }
            });
        }

        if(drain) {
            ch->onBufferedAmountLow([this, &ch, label] {
                if(&ch == &dcVid) DrainVid();
                else if(&ch == &dcAud) DrainAud();
            });
        }
    }

    void DrainVid() {
        if(!ChOpen(dcVid)) return;
        std::lock_guard<std::mutex> lk(sendMtx);
        size_t sent = 0;

        while(!vidQ.empty() && dcVid->bufferedAmount() <= VID_BUF) {
            try {
                dcVid->send((const std::byte*)vidQ.front().data(), vidQ.front().size());
                sent++;
            } catch(const std::exception& e) {
                videoSendErrors++;
                overflow++;
                needsKey = true;
                WARN("WebRTC: DrainVid send error: %s (overflow=%d)", e.what(), overflow.load());
            } catch(...) {
                videoSendErrors++;
                overflow++;
                needsKey = true;
                WARN("WebRTC: DrainVid unknown send error (overflow=%d)", overflow.load());
            }
            vidQ.pop();
        }
        if(sent > 0) { DBG("WebRTC: DrainVid sent %zu packets, %zu remaining", sent, vidQ.size()); }
    }

    void DrainAud() {
        if(!ChOpen(dcAud)) return;
        std::lock_guard<std::mutex> lk(sendMtx);
        size_t sent = 0;

        while(!audQ.empty() && dcAud->bufferedAmount() <= AUD_BUF) {
            try {
                dcAud->send((const std::byte*)audQ.front().data(), audQ.front().size());
                sent++;
            } catch(const std::exception& e) {
                audioSendErrors++;
                WARN("WebRTC: DrainAud send error: %s", e.what());
            } catch(...) {
                audioSendErrors++;
                WARN("WebRTC: DrainAud unknown send error");
            }
            audQ.pop();
        }
    }

    std::queue<std::vector<uint8_t>> vidQ, audQ;

    void Reset() {
        DBG("WebRTC: Resetting connection state");

        auto close = [](auto& ch, const char* name) {
            try {
                if(ch && ch->isOpen()) {
                    DBG("WebRTC: Closing channel '%s'", name);
                    ch->close();
                }
            } catch(const std::exception& e) {
                WARN("WebRTC: Exception closing channel '%s': %s", name, e.what());
            } catch(...) {
                WARN("WebRTC: Unknown exception closing channel '%s'", name);
            }
            ch.reset();
        };

        close(dcCtrl, "control");
        close(dcVid, "video");
        close(dcAud, "audio");
        close(dcIn, "input");
        close(dcMic, "mic");

        try {
            if(pc) { DBG("WebRTC: Closing peer connection"); pc->close(); }
        } catch(const std::exception& e) {
            WARN("WebRTC: Exception closing peer connection: %s", e.what());
        } catch(...) {
            WARN("WebRTC: Unknown exception closing peer connection");
        }

        conn = false;
        fpsRecv = false;
        gathered = false;
        hasDesc = false;
        chRdy = 0;
        overflow = 0;
        lastPing = 0;

        { std::lock_guard<std::mutex> lk(descMtx); localDesc.clear(); }

        {
            std::lock_guard<std::mutex> lk(sendMtx);
            size_t vidDropped = vidQ.size();
            size_t audDropped = audQ.size();
            while(!vidQ.empty()) vidQ.pop();
            while(!audQ.empty()) audQ.pop();
            if(vidDropped > 0 || audDropped > 0) {
                DBG("WebRTC: Reset dropped %zu video, %zu audio packets", vidDropped, audDropped);
            }
        }
    }

    void SetupPC() {
        if(pc && ChOpen(dcCtrl)) {
            try {
                uint8_t k[4];
                *(uint32_t*)k = MSG_KICKED;
                dcCtrl->send((const std::byte*)k, 4);
                LOG("WebRTC: Kicked existing client for new connection");
                std::this_thread::sleep_for(50ms);
            } catch(const std::exception& e) {
                DBG("WebRTC: Failed to send kick message: %s", e.what());
            } catch(...) {
                DBG("WebRTC: Failed to send kick message (unknown error)");
            }
        }

        Reset();
        needsKey = true;
        LOG("WebRTC: Creating new peer connection");
        pc = std::make_shared<rtc::PeerConnection>(cfg);

        pc->onLocalDescription([this](rtc::Description d) {
            std::lock_guard<std::mutex> lk(descMtx);
            localDesc = std::string(d);
            hasDesc = true;
            descCv.notify_all();
            DBG("WebRTC: Local description generated (%zu bytes)", localDesc.size());
        });

        pc->onLocalCandidate([this](rtc::Candidate c) {
            DBG("WebRTC: Local ICE candidate: %s", std::string(c).substr(0, 60).c_str());
            descCv.notify_all();
        });

        pc->onStateChange([this](auto s) {
            const char* stateNames[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
            int stateIdx = static_cast<int>(s);
            LOG("WebRTC: Connection state -> %s", stateIdx < 6 ? stateNames[stateIdx] : "Unknown");
            bool now = s == rtc::PeerConnection::State::Connected;
            if(now && !conn) {
                needsKey = true;
                lastPing = GetTimestamp() / 1000;
            }
            if(!now && conn) {
                fpsRecv = false;
                chRdy = 0;
                if(cb.onDisconnect) cb.onDisconnect();
            }
            conn = now;
        });

        pc->onGatheringStateChange([this](auto s) {
            const char* gatherNames[] = {"New", "InProgress", "Complete"};
            int gatherIdx = static_cast<int>(s);
            DBG("WebRTC: ICE gathering state -> %s", gatherIdx < 3 ? gatherNames[gatherIdx] : "Unknown");
            if(s == rtc::PeerConnection::GatheringState::Complete) {
                gathered = true;
                descCv.notify_all();
            }
        });

        pc->onDataChannel([this](auto ch) {
            std::string l = ch->label();
            DBG("WebRTC: Remote data channel created: '%s'", l.c_str());

            if(l == "control") {
                dcCtrl = ch;
                SetupCh(dcCtrl, false, true, [this](auto& m) { HandleCtrl(m); });
            } else if(l == "video") {
                dcVid = ch;
                SetupCh(dcVid, true, false);
            } else if(l == "audio") {
                dcAud = ch;
                SetupCh(dcAud, true, false);
            } else if(l == "input") {
                dcIn = ch;
                SetupCh(dcIn, false, true, [this](auto& m) { HandleInput(m); });
            } else if(l == "mic") {
                dcMic = ch;
                SetupCh(dcMic, false, true, [this](auto& m) { HandleMic(m); });
            } else {
                WARN("WebRTC: Unknown data channel '%s' ignored", l.c_str());
            }
        });
    }

    bool IsStale() {
        if(!conn) return false;
        int64_t now = GetTimestamp() / 1000;
        int64_t pingAge = lastPing > 0 ? (now - lastPing) : 0;
        int curOverflow = overflow.load();

        if(pingAge > 3000) {
            WARN("WebRTC: Connection stale - no ping for %lldms", pingAge);
            return true;
        }
        if(curOverflow >= 10) {
            WARN("WebRTC: Connection stale - overflow count %d", curOverflow);
            return true;
        }
        return false;
    }

    void LogStats() {
        int64_t now = GetTimestamp() / 1000;
        if(now - lastStatLog.load() < 60000) return;
        lastStatLog.store(now);

        if(conn || totalVideoFramesSent > 0) {
            LOG("WebRTC Stats: video=%llu/%llu audio=%llu/%llu ctrl=%llu/%llu input=%llu mic=%llu connections=%llu",
                totalVideoFramesSent.load(), videoSendErrors.load(),
                totalAudioPacketsSent.load(), audioSendErrors.load(),
                ctrlMsgsSent.load(), ctrlMsgsReceived.load(),
                inputMsgsReceived.load(), micPacketsReceived.load(),
                connectionCount.load());
        }
    }

public:
    WebRTCServer() {
        cfg.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
        cfg.portRangeBegin = 50000;
        cfg.portRangeEnd = 50020;
        cfg.enableIceTcp = false;
        LOG("WebRTC: Server initialized (ports 50000-50020)");
        SetupPC();
    }

    ~WebRTCServer() {
        LOG("WebRTC: Server shutting down");
        LogStats();
        Reset();
    }

    void Init(WebRTCCallbacks c) { cb = std::move(c); DBG("WebRTC: Callbacks initialized"); }

    std::string GetLocal() {
        std::unique_lock<std::mutex> lk(descMtx);
        if(!descCv.wait_for(lk, 200ms, [this] { return hasDesc.load(); })) {
            WARN("WebRTC: GetLocal timed out waiting for local description");
            return localDesc;
        }
        descCv.wait_for(lk, 150ms, [this] { return gathered.load(); });
        DBG("WebRTC: GetLocal returning %zu byte description (gathered=%d)",
            localDesc.size(), gathered.load());
        return localDesc;
    }

    void SetRemote(const std::string& sdp, const std::string& type) {
        LOG("WebRTC: SetRemote called (type=%s, sdp=%zu bytes)", type.c_str(), sdp.size());
        if(type == "offer") SetupPC();

        try {
            pc->setRemoteDescription(rtc::Description(sdp, type));
            DBG("WebRTC: Remote description set successfully");
        } catch(const std::exception& e) {
            ERR("WebRTC: setRemoteDescription failed: %s", e.what());
            throw;
        }

        if(type == "offer") {
            try {
                pc->setLocalDescription();
                DBG("WebRTC: Local description set");
            } catch(const std::exception& e) {
                ERR("WebRTC: setLocalDescription failed: %s", e.what());
                throw;
            }
        }
    }

    bool IsStreaming() const { return conn && fpsRecv && chRdy == NUM_CH; }
    bool NeedsKey() { return needsKey.exchange(false); }

    bool SendCursorShape(CursorType ct) {
        if(!IsStreaming()) return false;
        uint8_t buf[5];
        *(uint32_t*)buf = MSG_CURSOR_SHAPE;
        buf[4] = (uint8_t)ct;
        return SendCtrl(buf, sizeof(buf));
    }

    bool Send(const EncodedFrame& f) {
        if(!IsStreaming()) { DBG("WebRTC: Send called but not streaming"); return false; }

        if(IsStale()) {
            WARN("WebRTC: Connection stale, resetting");
            Reset();
            if(cb.onDisconnect) cb.onDisconnect();
            return false;
        }

        size_t sz = f.data.size();
        size_t nch = (sz + DATA_CHUNK - 1) / DATA_CHUNK;

        if(!sz) { WARN("WebRTC: Send called with empty frame"); return false; }
        if(nch > 65535) {
            ERR("WebRTC: Frame too large (%zu bytes, %zu chunks)", sz, nch);
            return false;
        }

        uint32_t fid = frmId++;
        PacketHeader h = {f.ts, (uint32_t)f.encUs, fid, 0, (uint16_t)nch, f.isKey ? (uint8_t)1 : (uint8_t)0};

        {
            std::lock_guard<std::mutex> lk(sendMtx);
            size_t dropped = 0;
            while(vidQ.size() > nch*3) {
                vidQ.pop();
                needsKey = true;
                dropped++;
            }
            if(dropped > 0) {
                droppedVideoFrames += dropped;
                DBG("WebRTC: Dropped %zu old video packets (queue overflow)", dropped);
            }

            for(size_t i=0; i<nch; i++) {
                h.chunkIndex = (uint16_t)i;
                size_t off = i * DATA_CHUNK;
                size_t len = std::min(DATA_CHUNK, sz - off);
                std::vector<uint8_t> pkt(HDR_SZ + len);
                memcpy(pkt.data(), &h, HDR_SZ);
                memcpy(pkt.data() + HDR_SZ, f.data.data() + off, len);
                vidQ.push(std::move(pkt));
            }
        }

        DrainVid();
        totalVideoFramesSent++;
        LogStats();
        return true;
    }

    bool SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples) {
        if(!IsStreaming()) return false;
        if(data.empty()) { DBG("WebRTC: SendAudio called with empty data"); return false; }
        if(data.size() > 4000) {
            WARN("WebRTC: SendAudio packet too large (%zu bytes, max 4000)", data.size());
            return false;
        }

        std::vector<uint8_t> pkt(sizeof(AudioPacketHeader) + data.size());
        auto* h = (AudioPacketHeader*)pkt.data();
        h->magic = MSG_AUDIO_DATA;
        h->timestamp = ts;
        h->samples = (uint16_t)samples;
        h->dataLength = (uint16_t)data.size();
        memcpy(pkt.data() + sizeof(AudioPacketHeader), data.data(), data.size());

        if(ChOpen(dcAud) && dcAud->bufferedAmount() <= AUD_BUF/2) {
            try {
                dcAud->send((const std::byte*)pkt.data(), pkt.size());
                totalAudioPacketsSent++;
                return true;
            } catch(const std::exception& e) {
                audioSendErrors++;
                WARN("WebRTC: Audio send error: %s", e.what());
            } catch(...) {
                audioSendErrors++;
                WARN("WebRTC: Audio send unknown error");
            }
        }

        std::lock_guard<std::mutex> lk(sendMtx);
        size_t dropped = 0;
        while(audQ.size() >= 8) { audQ.pop(); dropped++; }
        if(dropped > 0) {
            droppedAudioPackets += dropped;
            DBG("WebRTC: Dropped %zu old audio packets", dropped);
        }
        audQ.push(std::move(pkt));
        DrainAud();
        return true;
    }

    void GetStats(uint64_t& vidSent, uint64_t& vidErr, uint64_t& audSent,
                  uint64_t& audErr, uint64_t& conns) {
        vidSent = totalVideoFramesSent.load();
        vidErr = videoSendErrors.load();
        audSent = totalAudioPacketsSent.load();
        audErr = audioSendErrors.load();
        conns = connectionCount.load();
    }
};
