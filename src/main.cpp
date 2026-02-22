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
            while (running.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(50ms);
                int64_t req = lastReq.load(std::memory_order_acquire);
                if (req > 0) {
                    int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
                    if (now - req >= 100) {
                        lastReq.store(0, std::memory_order_release);
                        input.WiggleCenter();
                    }
                }
            }
        });
    }
    ~WiggleManager() { if (thr.joinable()) thr.join(); }
    void Request() {
        lastReq.store(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(), std::memory_order_release);
    }
};

static bool JoinWithTimeout(std::thread& t, const char* name, DWORD ms) {
    if (!t.joinable()) return true;
    if (WaitForSingleObject(reinterpret_cast<HANDLE>(t.native_handle()), ms) == WAIT_OBJECT_0) {
        t.join();
        return true;
    }
    WARN("main: Timeout waiting for %s; waiting until completion", name);
    t.join();
    return true;
}

int main(int argc, char* argv[]) {
    try {
        InitLogging();
        for (int i = 1; i < argc; i++)
            if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0)
                g_debugLogging = true;

        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);

        printf("\n=== SlipStream Server v%s ===\n\n", SLIPSTREAM_VERSION);

        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            ERR("WSAStartup failed");
            return 1;
        }

        SetupConfig();
        if (!EnsureSSLCert()) { ERR("SSL cert init failed"); WSACleanup(); return 1; }

        constexpr int PORT = 443;
        auto localIPs = GetLocalIPAddresses();
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

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
            if (i >= 0 && i < (int)g_monitors.size())
                input.UpdateFromMonitorInfo(g_monitors[i]);
        };

        updateBounds(capture.GetCurrentMonitorIndex());
        WiggleManager wiggle(g_running, input);

        uint8_t codecCaps = VideoEncoder::ProbeSupport(capture.GetDev());
        LOG("Codec support: AV1=%d H265=%d H264=%d", (codecCaps&1)?1:0, (codecCaps&2)?1:0, (codecCaps&4)?1:0);

        std::unique_ptr<AudioCapture> audio;
        try { audio = std::make_unique<AudioCapture>(); } catch (...) { WARN("AudioCapture init failed"); }

        std::unique_ptr<MicPlayback> mic;
        try { mic = std::make_unique<MicPlayback>("CABLE Input"); } catch (...) { LOG("MicPlayback not available"); }

        auto mkEncoder = [&](int w, int h, int fps, CodecType cc) {
            std::lock_guard<std::mutex> lk(encMtx);
            encReady = false;
            encoder.reset();
            try {
                encoder = std::make_unique<VideoEncoder>(w, h, fps, capture.GetDev(), capture.GetCtx(), capture.GetMT(), cc);
                curCodec = cc;
                encReady = true;
            } catch (const std::exception& e) { ERR("Encoder creation failed: %s", e.what()); }
        };

        capture.SetResolutionChangeCallback([&](int w, int h, int fps) {
            LOG("Resolution change: %dx%d@%d", w, h, fps);
            mkEncoder(w, h, fps, curCodec.load());
        });

        std::atomic<bool> cursorCapture{false};
        std::atomic<int64_t> lastEncTs{0};
        std::atomic<int> targetFps{60};

        rtc->Init({
            &input,
            [&](int fps, uint8_t mode) {
                capture.SetFPS(fps);
                targetFps.store(fps, std::memory_order_release);
                lastEncTs.store(0, std::memory_order_release);
                std::lock_guard<std::mutex> lk(encMtx);
                if (encoder) encoder->UpdateFPS(fps);
                else {
                    try {
                        encoder = std::make_unique<VideoEncoder>(capture.GetW(), capture.GetH(), fps,
                            capture.GetDev(), capture.GetCtx(), capture.GetMT(), curCodec.load());
                        encReady = true;
                    } catch (...) {}
                }
                if (!capture.IsCapturing()) capture.StartCapture();
                frameSlot.Wake();
            },
            [&] { return capture.RefreshHostFPS(); },
            [&] { return capture.GetCurrentMonitorIndex(); },
            [&](int i) {
                bool ok = capture.SwitchMonitor(i);
                if (ok) { updateBounds(i); lastEncTs.store(0); wiggle.Request(); }
                return ok;
            },
            [&] { capture.PauseCapture(); frameSlot.Wake(); lastEncTs.store(0); if (audio) audio->SetStreaming(false); },
            [&] { frameSlot.Wake(); lastEncTs.store(0); wiggle.Request(); },
            [&](CodecType c) -> bool {
                if (c == curCodec.load()) return true;
                if (!(codecCaps & (1 << static_cast<int>(c)))) return false;
                mkEncoder(capture.GetW(), capture.GetH(), capture.GetCurrentFPS(), c);
                lastEncTs.store(0);
                return true;
            },
            [&] { return curCodec.load(); },
            [&] { return codecCaps; },
            [&] { return input.GetClipboardText(); },
            [&](const std::string& text) { return input.SetClipboardText(text); },
            [&](bool en) { cursorCapture = en; capture.SetCursorCapture(en); },
            [&](bool en) { if (audio) audio->SetStreaming(en); },
            [&](bool en) { if (mic) mic->SetStreaming(en); },
            [&](const uint8_t* data, size_t len) { if (mic && mic->IsInitialized()) mic->PushPacket(data, len); }
        });

        auto certPath = GetSSLCertFilePath();
        auto keyPath = GetSSLKeyFilePath();
        httplib::SSLServer srv(certPath.c_str(), keyPath.c_str());
        if (!srv.is_valid()) { ERR("HTTPS server init failed"); WSACleanup(); return 1; }

        srv.set_post_routing_handler(SetupCORS);
        srv.Options(".*", [](auto&, auto& r) { r.status = 204; });
        srv.Get("/", [](auto&, auto& r) {
            auto c = LoadFile("index.html");
            r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html");
        });
        srv.Get("/styles.css", [](auto&, auto& r) { r.set_content(LoadFile("styles.css"), "text/css"); });

        for (auto* js : {"input", "media", "network", "renderer", "state", "ui", "mic"})
            srv.Get(std::string("/js/") + js + ".js", [=](auto&, auto& r) {
                r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()), "application/javascript");
            });

        srv.Post("/api/auth", HandleAuth);
        srv.Post("/api/logout", [](auto&, auto& res) {
            res.set_header("Set-Cookie", "session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0");
            res.set_content(R"({"success":true})", "application/json");
        });
        srv.Get("/api/session", AuthRequired([](const httplib::Request&, httplib::Response& res, const std::string& user) {
            res.set_content(json{{"valid", true}, {"username", user}}.dump(), "application/json");
        }));

        srv.Post("/api/offer", AuthRequired([&](const httplib::Request& req, httplib::Response& res, const std::string&) {
            if (req.body.size() > 65536) { JsonError(res, 413, "Payload too large"); return; }
            try {
                auto body = json::parse(req.body);
                if (!body.contains("sdp") || !body["sdp"].is_string()) { JsonError(res, 400, "Missing SDP"); return; }
                std::string offer = body["sdp"];
                if (offer.empty() || offer.size() > 65536) { JsonError(res, 400, "Invalid SDP"); return; }

                rtc->SetRemote(offer, "offer");
                std::string ans = rtc->GetLocal();
                if (ans.empty()) { JsonError(res, 500, "Failed to generate answer"); return; }

                if (size_t p = ans.find("a=setup:actpass"); p != std::string::npos)
                    ans.replace(p, 15, "a=setup:active");
                res.set_content(json{{"sdp", ans}, {"type", "answer"}}.dump(), "application/json");
            } catch (...) { JsonError(res, 400, "Invalid offer"); }
        }));

        std::thread srvThread([&] { srv.listen("0.0.0.0", PORT); });
        std::this_thread::sleep_for(100ms);

        printf("SlipStream v%s on port %d\n", SLIPSTREAM_VERSION, PORT);
        printf("  Local: https://localhost:%d\n", PORT);
        for (const auto& ip : localIPs) printf("  Network: https://%s:%d\n", ip.c_str(), PORT);
        printf("  User: %s | Display: %dHz\n", g_config.username.c_str(), capture.GetHostFPS());
        printf("  Mic: %s\n", mic && mic->IsInitialized() ? mic->GetDeviceName().c_str() : "N/A");

        if (audio) audio->Start();
        if (mic) mic->Start();

        std::thread audioThread([&] {
            if (!audio) return;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AudioPacket pkt;
            while (g_running.load(std::memory_order_acquire)) {
                if (!rtc->IsStreaming()) { std::this_thread::sleep_for(10ms); continue; }
                if (audio->PopPacket(pkt, 5)) {
                    try { [[maybe_unused]] const bool sent = rtc->SendAudio(pkt.data, pkt.ts, pkt.samples); } catch (...) {}
                }
            }
        });

        std::thread cursorThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (g_running.load(std::memory_order_acquire)) {
                if (!rtc->IsStreaming() || cursorCapture.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(50ms);
                    continue;
                }
                CursorType cursor;
                if (input.GetCurrentCursor(cursor)) {
                    try { [[maybe_unused]] const bool sent = rtc->SendCursorShape(cursor); } catch (...) {}
                }
                std::this_thread::sleep_for(33ms);
            }
        });

        std::thread encThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            FrameData fd, pf{};
            bool wasStr = false, hasPf = false;
            int64_t period = 16667, nextTs = 0;
            uint64_t lastGen = frameSlot.GetGeneration();

            while (g_running.load(std::memory_order_acquire)) {
                if (!frameSlot.Pop(fd)) {
                    if (!g_running.load(std::memory_order_acquire)) break;
                    continue;
                }
                int64_t now = GetTimestamp();
                uint64_t curGen = frameSlot.GetGeneration();

                if (curGen != lastGen) {
                    if (hasPf) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false; }
                    lastGen = curGen;
                    nextTs = 0;
                }

                if (fd.generation != curGen) { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }

                bool isStr = false;
                try { isStr = rtc->IsStreaming() && encReady.load(std::memory_order_acquire); } catch (...) {}

                if (isStr && !wasStr) {
                    std::lock_guard<std::mutex> lk(encMtx);
                    if (encoder) encoder->Flush();
                    int fps = targetFps.load(std::memory_order_acquire);
                    if (fps <= 0) fps = 60;
                    period = 1000000 / fps;
                    lastEncTs.store(0);
                    nextTs = 0;
                    if (hasPf) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false; }
                }
                wasStr = isStr;

                if (!isStr || !fd.tex) { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }

                int fps = targetFps.load(std::memory_order_acquire);
                if (fps <= 0) fps = 60;
                period = 1000000 / fps;

                bool needsKey = false;
                try { needsKey = rtc->NeedsKey(); } catch (...) { needsKey = true; }

                if (nextTs == 0) nextTs = fd.ts;

                auto encodeAndSend = [&](FrameData& frame, bool key) {
                    if (frame.needsSync && !capture.WaitReady(frame.fence)) return false;
                    try {
                        std::lock_guard<std::mutex> lk(encMtx);
                        if (encoder && rtc->IsStreaming()) {
                            if (auto* out = encoder->Encode(frame.tex, frame.ts, key)) {
                                if (rtc->Send(*out)) lastEncTs.store(frame.ts, std::memory_order_release);
                            }
                        }
                    } catch (...) {}
                    try {
                        std::lock_guard<std::mutex> lk(encMtx);
                        if (encoder && !encoder->IsEncodeComplete())
                            for (int r = 0; !encoder->IsEncodeComplete() && r < 8; r++)
                                std::this_thread::sleep_for(std::chrono::microseconds(500));
                    } catch (...) {}
                    return true;
                };

                if (needsKey) {
                    if (hasPf) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false; }
                    if (encodeAndSend(fd, true)) nextTs = fd.ts + period;
                    frameSlot.MarkReleased(fd.poolIdx);
                    fd.Release();
                    continue;
                }

                if (fd.ts - nextTs < -period * 3 / 2) { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }

                if (hasPf) {
                    if (pf.generation != curGen) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false; }
                    if (hasPf) {
                        if (std::abs(fd.ts - nextTs) < std::abs(pf.ts - nextTs)) {
                            frameSlot.MarkReleased(pf.poolIdx); pf.Release(); pf = fd; fd = {};
                        } else { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); }
                    } else { pf = fd; fd = {}; hasPf = true; }
                } else { pf = fd; fd = {}; hasPf = true; }

                if (hasPf && (pf.ts >= nextTs || now >= nextTs + period / 2)) {
                    if (pf.generation != frameSlot.GetGeneration()) {
                        frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false;
                        continue;
                    }
                    if (now - pf.ts > period * 2) {
                        frameSlot.MarkReleased(pf.poolIdx); pf.Release(); hasPf = false;
                        while (nextTs < now - period) nextTs += period;
                        continue;
                    }
                    encodeAndSend(pf, false);
                    frameSlot.MarkReleased(pf.poolIdx);
                    pf.Release();
                    hasPf = false;
                    nextTs += period;
                    if (nextTs < now - period * 2) nextTs = now;
                }
            }
            if (hasPf) { frameSlot.MarkReleased(pf.poolIdx); pf.Release(); }
        });

        if (!InitAppTray()) WARN("Tray init failed");

        while (g_running.load(std::memory_order_acquire)) {
            PumpAppTrayMessages();
            std::this_thread::sleep_for(50ms);
        }

        LOG("Shutting down...");
        if (audio) audio->Stop();
        if (mic) mic->Stop();
        srv.stop();
        frameSlot.Wake();

        constexpr DWORD TIMEOUT = 5000;
        JoinWithTimeout(encThread, "encoder", TIMEOUT);
        JoinWithTimeout(audioThread, "audio", TIMEOUT);
        JoinWithTimeout(cursorThread, "cursor", TIMEOUT);
        JoinWithTimeout(srvThread, "server", TIMEOUT);

        if (rtc) {
            rtc->Shutdown();
            rtc.reset();
        }

        CleanupAppTray();
        WSACleanup();
        LOG("Shutdown complete");

    } catch (const std::exception& e) {
        CleanupAppTray();
        ERR("Fatal: %s", e.what());
        WSACleanup();
        return 1;
    }
    return 0;
}
