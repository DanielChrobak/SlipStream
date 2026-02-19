#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "audio.hpp"
#include "input.hpp"
#include "mic.hpp"
#include "app_support.hpp"
#include "tray.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>

class WiggleManager {
    std::atomic<bool>& running;
    InputHandler& input;
    std::atomic<int64_t> lastReq{0};
    std::thread thr;

public:
    WiggleManager(std::atomic<bool>& r, InputHandler& i) : running(r), input(i) {
        thr = std::thread([this] {
            DBG("WiggleManager: Thread started");
            while(running.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(50ms);
                int64_t req = lastReq.load(std::memory_order_acquire);
                if(req > 0) {
                    int64_t now = duration_cast<milliseconds>(
                        steady_clock::now().time_since_epoch()).count();
                    if(now - req >= 100) {
                        lastReq.store(0, std::memory_order_release);
                        input.WiggleCenter();
                        DBG("WiggleManager: Wiggle executed");
                    }
                }
            }
            DBG("WiggleManager: Thread exiting");
        });
    }

    ~WiggleManager() {
        if(thr.joinable()) thr.join();
        DBG("WiggleManager: Destroyed");
    }

    void Request() {
        lastReq.store(duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count(), std::memory_order_release);
    }
};

namespace {
bool JoinThreadWithTimeout(std::thread& t, const char* name, DWORD timeoutMs) {
    if(!t.joinable()) return true;

    HANDLE h = (HANDLE)t.native_handle();
    DWORD wait = WaitForSingleObject(h, timeoutMs);
    if(wait == WAIT_OBJECT_0) {
        t.join();
        DBG("main: Joined %s thread", name);
        return true;
    }

    WARN("main: Timeout waiting for %s thread (%lu ms); detaching", name, timeoutMs);
    t.detach();
    return false;
}
}

int main(int argc, char* argv[]) {
    try {
        for(int i = 1; i < argc; i++) {
            if(strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
                g_debugLogging = true;
                LOG("Debug logging enabled");
            }
        }

        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        if(!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
            WARN("main: SetConsoleCtrlHandler failed: %lu", GetLastError());
        }

        printf("\n=== SlipStream Server v%s ===\n\n", SLIPSTREAM_VERSION);

        WSADATA wsaData;
        int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if(wsaResult != 0) {
            ERR("main: WSAStartup failed: %d", wsaResult);
            getchar();
            return 1;
        }
        LOG("Winsock initialized (version %d.%d)", LOBYTE(wsaData.wVersion), HIBYTE(wsaData.wVersion));

        SetupConfig();
        if(!EnsureSSLCert()) {
            ERR("Failed to initialize SSL certificates");
            WSACleanup();
            getchar();
            return 1;
        }

        constexpr int PORT = 443;
        auto localIPs = GetLocalIPAddresses();

        if(!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
            WARN("main: SetPriorityClass failed: %lu", GetLastError());
        } else {
            DBG("main: Process priority set to ABOVE_NORMAL");
        }

        FrameSlot frameSlot;
        auto rtc = std::make_shared<WebRTCServer>();
        ScreenCapture capture(&frameSlot);

        std::unique_ptr<VideoEncoder> encoder;
        std::mutex encMtx;
        std::atomic<bool> encReady{false};
        std::atomic<CodecType> curCodec{CODEC_AV1};

        InputHandler input;
        input.Enable();

        auto updateBounds = [&](int i) {
            std::lock_guard<std::mutex> lk(g_monitorsMutex);
            if(i >= 0 && i < (int)g_monitors.size()) {
                input.UpdateFromMonitorInfo(g_monitors[i]);
                DBG("main: Input bounds updated for monitor %d", i);
            } else {
                WARN("main: updateBounds called with invalid index %d (have %zu monitors)",
                     i, g_monitors.size());
            }
        };

        updateBounds(capture.GetCurrentMonitorIndex());
        WiggleManager wiggle(g_running, input);

        uint8_t codecCaps = VideoEncoder::ProbeEncoderSupport(capture.GetDev());
        LOG("Codec support: AV1=%d H265=%d H264=%d",
            (codecCaps & 1) ? 1 : 0, (codecCaps & 2) ? 1 : 0, (codecCaps & 4) ? 1 : 0);

        std::unique_ptr<AudioCapture> audio;
        try {
            audio = std::make_unique<AudioCapture>();
            LOG("AudioCapture initialized");
        } catch(const std::exception& e) {
            WARN("AudioCapture initialization failed: %s", e.what());
        } catch(...) {
            WARN("AudioCapture initialization failed (unknown error)");
        }

        std::unique_ptr<MicPlayback> mic;
        try {
            mic = std::make_unique<MicPlayback>("CABLE Input");
            if(mic->IsInitialized()) {
                LOG("MicPlayback initialized: %s", mic->GetDeviceName().c_str());
            } else {
                WARN("MicPlayback created but not initialized");
            }
        } catch(const std::exception& e) {
            LOG("MicPlayback not available: %s", e.what());
        } catch(...) {
            LOG("MicPlayback not available (unknown error)");
        }

        auto mkEncoder = [&](int w, int h, int fps, CodecType cc) {
            std::lock_guard<std::mutex> lk(encMtx);
            encReady = false;
            encoder.reset();
            try {
                encoder = std::make_unique<VideoEncoder>(w, h, fps,
                    capture.GetDev(), capture.GetCtx(), capture.GetMT(), cc);
                curCodec = cc;
                encReady = true;
                LOG("Encoder created: %dx%d @ %dfps, codec=%d", w, h, fps, (int)cc);
            } catch(const std::exception& e) {
                ERR("Encoder creation failed: %s", e.what());
            } catch(...) {
                ERR("Encoder creation failed (unknown error)");
            }
        };

        capture.SetResolutionChangeCallback([&](int w, int h, int fps) {
            LOG("Resolution change detected: %dx%d @ %dfps", w, h, fps);
            mkEncoder(w, h, fps, curCodec.load());
        });

        std::atomic<bool> cursorCapture{false};
        std::atomic<int64_t> lastEncTs{0};
        std::atomic<int> targetFps{60};

        rtc->Init({
            &input,
            [&](int fps, uint8_t mode) {
                LOG("FPS change: %d (mode=%d)", fps, mode);
                capture.SetFPS(fps);
                targetFps.store(fps, std::memory_order_release);
                lastEncTs.store(0, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lk(encMtx);
                    if(encoder) {
                        encoder->UpdateFPS(fps);
                    } else {
                        encReady = false;
                        try {
                            encoder = std::make_unique<VideoEncoder>(
                                capture.GetW(), capture.GetH(), fps,
                                capture.GetDev(), capture.GetCtx(), capture.GetMT(), curCodec.load());
                            encReady = true;
                            LOG("Encoder created on FPS change");
                        } catch(const std::exception& e) {
                            ERR("Encoder creation on FPS change failed: %s", e.what());
                        } catch(...) {
                            ERR("Encoder creation on FPS change failed (unknown)");
                        }
                    }
                }
                if(!capture.IsCapturing()) capture.StartCapture();
                frameSlot.Wake();
            },
            [&] { return capture.RefreshHostFPS(); },
            [&] { return capture.GetCurrentMonitorIndex(); },
            [&](int i) {
                bool ok = capture.SwitchMonitor(i);
                if(ok) {
                    updateBounds(i);
                    lastEncTs.store(0, std::memory_order_release);
                    wiggle.Request();
                    LOG("Monitor switched to %d", i);
                } else {
                    WARN("Monitor switch to %d failed", i);
                }
                return ok;
            },
            [&] {
                LOG("Client disconnected");
                capture.PauseCapture();
                frameSlot.Wake();
                lastEncTs.store(0, std::memory_order_release);
                if(audio) audio->SetStreaming(false);
            },
            [&] {
                LOG("Client connected");
                frameSlot.Wake();
                lastEncTs.store(0, std::memory_order_release);
                wiggle.Request();
            },
            [&](CodecType c) -> bool {
                if(c == curCodec.load()) return true;
                if(!(codecCaps & (1 << (int)c))) {
                    WARN("Codec %d not supported", (int)c);
                    return false;
                }
                mkEncoder(capture.GetW(), capture.GetH(), capture.GetCurrentFPS(), c);
                lastEncTs.store(0, std::memory_order_release);
                LOG("Codec changed to %d", (int)c);
                return true;
            },
            [&] { return curCodec.load(); },
            [&] { return codecCaps; },
            [&] { return input.GetClipboardText(); },
            [&](const std::string& text) { return input.SetClipboardText(text); },
            [&](bool en) {
                cursorCapture = en;
                capture.SetCursorCapture(en);
                DBG("Cursor capture: %s", en ? "enabled" : "disabled");
            },
            [&](bool en) {
                if(audio) {
                    audio->SetStreaming(en);
                    LOG("Audio streaming: %s", en ? "enabled" : "disabled");
                } else {
                    DBG("Audio enable requested but AudioCapture not available");
                }
            },
            [&](bool en) {
                if(mic) {
                    mic->SetStreaming(en);
                    LOG("Mic streaming: %s", en ? "enabled" : "disabled");
                } else {
                    DBG("Mic enable requested but MicPlayback not available");
                }
            },
            [&](const uint8_t* data, size_t len) {
                if(mic && mic->IsInitialized()) { mic->PushPacket(data, len); }
            }
        });

        auto certPath = GetSSLCertFilePath();
        auto keyPath = GetSSLKeyFilePath();
        httplib::SSLServer srv(certPath.c_str(), keyPath.c_str());
        if(!srv.is_valid()) {
            ERR("Failed to initialize HTTPS server");
            WSACleanup();
            getchar();
            return 1;
        }
        LOG("HTTPS server initialized");

        srv.set_post_routing_handler(SetupCORS);
        srv.Options(".*", [](auto&, auto& r) { r.status = 204; });

        srv.Get("/", [](auto&, auto& r) {
            auto c = LoadFile("index.html");
            r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html");
        });

        srv.Get("/styles.css", [](auto&, auto& r) {
            r.set_content(LoadFile("styles.css"), "text/css");
        });

        for(auto* js : {"input", "media", "network", "renderer", "state", "ui", "mic"}) {
            srv.Get(std::string("/js/") + js + ".js", [=](auto&, auto& r) {
                r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()),
                              "application/javascript");
            });
        }

        srv.Post("/api/auth", HandleAuth);

        srv.Post("/api/logout", [](auto&, auto& res) {
            res.set_header("Set-Cookie",
                "session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0");
            res.set_content(R"({"success":true})", "application/json");
            DBG("Logout request processed");
        });

        srv.Get("/api/session", AuthRequired([](const httplib::Request&, httplib::Response& res, const std::string& user) {
            res.set_content(json{{"valid", true}, {"username", user}}.dump(), "application/json");
        }));

        srv.Post("/api/offer", AuthRequired([&](const httplib::Request& req,
                                                 httplib::Response& res,
                                                 const std::string& user) {
            DBG("WebRTC offer from user '%s' (%zu bytes)", user.c_str(), req.body.size());
            if(req.body.size() > 65536) {
                WARN("Offer payload too large: %zu bytes", req.body.size());
                JsonError(res, 413, "Payload too large");
                return;
            }

            try {
                auto body = json::parse(req.body);
                if(!body.contains("sdp") || !body["sdp"].is_string()) {
                    WARN("Offer missing SDP field");
                    JsonError(res, 400, "Missing SDP");
                    return;
                }

                std::string offer = body["sdp"];
                if(offer.empty() || offer.size() > 65536) {
                    WARN("Offer SDP invalid size: %zu", offer.size());
                    JsonError(res, 400, "Invalid SDP");
                    return;
                }

                rtc->SetRemote(offer, "offer");
                std::string ans = rtc->GetLocal();

                if(ans.empty()) {
                    ERR("Failed to generate WebRTC answer");
                    JsonError(res, 500, "Failed to generate answer");
                    return;
                }

                if(size_t p = ans.find("a=setup:actpass"); p != std::string::npos) {
                    ans.replace(p, 15, "a=setup:active");
                }

                res.set_content(json{{"sdp", ans}, {"type", "answer"}}.dump(), "application/json");
                LOG("WebRTC answer generated (%zu bytes)", ans.size());
            } catch(const json::exception& e) {
                WARN("Offer JSON parse error: %s", e.what());
                JsonError(res, 400, "Invalid offer");
            } catch(const std::exception& e) {
                ERR("Offer processing error: %s", e.what());
                JsonError(res, 500, "Internal error");
            } catch(...) {
                ERR("Offer processing unknown error");
                JsonError(res, 500, "Internal error");
            }
        }));

        std::thread srvThread([&] {
            DBG("HTTP server thread started");
            srv.listen("0.0.0.0", PORT);
            DBG("HTTP server thread exiting");
        });
        std::this_thread::sleep_for(100ms);

        printf("SlipStream v%s running on port %d\n", SLIPSTREAM_VERSION, PORT);
        printf("  Local:   https://localhost:%d\n", PORT);
        if(localIPs.empty()) {
            printf("  Network: (no non-loopback IPv4 addresses found)\n");
        } else {
            printf("  Network: https://%s:%d\n", localIPs[0].c_str(), PORT);
            for(size_t i = 1; i < localIPs.size(); i++) {
                printf("           https://%s:%d\n", localIPs[i].c_str(), PORT);
            }
        }
        printf("  User:    %s | Display: %dHz\n", g_config.username.c_str(), capture.GetHostFPS());
        printf("  Mic:     %s\n", mic && mic->IsInitialized() ? mic->GetDeviceName().c_str() : "Not available");
        printf("Note: Self-signed certificate - browser may show security warning.\n");

        if(audio) audio->Start();
        if(mic) mic->Start();

        std::thread audioThread([&] {
            DBG("Audio thread started");
            if(!audio) { DBG("Audio thread exiting (no audio capture)"); return; }
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AudioPacket pkt;
            uint64_t packetsSent = 0, sendErrors = 0;

            while(g_running.load(std::memory_order_acquire)) {
                if(!rtc->IsStreaming()) { std::this_thread::sleep_for(10ms); continue; }
                if(audio->PopPacket(pkt, 5)) {
                    try {
                        rtc->SendAudio(pkt.data, pkt.ts, pkt.samples);
                        packetsSent++;
                    } catch(const std::exception& e) {
                        if(++sendErrors % 100 == 1) {
                            WARN("Audio send error: %s (total errors: %llu)", e.what(), sendErrors);
                        }
                    } catch(...) {
                        if(++sendErrors % 100 == 1) {
                            WARN("Audio send unknown error (total: %llu)", sendErrors);
                        }
                    }
                }
            }
            LOG("Audio thread exiting (sent %llu packets, %llu errors)", packetsSent, sendErrors);
        });

        std::thread cursorThread([&] {
            DBG("Cursor thread started");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            uint64_t cursorsSent = 0;

            while(g_running.load(std::memory_order_acquire)) {
                if(!rtc->IsStreaming() || cursorCapture.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(50ms);
                    continue;
                }
                CursorType cursor;
                if(input.GetCurrentCursor(cursor)) {
                    try {
                        rtc->SendCursorShape(cursor);
                        cursorsSent++;
                    } catch(const std::exception& e) {
                        DBG("Cursor send error: %s", e.what());
                    } catch(...) {
                        DBG("Cursor send unknown error");
                    }
                }
                std::this_thread::sleep_for(33ms);
            }
            DBG("Cursor thread exiting (sent %llu cursor updates)", cursorsSent);
        });

        std::thread encThread([&] {
            DBG("Encoder thread started");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

            FrameData fd, pf{};
            bool wasStr = false, hasPf = false;
            int64_t period = 16667, nextTs = 0;
            uint64_t lastGen = frameSlot.GetGeneration();
            uint64_t framesEncoded = 0, framesDropped = 0, encodeErrors = 0;

            while(g_running.load(std::memory_order_acquire)) {
                if(!frameSlot.Pop(fd)) {
                    if(!g_running.load(std::memory_order_acquire)) break;
                    continue;
                }
                int64_t now = GetTimestamp();

                uint64_t curGen = frameSlot.GetGeneration();
                if(curGen != lastGen) {
                    if(hasPf) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false; }
                    lastGen = curGen;
                    nextTs = 0;
                    DBG("Encoder: Generation changed to %llu", curGen);
                }

                if(fd.generation != curGen) {
                    frameSlot.MarkReleased(fd.poolIdx);
                    fd.Release();
                    framesDropped++;
                    continue;
                }

                bool isStr = false;
                try {
                    isStr = rtc->IsStreaming() && encReady.load(std::memory_order_acquire);
                } catch(const std::exception& e) {
                    DBG("Encoder: IsStreaming check failed: %s", e.what());
                } catch(...) {
                    DBG("Encoder: IsStreaming check failed (unknown)");
                }

                if(isStr && !wasStr) {
                    std::lock_guard<std::mutex> lk(encMtx);
                    if(encoder) encoder->Flush();
                    int fps = targetFps.load(std::memory_order_acquire);
                    if(fps <= 0) fps = 60;
                    period = 1000000 / fps;
                    lastEncTs.store(0, std::memory_order_release);
                    nextTs = 0;
                    if(hasPf) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false; }
                    LOG("Encoder: Streaming started (fps=%d, period=%lldus)", fps, period);
                }
                wasStr = isStr;

                if(!isStr || !fd.tex) {
                    frameSlot.MarkReleased(fd.poolIdx);
                    fd.Release();
                    continue;
                }

                int fps = targetFps.load(std::memory_order_acquire);
                if(fps <= 0) fps = 60;
                period = 1000000 / fps;

                bool needsKey = false;
                try {
                    needsKey = rtc->NeedsKey();
                } catch(const std::exception& e) {
                    WARN("Encoder: NeedsKey check failed: %s", e.what());
                    needsKey = true;
                } catch(...) {
                    WARN("Encoder: NeedsKey check failed (unknown)");
                    needsKey = true;
                }

                if(nextTs == 0) nextTs = fd.ts;

                if(needsKey) {
                    if(hasPf) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false; }
                    if(fd.needsSync && !capture.WaitReady(fd.fence)) {
                        frameSlot.MarkReleased(fd.poolIdx);
                        fd.Release();
                        framesDropped++;
                        continue;
                    }
                    try {
                        std::lock_guard<std::mutex> lk(encMtx);
                        if(encoder && rtc->IsStreaming()) {
                            if(auto* out = encoder->Encode(fd.tex, fd.ts, true)) {
                                try {
                                    if(rtc->Send(*out)) {
                                        lastEncTs.store(fd.ts, std::memory_order_release);
                                        nextTs = fd.ts + period;
                                        framesEncoded++;
                                    }
                                } catch(const std::exception& e) {
                                    WARN("Encoder: Send keyframe failed: %s", e.what());
                                    encodeErrors++;
                                } catch(...) {
                                    WARN("Encoder: Send keyframe failed (unknown)");
                                    encodeErrors++;
                                }
                            }
                        }
                    } catch(const std::exception& e) {
                        WARN("Encoder: Encode keyframe failed: %s", e.what());
                        encodeErrors++;
                    } catch(...) {
                        WARN("Encoder: Encode keyframe failed (unknown)");
                        encodeErrors++;
                    }
                    try {
                        std::lock_guard<std::mutex> lk(encMtx);
                        if(encoder && !encoder->IsEncodeComplete()) {
                            for(int r = 0; !encoder->IsEncodeComplete() && r < 8; r++) {
                                std::this_thread::sleep_for(std::chrono::microseconds(500));
                            }
                        }
                    } catch(...) {}
                    frameSlot.MarkReleased(fd.poolIdx);
                    fd.Release();
                    continue;
                }

                if(fd.ts - nextTs < -period * 3 / 2) {
                    frameSlot.MarkReleased(fd.poolIdx);
                    fd.Release();
                    framesDropped++;
                    continue;
                }

                if(hasPf) {
                    if(pf.generation != curGen) {
                        frameSlot.MarkReleased(pf.poolIdx);
                        pf.Release();
                        hasPf = false;
                    }
                    if(hasPf) {
                        if(std::abs(fd.ts - nextTs) < std::abs(pf.ts - nextTs)) {
                            frameSlot.MarkReleased(pf.poolIdx);
                            pf.Release();
                            pf = fd;
                            fd = {};
                        } else {
                            frameSlot.MarkReleased(fd.poolIdx);
                            fd.Release();
                        }
                    } else {
                        pf = fd;
                        fd = {};
                        hasPf = true;
                    }
                } else {
                    pf = fd;
                    fd = {};
                    hasPf = true;
                }

                if(hasPf && (pf.ts >= nextTs || now >= nextTs + period / 2)) {
                    if(pf.generation != frameSlot.GetGeneration()) {
                        frameSlot.MarkReleased(pf.poolIdx);
                        pf.Release();
                        hasPf = false;
                        continue;
                    }

                    int64_t ho = now - pf.ts;
                    if(ho > period * 2) {
                        frameSlot.MarkReleased(pf.poolIdx);
                        pf.Release();
                        hasPf = false;
                        framesDropped++;
                        while(nextTs < now - period) nextTs += period;
                        continue;
                    }

                    if(pf.needsSync && !capture.WaitReady(pf.fence)) {
                        frameSlot.MarkReleased(pf.poolIdx);
                        pf.Release();
                        hasPf = false;
                        framesDropped++;
                        continue;
                    }

                    try {
                        std::lock_guard<std::mutex> lk(encMtx);
                        if(encoder && rtc->IsStreaming()) {
                            if(auto* out = encoder->Encode(pf.tex, pf.ts, false)) {
                                try {
                                    if(rtc->Send(*out)) {
                                        lastEncTs.store(pf.ts, std::memory_order_release);
                                        framesEncoded++;
                                    }
                                } catch(const std::exception& e) {
                                    DBG("Encoder: Send failed: %s", e.what());
                                    encodeErrors++;
                                } catch(...) {
                                    DBG("Encoder: Send failed (unknown)");
                                    encodeErrors++;
                                }
                            }
                        }
                    } catch(const std::exception& e) {
                        DBG("Encoder: Encode failed: %s", e.what());
                        encodeErrors++;
                    } catch(...) {
                        DBG("Encoder: Encode failed (unknown)");
                        encodeErrors++;
                    }

                    try {
                        std::lock_guard<std::mutex> lk(encMtx);
                        if(encoder && !encoder->IsEncodeComplete()) {
                            for(int r = 0; !encoder->IsEncodeComplete() && r < 8; r++) {
                                std::this_thread::sleep_for(std::chrono::microseconds(500));
                            }
                        }
                    } catch(...) {}

                    frameSlot.MarkReleased(pf.poolIdx);
                    pf.Release();
                    hasPf = false;
                    nextTs += period;
                    if(nextTs < now - period * 2) nextTs = now;
                }
            }

            if(hasPf) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); }
            LOG("Encoder thread exiting (encoded=%llu dropped=%llu errors=%llu)",
                framesEncoded, framesDropped, encodeErrors);
        });

        if(!InitAppTray()) { WARN("main: Tray initialization failed"); }

        while(g_running.load(std::memory_order_acquire)) {
            PumpAppTrayMessages();
            std::this_thread::sleep_for(50ms);
        }

        LOG("Initiating shutdown...");
        if(audio) audio->Stop();
        if(mic) mic->Stop();
        srv.stop();
        frameSlot.Wake();

        constexpr DWORD JOIN_TIMEOUT_MS = 5000;
        JoinThreadWithTimeout(encThread, "encoder", JOIN_TIMEOUT_MS);
        JoinThreadWithTimeout(audioThread, "audio", JOIN_TIMEOUT_MS);
        JoinThreadWithTimeout(cursorThread, "cursor", JOIN_TIMEOUT_MS);
        JoinThreadWithTimeout(srvThread, "server", JOIN_TIMEOUT_MS);

        CleanupAppTray();
        WSACleanup();
        LOG("Shutdown complete");

    } catch(const std::exception& e) {
        CleanupAppTray();
        ERR("Fatal: %s", e.what());
        WSACleanup();
        getchar();
        return 1;
    } catch(...) {
        CleanupAppTray();
        ERR("Fatal: Unknown exception");
        WSACleanup();
        getchar();
        return 1;
    }

    return 0;
}
