#include "hpp/common.hpp"
#include "hpp/capture.hpp"
#include "hpp/encoder.hpp"
#include "hpp/webrtc.hpp"
#include "hpp/audio.hpp"
#include "hpp/input.hpp"
#include "hpp/stats.hpp"
#include <conio.h>

std::atomic<bool> g_quitting{false};
std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;
struct Config { std::string username, passwordHash, salt; } g_config;
AuthManager g_auth;

void RefreshMonitorList() {
    std::lock_guard<std::mutex> lock(g_monitorsMutex);
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* mons = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
        MONITORINFOEXW mi{sizeof(mi)}; DEVMODEW dm{.dmSize = sizeof(dm)};
        char name[64] = {};
        if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        DISPLAY_DEVICEW dd = {sizeof(dd)};
        if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0])
            WideCharToMultiByte(CP_UTF8, 0, dd.DeviceString, -1, name, sizeof(name), nullptr, nullptr);
        else WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name, sizeof(name), nullptr, nullptr);
        mons->push_back({hMon, (int)mons->size(), mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top, dm.dmDisplayFrequency ? (int)dm.dmDisplayFrequency : 60,
            (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, name});
        return TRUE;
    }, (LPARAM)&g_monitors);
    std::sort(g_monitors.begin(), g_monitors.end(), [](auto& a, auto& b) { return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index; });
    for (size_t i = 0; i < g_monitors.size(); i++) g_monitors[i].index = (int)i;
    LOG("Found %zu monitor(s)", g_monitors.size());
}

std::string LoadFile(const char* path) { std::ifstream f(path); return f.is_open() ? std::string(std::istreambuf_iterator<char>(f), {}) : ""; }

bool LoadConfig() {
    try {
        std::ifstream f("auth.json"); if (!f.is_open()) return false;
        json c = json::parse(f);
        if (c.contains("username") && c.contains("passwordHash") && c.contains("salt")) {
            g_config = {c["username"], c["passwordHash"], c["salt"]};
            return g_config.username.size() >= 3 && g_config.passwordHash.size() == 64 && g_config.salt.size() == 32;
        }
    } catch (const std::exception& e) { ERR("Config load failed: %s", e.what()); }
    return false;
}

bool SaveConfig() {
    try { std::ofstream("auth.json") << json{{"username", g_config.username}, {"passwordHash", g_config.passwordHash}, {"salt", g_config.salt}}.dump(2); return true; }
    catch (...) { return false; }
}

bool ValidateUsername(const std::string& u) {
    if (u.length() < 3 || u.length() > 32) return false;
    for (char c : u) if (!isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

bool ValidatePassword(const std::string& p) {
    if (p.length() < 8 || p.length() > 128) return false;
    bool hasLetter = false, hasDigit = false;
    for (char c : p) { if (isalpha(c)) hasLetter = true; if (isdigit(c)) hasDigit = true; }
    return hasLetter && hasDigit;
}

std::string GetPasswordInput() {
    std::string pw;
    for (int ch; (ch = _getch()) != '\r' && ch != '\n';) {
        if (ch == 8 || ch == 127) { if (!pw.empty()) { pw.pop_back(); printf("\b \b"); } }
        else if (ch == 27) { while (!pw.empty()) { pw.pop_back(); printf("\b \b"); } }
        else if (ch >= 32 && ch <= 126) { pw += (char)ch; printf("*"); }
    }
    printf("\n"); return pw;
}

void SetupConfig() {
    if (LoadConfig()) { printf("\033[32mLoaded config (user: %s)\033[0m\n\n", g_config.username.c_str()); return; }
    printf("\n\033[1;36m=== First Time Setup ===\033[0m\n\n\033[1mAuthentication\033[0m\n");
    while (true) {
        printf("  Username (3-32 chars): "); std::getline(std::cin, g_config.username);
        if (ValidateUsername(g_config.username)) break;
        printf("  \033[31mInvalid username\033[0m\n");
    }
    std::string pw, conf;
    while (true) {
        printf("  Password (8+ chars, letter+number): "); pw = GetPasswordInput();
        if (!ValidatePassword(pw)) { printf("  \033[31mInvalid password\033[0m\n"); continue; }
        printf("  Confirm password: "); conf = GetPasswordInput();
        if (pw == conf) break;
        printf("  \033[31mPasswords don't match\033[0m\n");
    }
    g_config.salt = GenerateSalt(); g_config.passwordHash = HashPassword(pw, g_config.salt);
    if (SaveConfig()) printf("\n\033[32mConfiguration saved\033[0m\n\n");
    else { printf("\n\033[31mFailed to save\033[0m\n"); SetupConfig(); }
}

// HTTP helpers
std::string ExtractBearerToken(const httplib::Request& req) {
    auto it = req.headers.find("Authorization");
    return (it != req.headers.end() && it->second.substr(0, 7) == "Bearer ") ? it->second.substr(7) : "";
}

std::string GetClientIP(const httplib::Request& req) {
    auto it = req.headers.find("X-Forwarded-For");
    if (it != req.headers.end() && !it->second.empty()) {
        size_t c = it->second.find(',');
        return c != std::string::npos ? it->second.substr(0, c) : it->second;
    }
    return req.remote_addr;
}

void JsonError(httplib::Response& res, int status, const std::string& err) {
    res.status = status; res.set_content(json{{"error", err}}.dump(), "application/json");
}

template<typename F> auto AuthRequired(F h) {
    return [h](const httplib::Request& req, httplib::Response& res) {
        std::string t = ExtractBearerToken(req);
        if (t.empty() || !g_auth.ValidateSession(t)) { JsonError(res, 401, t.empty() ? "Authentication required" : "Session expired"); return; }
        h(req, res, t);
    };
}

// HTTP Handlers
void HandleAuth(const httplib::Request& req, httplib::Response& res) {
    json body;
    try { body = json::parse(req.body); } catch (...) { JsonError(res, 400, "Invalid JSON"); return; }

    std::string ip = GetClientIP(req);
    if (!g_auth.IsAllowed(ip)) {
        res.status = 429;
        res.set_content(json{{"error", "Too many attempts"}, {"lockoutSeconds", g_auth.LockoutSecondsRemaining(ip)}}.dump(), "application/json");
        WARN("Rate limited: %s", ip.c_str());
        return;
    }

    std::string u = body.value("username", ""), p = body.value("password", "");
    if (u.empty() || p.empty()) { g_auth.RecordAttempt(ip, false); JsonError(res, 400, "Credentials required"); return; }

    if (u != g_config.username || !VerifyPassword(p, g_config.salt, g_config.passwordHash)) {
        g_auth.RecordAttempt(ip, false);
        res.status = 401;
        res.set_content(json{{"error", "Invalid credentials"}, {"remainingAttempts", g_auth.RemainingAttempts(ip)}}.dump(), "application/json");
        WARN("Failed auth: %s (%s)", ip.c_str(), u.c_str());
        return;
    }

    g_auth.RecordAttempt(ip, true);
    res.set_content(json{{"token", g_auth.CreateSession(u, ip)}, {"expiresIn", 86400}}.dump(), "application/json");
    LOG("Auth: %s from %s", u.c_str(), ip.c_str());
}

void HandleLogout(const httplib::Request& req, httplib::Response& res) {
    if (auto t = ExtractBearerToken(req); !t.empty()) g_auth.InvalidateSession(t);
    res.set_content(R"({"success":true})", "application/json");
}

void HandleSession(const httplib::Request&, httplib::Response& res, const std::string& t) {
    res.set_content(json{{"valid", true}, {"username", g_auth.GetUsername(t)}}.dump(), "application/json");
}

void SetupCORS(const httplib::Request& req, httplib::Response& r) {
    std::string origin = req.get_header_value("Origin");
    if (!origin.empty()) {
        std::string host = req.get_header_value("Host");
        size_t pe = origin.find("://"); std::string oh = (pe != std::string::npos) ? origin.substr(pe + 3) : origin;
        size_t pp = oh.find(':'), hp = host.find(':');
        std::string ohn = (pp != std::string::npos) ? oh.substr(0, pp) : oh, hn = (hp != std::string::npos) ? host.substr(0, hp) : host;
        bool local = (ohn == "localhost" || ohn == "127.0.0.1" || hn == "localhost" || hn == "127.0.0.1");
        if (local || ohn == hn) {
            r.set_header("Access-Control-Allow-Origin", origin);
            r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            r.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            r.set_header("Access-Control-Allow-Credentials", "true");
        }
    }
    r.set_header("X-Content-Type-Options", "nosniff");
    r.set_header("X-Frame-Options", "SAMEORIGIN");
    r.set_header("Referrer-Policy", "strict-origin-when-cross-origin");
    r.set_header("X-XSS-Protection", "1; mode=block");
    r.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
    r.set_header("Pragma", "no-cache");
    r.set_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self' wss: ws:; frame-ancestors 'self'; form-action 'self'; base-uri 'self'");
}

int main() {
    try {
        SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
        if (HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); h != INVALID_HANDLE_VALUE) {
            DWORD m = 0; if (GetConsoleMode(h, &m)) SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
        puts("\n\033[1;36m=== SlipStream Server ===\033[0m\n");
        SetupConfig();

        constexpr int PORT = 6060;
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        FrameSlot frameSlot; EncoderThreadMetrics encMetrics;
        auto rtc = std::make_shared<WebRTCServer>();
        ScreenCapture capture(&frameSlot);
        std::unique_ptr<VideoEncoder> encoder; std::mutex encMtx;
        std::atomic<bool> encReady{false}, running{true};
        std::atomic<CodecType> currentCodec{CODEC_H264};

        InputHandler input; input.Enable();
        auto updateBounds = [&](int i) { std::lock_guard<std::mutex> lk(g_monitorsMutex); if (i >= 0 && i < (int)g_monitors.size()) input.UpdateFromMonitorInfo(g_monitors[i]); };
        updateBounds(capture.GetCurrentMonitorIndex());

        std::unique_ptr<AudioCapture> audio;
        try { audio = std::make_unique<AudioCapture>(); } catch (...) {}

        auto mkEncoder = [&](int w, int h, int fps, CodecType codec) {
            std::lock_guard<std::mutex> lk(encMtx); encReady = false; encoder.reset();
            try {
                encoder = std::make_unique<VideoEncoder>(w, h, fps, capture.GetDev(), capture.GetCtx(), capture.GetMT(), codec);
                currentCodec = codec; encReady = true;
                LOG("Encoder: %dx%d@%d codec=%s", w, h, fps, codec == CODEC_H264 ? "H.264" : "AV1");
            } catch (const std::exception& e) { ERR("Encoder: %s", e.what()); }
        };

        mkEncoder(capture.GetW(), capture.GetH(), capture.GetHostFPS(), CODEC_H264);
        capture.SetResolutionChangeCallback([&](int w, int h, int fps) { mkEncoder(w, h, fps, currentCodec.load()); });

        rtc->Init({&input,
            [&](int fps, uint8_t) { capture.SetFPS(fps); if (!capture.IsCapturing()) capture.StartCapture(); frameSlot.Wake(); },
            [&] { return capture.RefreshHostFPS(); },
            [&](int i) { bool ok = capture.SwitchMonitor(i); if (ok) { updateBounds(i); std::thread([&] { std::this_thread::sleep_for(100ms); input.WiggleCenter(); }).detach(); } return ok; },
            [&] { return capture.GetCurrentMonitorIndex(); },
            [&] { capture.PauseCapture(); frameSlot.Wake(); },
            [&] { frameSlot.Wake(); std::thread([&] { std::this_thread::sleep_for(100ms); input.WiggleCenter(); }).detach(); },
            [&](CodecType newCodec) -> bool {
                if (newCodec == currentCodec.load()) return true;
                LOG("Switching codec to %s", newCodec == CODEC_H264 ? "H.264" : "AV1");
                try { mkEncoder(capture.GetW(), capture.GetH(), capture.GetCurrentFPS(), newCodec); return true; }
                catch (const std::exception& e) { ERR("Codec switch failed: %s", e.what()); return false; }
            },
            [&] { return currentCodec.load(); }
        });

        httplib::Server srv;
        srv.set_post_routing_handler(SetupCORS);
        srv.Options(".*", [](auto&, auto& r) { r.status = 204; });
        srv.Get("/", [](auto&, auto& r) { auto c = LoadFile("index.html"); r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html"); });
        srv.Get("/styles.css", [](auto&, auto& r) { r.set_content(LoadFile("styles.css"), "text/css"); });
        for (auto js : {"input", "media", "network", "renderer", "state", "ui"})
            srv.Get(std::string("/js/") + js + ".js", [js](auto&, auto& r) { r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()), "application/javascript"); });

        srv.Post("/api/auth", HandleAuth);
        srv.Post("/api/logout", HandleLogout);
        srv.Get("/api/session", AuthRequired(HandleSession));

        srv.Post("/api/offer", AuthRequired([&](const httplib::Request& req, httplib::Response& res, const std::string& t) {
            constexpr size_t MAX_SDP = 65536;
            if (req.body.size() > MAX_SDP) { JsonError(res, 413, "Payload too large"); return; }
            try {
                auto body = json::parse(req.body);
                if (!body.contains("sdp") || !body["sdp"].is_string()) { JsonError(res, 400, "Missing or invalid SDP"); return; }
                std::string offer = body["sdp"];
                if (offer.empty() || offer.size() > MAX_SDP) { JsonError(res, 400, "Invalid SDP size"); return; }
                LOG("Offer from %s (%s)", g_auth.GetUsername(t).c_str(), GetClientIP(req).c_str());
                rtc->SetRemote(offer, "offer");
                std::string ans = rtc->GetLocal();
                if (ans.empty()) { JsonError(res, 500, "Failed to generate answer"); return; }
                if (size_t p = ans.find("a=setup:actpass"); p != std::string::npos) ans.replace(p, 15, "a=setup:active");
                res.set_content(json{{"sdp", ans}, {"type", "answer"}}.dump(), "application/json");
                LOG("Answer to %s", g_auth.GetUsername(t).c_str());
            } catch (const std::exception& e) { ERR("Offer: %s", e.what()); JsonError(res, 400, "Invalid offer"); }
        }));

        std::thread srvThread([&] { srv.listen("0.0.0.0", PORT); });
        std::this_thread::sleep_for(100ms);
        std::thread cleanThread([&] { while (running && !g_quitting) { std::this_thread::sleep_for(std::chrono::minutes(5)); g_auth.Cleanup(); } });

        printf("\n\033[1;36m========================================\033[0m\n");
        printf("\033[1;36m          SLIPSTREAM SERVER           \033[0m\n");
        printf("\033[1;36m========================================\033[0m\n");
        printf("  Local: http://localhost:%d\n", PORT);
        printf("  User: %s | %dHz | Mode: no-vsync\n", g_config.username.c_str(), capture.GetHostFPS());
        printf("\033[1;36m========================================\033[0m\n\n");

        if (audio) audio->Start();

        std::thread audioThread([&] {
            if (!audio) return;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AudioPacket pkt;
            while (running && !g_quitting) {
                if (!rtc->IsStreaming()) { std::this_thread::sleep_for(10ms); continue; }
                if (audio->PopPacket(pkt, 5)) rtc->SendAudio(pkt.data, pkt.ts, pkt.samples);
            }
        });

        StatsLogger stats(running, g_quitting, rtc, capture, encoder, encMtx, input, frameSlot, encMetrics, g_config.username);
        std::thread statsThread([&] { stats.Run(); });

        std::thread encThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            FrameData fd; bool wasStreaming = false; int64_t period = 16667;
            while (running && !g_quitting) {
                if (!frameSlot.Pop(fd)) { if (g_quitting || !running) break; continue; }
                int64_t popTime = GetTimestamp(), handoff = popTime - fd.ts;
                bool isStreaming = rtc->IsStreaming() && encReady.load();
                if (isStreaming && !wasStreaming) {
                    LOG("Streaming at %d FPS", rtc->GetCurrentFps());
                    std::lock_guard<std::mutex> lk(encMtx); if (encoder) encoder->Flush();
                    int fps = rtc->GetCurrentFps(); period = fps > 0 ? 1000000 / fps : 16667;
                }
                wasStreaming = isStreaming;
                if (!isStreaming || !fd.tex) { encMetrics.stateDropCount++; frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }
                if (int fps = rtc->GetCurrentFps(); fps > 0) period = 1000000 / fps;
                int64_t deadline = (period * 3) / 2, late = handoff - deadline;
                if (late > 0) { encMetrics.RecordDeadlineMiss(late); WARN("Frame late %.2fms", late / 1000.0); frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }
                encMetrics.RecordHandoff(handoff);
                if (fd.needsSync && !capture.WaitReady(fd.fence)) { WARN("Capture fence timeout ts=%lld", fd.ts); encMetrics.stateDropCount++; frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }
                int64_t encodeStart = GetTimestamp(); bool encoded = false;
                { std::lock_guard<std::mutex> lk(encMtx); if (encoder) if (auto* out = encoder->Encode(fd.tex, fd.ts, rtc->NeedsKey())) { rtc->Send(*out); encoded = true; } }
                encMetrics.RecordEncode(GetTimestamp() - encodeStart);
                if (encoded) {
                    std::lock_guard<std::mutex> lk(encMtx);
                    if (encoder && !encoder->IsEncodeComplete()) {
                        int retries = 0;
                        while (!encoder->IsEncodeComplete() && retries++ < 8) std::this_thread::sleep_for(std::chrono::microseconds(500));
                        if (retries >= 8) WARN("Encoder GPU work incomplete");
                    }
                }
                frameSlot.MarkReleased(fd.poolIdx); fd.Release();
            }
        });

        while (!g_quitting && running) std::this_thread::sleep_for(100ms);
        LOG("Shutting down..."); running = false;
        if (audio) audio->Stop();
        srv.stop(); frameSlot.Wake();

        std::thread watchdog([&] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!g_quitting) return;
            WARN("Shutdown timeout - forcing exit"); ExitProcess(0);
        }); watchdog.detach();

        srvThread.join(); encThread.join(); audioThread.join(); statsThread.join(); cleanThread.join();
        LOG("Shutdown complete");
    } catch (const std::exception& e) { ERR("Fatal: %s", e.what()); getchar(); return 1; }
    return 0;
}
