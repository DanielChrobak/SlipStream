#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "audio.hpp"
#include "input.hpp"
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

std::atomic<bool> g_running{true};

BOOL WINAPI ConsoleHandler(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_CLOSE_EVENT || sig == CTRL_BREAK_EVENT) { printf("\n[Shutting down...]\n"); g_running = false; return TRUE; }
    return FALSE;
}

std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;
struct Config { std::string username, passwordHash, salt; } g_config;
JWTAuth g_jwt;
RateLimiter g_rateLimiter;

std::string GetMonitorFriendlyName(const wchar_t* gdiName) {
    UINT32 pathCnt = 0, modeCnt = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCnt, &modeCnt) != ERROR_SUCCESS || !pathCnt) return "";
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCnt);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCnt);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCnt, paths.data(), &modeCnt, modes.data(), nullptr) != ERROR_SUCCESS) return "";
    for (UINT32 i = 0; i < pathCnt; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {}; src.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(src), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && wcscmp(src.viewGdiDeviceName, gdiName) == 0) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {}; tgt.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(tgt), paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS && tgt.monitorFriendlyDeviceName[0]) {
                char name[64] = {}; WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1, name, sizeof(name), nullptr, nullptr);
                return name;
            }
        }
    }
    return "";
}

void RefreshMonitorList() {
    std::lock_guard<std::mutex> lk(g_monitorsMutex);
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* mons = (std::vector<MonitorInfo>*)lp;
        MONITORINFOEXW mi{sizeof(mi)}; DEVMODEW dm{.dmSize = sizeof(dm)};
        if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        std::string friendly = GetMonitorFriendlyName(mi.szDevice);
        char name[64] = {};
        if (!friendly.empty()) strncpy(name, friendly.c_str(), sizeof(name) - 1);
        else WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name, sizeof(name), nullptr, nullptr);
        mons->push_back({hMon, (int)mons->size(), mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            dm.dmDisplayFrequency ? (int)dm.dmDisplayFrequency : 60, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, name});
        return TRUE;
    }, (LPARAM)&g_monitors);
    std::sort(g_monitors.begin(), g_monitors.end(), [](auto& a, auto& b) { return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index; });
    for (size_t i = 0; i < g_monitors.size(); i++) g_monitors[i].index = (int)i;
}

std::string LoadFile(const char* path) { std::ifstream f(path); return f ? std::string(std::istreambuf_iterator<char>(f), {}) : ""; }

bool LoadConfig() {
    try {
        std::ifstream f("auth.json"); if (!f) return false;
        json c = json::parse(f);
        if (c.contains("username") && c.contains("passwordHash") && c.contains("salt")) {
            g_config = {c["username"], c["passwordHash"], c["salt"]};
            return g_config.username.size() >= 3 && g_config.passwordHash.size() == 64 && g_config.salt.size() == 32;
        }
    } catch (...) { ERR("LoadConfig failed"); }
    return false;
}

bool SaveConfig() {
    try { std::ofstream("auth.json") << json{{"username", g_config.username}, {"passwordHash", g_config.passwordHash}, {"salt", g_config.salt}}.dump(2); return true; }
    catch (...) { ERR("SaveConfig failed"); return false; }
}

bool ValidateUsername(const std::string& u) {
    if (u.length() < 3 || u.length() > 32) return false;
    for (char c : u) if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
    return true;
}

bool ValidatePassword(const std::string& p) {
    if (p.length() < 8 || p.length() > 128) return false;
    bool l = false, d = false;
    for (char c : p) { if (isalpha((unsigned char)c)) l = true; if (isdigit((unsigned char)c)) d = true; }
    return l && d;
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
    if (LoadConfig()) { printf("Loaded config (user: %s)\n", g_config.username.c_str()); return; }
    printf("\n=== First Time Setup ===\n");
    while (true) { printf("Username (3-32 chars): "); std::getline(std::cin, g_config.username); if (ValidateUsername(g_config.username)) break; printf("Invalid username\n"); }
    std::string pw, conf;
    while (true) {
        printf("Password (8+ chars, letter+number): "); pw = GetPasswordInput();
        if (!ValidatePassword(pw)) { printf("Invalid password\n"); continue; }
        printf("Confirm password: "); conf = GetPasswordInput();
        if (pw == conf) break; printf("Passwords don't match\n");
    }
    g_config.salt = GenerateSalt(); g_config.passwordHash = HashPassword(pw, g_config.salt);
    if (SaveConfig()) printf("Configuration saved\n\n"); else { printf("Failed to save\n"); SetupConfig(); }
}

std::string ExtractSessionCookie(const httplib::Request& req) {
    auto it = req.headers.find("Cookie"); if (it == req.headers.end()) return "";
    size_t pos = it->second.find("session="); if (pos == std::string::npos) return "";
    size_t start = pos + 8, end = it->second.find(';', start);
    return end != std::string::npos ? it->second.substr(start, end - start) : it->second.substr(start);
}

std::string GetClientIP(const httplib::Request& req) {
    auto it = req.headers.find("X-Forwarded-For");
    if (it != req.headers.end() && !it->second.empty()) { size_t c = it->second.find(','); return c != std::string::npos ? it->second.substr(0, c) : it->second; }
    return req.remote_addr;
}

void JsonError(httplib::Response& res, int status, const std::string& err) { res.status = status; res.set_content(json{{"error", err}}.dump(), "application/json"); }

template<typename F> auto AuthRequired(F h) {
    return [h](const httplib::Request& req, httplib::Response& res) {
        std::string token = ExtractSessionCookie(req), user;
        if (token.empty() || !g_jwt.ValidateToken(token, user)) { JsonError(res, 401, token.empty() ? "Authentication required" : "Invalid token"); return; }
        h(req, res, user);
    };
}

void HandleAuth(const httplib::Request& req, httplib::Response& res) {
    json body; try { body = json::parse(req.body); } catch (...) { ERR("HandleAuth JSON parse failed"); JsonError(res, 400, "Invalid JSON"); return; }
    std::string ip = GetClientIP(req);
    if (!g_rateLimiter.IsAllowed(ip)) { res.status = 429; res.set_content(json{{"error", "Too many attempts"}, {"lockoutSeconds", g_rateLimiter.LockoutSeconds(ip)}}.dump(), "application/json"); return; }
    std::string u = body.value("username", ""), p = body.value("password", "");
    if (u.empty() || p.empty()) { g_rateLimiter.RecordAttempt(ip, false); JsonError(res, 400, "Credentials required"); return; }
    if (u != g_config.username || !VerifyPassword(p, g_config.salt, g_config.passwordHash)) {
        g_rateLimiter.RecordAttempt(ip, false);
        res.status = 401; res.set_content(json{{"error", "Invalid credentials"}, {"remainingAttempts", g_rateLimiter.RemainingAttempts(ip)}}.dump(), "application/json"); return;
    }
    g_rateLimiter.RecordAttempt(ip, true);
    res.set_header("Set-Cookie", "session=" + g_jwt.CreateToken(u) + "; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400");
    res.set_content(json{{"success", true}, {"username", u}}.dump(), "application/json");
}

void SetupCORS(const httplib::Request& req, httplib::Response& r) {
    r.set_header("X-Content-Type-Options", "nosniff");
    r.set_header("X-Frame-Options", "DENY");
    r.set_header("Referrer-Policy", "no-referrer");
    std::string origin = req.get_header_value("Origin"); if (origin.empty()) return;
    std::string host = req.get_header_value("Host");
    size_t pe = origin.find("://"); std::string oh = (pe != std::string::npos) ? origin.substr(pe + 3) : origin;
    size_t pp = oh.find(':'), hp = host.find(':');
    std::string ohn = (pp != std::string::npos) ? oh.substr(0, pp) : oh, hn = (hp != std::string::npos) ? host.substr(0, hp) : host;
    if (ohn == "localhost" || ohn == "127.0.0.1" || hn == "localhost" || hn == "127.0.0.1" || ohn == hn) {
        r.set_header("Access-Control-Allow-Origin", origin);
        r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        r.set_header("Access-Control-Allow-Credentials", "true");
    }
}

std::string GetLocalIPAddress() {
    std::string result = "127.0.0.1";
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) return result;
    addrinfo hints = {}, *info = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &info) != 0 || !info) return result;
    for (auto* p = info; p; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            char ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip))) {
                std::string ipStr(ip);
                if (ipStr != "127.0.0.1" && ipStr.find("127.") != 0) { result = ipStr; break; }
            }
        }
    }
    freeaddrinfo(info);
    return result;
}

// Helper class to manage pending wiggle operations safely
class WiggleManager {
    std::atomic<bool>& running;
    InputHandler& input;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<int> pendingWiggles{0};
    std::thread workerThread;

public:
    WiggleManager(std::atomic<bool>& r, InputHandler& i) : running(r), input(i) {
        workerThread = std::thread([this] {
            while (running.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait_for(lk, 50ms, [this] {
                    return pendingWiggles.load(std::memory_order_acquire) > 0 || !running.load(std::memory_order_acquire);
                });
                if (!running.load(std::memory_order_acquire)) break;
                if (pendingWiggles.load(std::memory_order_acquire) > 0) {
                    pendingWiggles.store(0, std::memory_order_release);
                    lk.unlock();
                    std::this_thread::sleep_for(100ms);
                    if (running.load(std::memory_order_acquire)) {
                        input.WiggleCenter();
                    }
                }
            }
        });
    }

    ~WiggleManager() {
        cv.notify_all();
        if (workerThread.joinable()) workerThread.join();
    }

    void RequestWiggle() {
        pendingWiggles.fetch_add(1, std::memory_order_release);
        cv.notify_one();
    }
};

int main() {
    try {
        SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
        printf("\n=== SlipStream Server ===\n\n");
        SetupConfig();
        if (!EnsureSSLCert()) { ERR("Failed to initialize SSL certificates"); getchar(); return 1; }
        constexpr int PORT = 443;
        std::string localIP = GetLocalIPAddress();
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        FrameSlot frameSlot;
        auto rtc = std::make_shared<WebRTCServer>();
        ScreenCapture capture(&frameSlot);
        std::unique_ptr<VideoEncoder> encoder; std::mutex encMtx;
        std::atomic<bool> encReady{false};
        std::atomic<CodecType> curCodec{CODEC_AV1};

        InputHandler input; input.Enable();
        auto updateBounds = [&](int i) {
            std::lock_guard<std::mutex> lk(g_monitorsMutex);
            if (i >= 0 && i < (int)g_monitors.size()) input.UpdateFromMonitorInfo(g_monitors[i]);
        };
        updateBounds(capture.GetCurrentMonitorIndex());

        // Create wiggle manager to handle delayed wiggle operations safely
        WiggleManager wiggleManager(g_running, input);

        std::unique_ptr<AudioCapture> audio;
        try { audio = std::make_unique<AudioCapture>(); } catch (...) { ERR("AudioCapture init failed"); }

        auto mkEncoder = [&](int w, int h, int fps, CodecType cc) {
            std::lock_guard<std::mutex> lk(encMtx); encReady = false; encoder.reset();
            try { encoder = std::make_unique<VideoEncoder>(w, h, fps, capture.GetDev(), capture.GetCtx(), capture.GetMT(), cc); curCodec = cc; encReady = true; }
            catch (...) { ERR("Encoder init failed"); }
        };

        capture.SetResolutionChangeCallback([&](int w, int h, int fps) { mkEncoder(w, h, fps, curCodec.load()); });
        std::atomic<bool> cursorCaptureEnabled{false};

        rtc->Init({&input,
            [&](int fps, uint8_t) {
                capture.SetFPS(fps);
                { std::lock_guard<std::mutex> lk(encMtx);
                  if (encoder) encoder->UpdateFPS(fps);
                  else { encReady = false; try { encoder = std::make_unique<VideoEncoder>(capture.GetW(), capture.GetH(), fps, capture.GetDev(), capture.GetCtx(), capture.GetMT(), curCodec.load()); encReady = true; } catch (...) { ERR("Encoder init in FPS callback failed"); } } }
                if (!capture.IsCapturing()) capture.StartCapture(); frameSlot.Wake();
            },
            [&] { return capture.RefreshHostFPS(); },
            [&](int i) {
                bool ok = capture.SwitchMonitor(i);
                if (ok) {
                    updateBounds(i);
                    wiggleManager.RequestWiggle();  // Safe wiggle request
                }
                return ok;
            },
            [&] { return capture.GetCurrentMonitorIndex(); },
            [&] { capture.PauseCapture(); frameSlot.Wake(); },
            [&] {
                frameSlot.Wake();
                wiggleManager.RequestWiggle();  // Safe wiggle request
            },
            [&](CodecType c) -> bool { if (c == curCodec.load()) return true; try { mkEncoder(capture.GetW(), capture.GetH(), capture.GetCurrentFPS(), c); return true; } catch (...) { ERR("Codec change failed"); return false; } },
            [&] { return curCodec.load(); },
            [&] { return input.GetClipboardText(); },
            [&](const std::string& text) { return input.SetClipboardText(text); },
            [&](bool enabled) { cursorCaptureEnabled = enabled; capture.SetCursorCapture(enabled); }
        });

        httplib::SSLServer srv(SSL_CERT_FILE, SSL_KEY_FILE);
        if (!srv.is_valid()) { ERR("Failed to initialize HTTPS server"); getchar(); return 1; }

        srv.set_post_routing_handler(SetupCORS);
        srv.Options(".*", [](auto&, auto& r) { r.status = 204; });
        srv.Get("/", [](auto&, auto& r) { auto c = LoadFile("index.html"); r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html"); });
        srv.Get("/styles.css", [](auto&, auto& r) { r.set_content(LoadFile("styles.css"), "text/css"); });
        for (auto js : {"input", "media", "network", "renderer", "state", "ui"})
            srv.Get(std::string("/js/") + js + ".js", [js](auto&, auto& r) { r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()), "application/javascript"); });

        srv.Post("/api/auth", HandleAuth);
        srv.Post("/api/logout", [](auto&, auto& res) { res.set_header("Set-Cookie", "session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0"); res.set_content(R"({"success":true})", "application/json"); });
        srv.Get("/api/session", AuthRequired([](auto&, auto& res, const std::string& user) { res.set_content(json{{"valid", true}, {"username", user}}.dump(), "application/json"); }));
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
                if (size_t p = ans.find("a=setup:actpass"); p != std::string::npos) ans.replace(p, 15, "a=setup:active");
                res.set_content(json{{"sdp", ans}, {"type", "answer"}}.dump(), "application/json");
            } catch (...) { ERR("Offer handler failed"); JsonError(res, 400, "Invalid offer"); }
        }));

        std::thread srvThread([&] { srv.listen("0.0.0.0", PORT); });
        std::this_thread::sleep_for(100ms);
        printf("Server running on port %d\n", PORT);
        printf("  Local:   https://localhost:%d\n", PORT);
        printf("  Network: https://%s:%d\n", localIP.c_str(), PORT);
        printf("  User:    %s | Display: %dHz\n", g_config.username.c_str(), capture.GetHostFPS());
        printf("Note: Self-signed certificate - browser may show security warning.\n");

        if (audio) audio->Start();

        std::thread audioThread([&] {
            if (!audio) return;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AudioPacket pkt;
            while (g_running.load(std::memory_order_acquire)) {
                if (!rtc->IsStreaming()) { std::this_thread::sleep_for(10ms); continue; }
                if (audio->PopPacket(pkt, 5)) try { rtc->SendAudio(pkt.data, pkt.ts, pkt.samples); } catch (...) { ERR("SendAudio failed"); }
            }
        });

        std::thread cursorThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (g_running.load(std::memory_order_acquire)) {
                if (!rtc->IsStreaming() || cursorCaptureEnabled.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(50ms);
                    continue;
                }
                CursorType cursor;
                if (input.GetCurrentCursor(cursor)) try { rtc->SendCursorShape(cursor); } catch (...) { ERR("SendCursorShape failed"); }
                std::this_thread::sleep_for(33ms);
            }
        });

        std::thread encThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            FrameData fd; bool wasStreaming = false; int64_t period = 16667;
            while (g_running.load(std::memory_order_acquire)) {
                if (!frameSlot.Pop(fd)) { if (!g_running.load(std::memory_order_acquire)) break; continue; }
                int64_t handoff = GetTimestamp() - fd.ts;
                bool isStreaming = false;
                try { isStreaming = rtc->IsStreaming() && encReady.load(std::memory_order_acquire); } catch (...) { ERR("IsStreaming check failed"); }

                if (isStreaming && !wasStreaming) {
                    std::lock_guard<std::mutex> lk(encMtx);
                    if (encoder) encoder->Flush();
                    int fps = 60; try { fps = rtc->GetCurrentFps(); } catch (...) { ERR("GetCurrentFps failed"); }
                    period = fps > 0 ? 1000000 / fps : 16667;
                }
                wasStreaming = isStreaming;

                if (!isStreaming || !fd.tex) { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }

                int fps = 60; try { fps = rtc->GetCurrentFps(); } catch (...) { ERR("GetCurrentFps failed"); }
                if (fps > 0) period = 1000000 / fps;

                if (handoff > (period * 3) / 2) { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }
                if (fd.needsSync && !capture.WaitReady(fd.fence)) { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }

                try {
                    std::lock_guard<std::mutex> lk(encMtx);
                    if (encoder && rtc->IsStreaming()) {
                        bool needsKey = false; try { needsKey = rtc->NeedsKey(); } catch (...) { ERR("NeedsKey check failed"); needsKey = true; }
                        if (auto* out = encoder->Encode(fd.tex, fd.ts, needsKey)) try { rtc->Send(*out); } catch (...) { ERR("Send frame failed"); }
                    }
                } catch (...) { ERR("Encode loop failed"); }

                try {
                    std::lock_guard<std::mutex> lk(encMtx);
                    if (encoder && !encoder->IsEncodeComplete())
                        for (int r = 0; !encoder->IsEncodeComplete() && r < 8; r++) std::this_thread::sleep_for(std::chrono::microseconds(500));
                } catch (...) { ERR("Encode complete check failed"); }

                frameSlot.MarkReleased(fd.poolIdx); fd.Release();
            }
        });

        while (g_running.load(std::memory_order_acquire)) std::this_thread::sleep_for(100ms);

        // Orderly shutdown
        LOG("Initiating shutdown...");

        // Stop audio first (it's independent)
        if (audio) audio->Stop();

        // Stop server and wake frame slot
        srv.stop();
        frameSlot.Wake();

        // Set a timeout for thread cleanup
        std::atomic<bool> forceExit{false};
        std::thread timeoutThread([&] {
            std::this_thread::sleep_for(3s);
            if (!forceExit.load(std::memory_order_acquire)) {
                ERR("Shutdown timeout - forcing exit");
                ExitProcess(0);
            }
        });
        timeoutThread.detach();

        // Join threads in order
        if (encThread.joinable()) encThread.join();
        if (audioThread.joinable()) audioThread.join();
        if (cursorThread.joinable()) cursorThread.join();
        if (srvThread.joinable()) srvThread.join();

        forceExit.store(true, std::memory_order_release);
        LOG("Shutdown complete");

    } catch (const std::exception& e) { ERR("Fatal: %s", e.what()); getchar(); return 1; }
    return 0;
}
