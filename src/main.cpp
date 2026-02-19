#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "audio.hpp"
#include "input.hpp"
#include "mic.hpp"
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

std::atomic<bool> g_running{true};
std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;
struct Config { std::string username, passwordHash, salt; } g_config;
JWTAuth g_jwt;
RateLimiter g_rateLimiter;

BOOL WINAPI ConsoleHandler(DWORD sig) {
    if(sig==CTRL_C_EVENT || sig==CTRL_CLOSE_EVENT || sig==CTRL_BREAK_EVENT) {
        printf("\n[Shutting down...]\n");
        g_running = false;
        return TRUE;
    }
    DBG("ConsoleHandler: Received signal %lu", sig);
    return FALSE;
}

std::string GetMonitorFriendlyName(const wchar_t* gdiName) {
    UINT32 pathCnt=0, modeCnt=0;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCnt, &modeCnt);
    if(result != ERROR_SUCCESS) {
        DBG("GetMonitorFriendlyName: GetDisplayConfigBufferSizes failed: %ld", result);
        return "";
    }
    if(!pathCnt) { DBG("GetMonitorFriendlyName: No active paths found"); return ""; }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCnt);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCnt);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCnt, paths.data(),
                                &modeCnt, modes.data(), nullptr);
    if(result != ERROR_SUCCESS) {
        DBG("GetMonitorFriendlyName: QueryDisplayConfig failed: %ld", result);
        return "";
    }

    for(UINT32 i=0; i<pathCnt; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(src),
                      paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
        LONG srcResult = DisplayConfigGetDeviceInfo(&src.header);
        if(srcResult != ERROR_SUCCESS) {
            DBG("GetMonitorFriendlyName: DisplayConfigGetDeviceInfo (source) failed: %ld", srcResult);
            continue;
        }
        if(wcscmp(src.viewGdiDeviceName, gdiName) == 0) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
            tgt.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(tgt),
                          paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
            LONG tgtResult = DisplayConfigGetDeviceInfo(&tgt.header);
            if(tgtResult != ERROR_SUCCESS) {
                DBG("GetMonitorFriendlyName: DisplayConfigGetDeviceInfo (target) failed: %ld", tgtResult);
                continue;
            }
            if(tgt.monitorFriendlyDeviceName[0]) {
                char name[64] = {};
                int converted = WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1,
                                                    name, sizeof(name), nullptr, nullptr);
                if(converted == 0) { WARN("GetMonitorFriendlyName: WideCharToMultiByte failed: %lu", GetLastError()); }
                return name;
            }
        }
    }
    return "";
}

void RefreshMonitorList() {
    std::lock_guard<std::mutex> lk(g_monitorsMutex);
    g_monitors.clear();

    BOOL enumResult = EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* mons = (std::vector<MonitorInfo>*)lp;
        MONITORINFOEXW mi{sizeof(mi)};
        DEVMODEW dm{.dmSize = sizeof(dm)};

        if(!GetMonitorInfoW(hMon, &mi)) {
            WARN("RefreshMonitorList: GetMonitorInfoW failed: %lu", GetLastError());
            return TRUE;
        }
        if(!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            DBG("RefreshMonitorList: EnumDisplaySettingsW failed, using 60Hz default");
        }

        std::string friendly = GetMonitorFriendlyName(mi.szDevice);
        char name[64] = {};
        if(!friendly.empty()) {
            strncpy(name, friendly.c_str(), sizeof(name)-1);
        } else {
            int converted = WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1,
                                                name, sizeof(name), nullptr, nullptr);
            if(converted == 0) {
                WARN("RefreshMonitorList: WideCharToMultiByte failed: %lu", GetLastError());
                strcpy(name, "Unknown");
            }
        }

        int refreshRate = dm.dmDisplayFrequency ? (int)dm.dmDisplayFrequency : 60;
        int width = mi.rcMonitor.right - mi.rcMonitor.left;
        int height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        bool isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        mons->push_back({hMon, (int)mons->size(), width, height, refreshRate, isPrimary, name});
        DBG("RefreshMonitorList: Found monitor %zu: %s (%dx%d @ %dHz, primary=%d)",
            mons->size()-1, name, width, height, refreshRate, isPrimary);
        return TRUE;
    }, (LPARAM)&g_monitors);

    if(!enumResult) { ERR("RefreshMonitorList: EnumDisplayMonitors failed: %lu", GetLastError()); }

    std::sort(g_monitors.begin(), g_monitors.end(), [](auto& a, auto& b) {
        return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index;
    });
    for(size_t i=0; i<g_monitors.size(); i++) g_monitors[i].index = (int)i;
    LOG("RefreshMonitorList: Found %zu monitors", g_monitors.size());
}

std::string LoadFile(const char* path) {
    std::ifstream f(path);
    if(!f) { DBG("LoadFile: Failed to open '%s'", path); return ""; }
    std::string content(std::istreambuf_iterator<char>(f), {});
    DBG("LoadFile: Loaded '%s' (%zu bytes)", path, content.size());
    return content;
}

bool LoadConfig() {
    try {
        auto authPath = GetSlipStreamDataFilePath("auth.json");
        std::ifstream f(authPath);
        if(!f) {
            f.open("auth.json");
            if(!f) {
                DBG("LoadConfig: auth.json not found in data dir or working directory");
                return false;
            }
            LOG("LoadConfig: Using legacy auth.json from working directory");
        }
        json c = json::parse(f);
        if(c.contains("username") && c.contains("passwordHash") && c.contains("salt")) {
            g_config = {c["username"], c["passwordHash"], c["salt"]};
            bool valid = g_config.username.size() >= 3 &&
                         g_config.passwordHash.size() == 64 &&
                         g_config.salt.size() == 32;
            if(!valid) {
                WARN("LoadConfig: Config validation failed (username=%zu, hash=%zu, salt=%zu)",
                     g_config.username.size(), g_config.passwordHash.size(), g_config.salt.size());
            }
            return valid;
        }
        WARN("LoadConfig: Missing required fields in auth.json");
    } catch(const json::exception& e) {
        ERR("LoadConfig: JSON parse error: %s", e.what());
    } catch(const std::exception& e) {
        ERR("LoadConfig: Error loading config: %s", e.what());
    } catch(...) {
        ERR("LoadConfig: Unknown error loading config");
    }
    return false;
}

bool SaveConfig() {
    try {
        auto authPath = GetSlipStreamDataFilePath("auth.json");
        std::ofstream f(authPath);
        if(!f) { ERR("SaveConfig: Failed to open %s for writing", authPath.c_str()); return false; }
        f << json{{"username", g_config.username},
                  {"passwordHash", g_config.passwordHash},
                  {"salt", g_config.salt}}.dump(2);
        if(!f.good()) { ERR("SaveConfig: Error writing to %s", authPath.c_str()); return false; }
        LOG("SaveConfig: Configuration saved successfully");
        return true;
    } catch(const std::exception& e) {
        ERR("SaveConfig: Exception: %s", e.what());
        return false;
    } catch(...) {
        ERR("SaveConfig: Unknown exception");
        return false;
    }
}

bool ValidateUsername(const std::string& u) {
    if(u.length() < 3 || u.length() > 32) {
        DBG("ValidateUsername: Length invalid (%zu, must be 3-32)", u.length());
        return false;
    }
    for(char c : u) {
        if(!isalnum((unsigned char)c) && c != '_' && c != '-') {
            DBG("ValidateUsername: Invalid character '%c'", c);
            return false;
        }
    }
    return true;
}

bool ValidatePassword(const std::string& p) {
    if(p.length() < 8 || p.length() > 128) {
        DBG("ValidatePassword: Length invalid (%zu, must be 8-128)", p.length());
        return false;
    }
    bool l = false, d = false;
    for(char c : p) {
        if(isalpha((unsigned char)c)) l = true;
        if(isdigit((unsigned char)c)) d = true;
    }
    if(!l || !d) { DBG("ValidatePassword: Missing letter or digit (hasLetter=%d, hasDigit=%d)", l, d); }
    return l && d;
}

std::string GetPasswordInput() {
    std::string pw;
    for(int ch; (ch = _getch()) != '\r' && ch != '\n';) {
        if(ch == 8 || ch == 127) {
            if(!pw.empty()) { pw.pop_back(); printf("\b \b"); }
        } else if(ch == 27) {
            while(!pw.empty()) { pw.pop_back(); printf("\b \b"); }
        } else if(ch >= 32 && ch <= 126) {
            pw += (char)ch;
            printf("*");
        }
    }
    printf("\n");
    return pw;
}

void SetupConfig() {
    if(LoadConfig()) {
        printf("Loaded config (user: %s)\n", g_config.username.c_str());
        return;
    }
    printf("\n=== First Time Setup ===\n");

    while(true) {
        printf("Username (3-32 chars): ");
        std::getline(std::cin, g_config.username);
        if(ValidateUsername(g_config.username)) break;
        printf("Invalid username\n");
    }

    std::string pw, conf;
    while(true) {
        printf("Password (8+ chars, letter+number): ");
        pw = GetPasswordInput();
        if(!ValidatePassword(pw)) { printf("Invalid password\n"); continue; }
        printf("Confirm password: ");
        conf = GetPasswordInput();
        if(pw == conf) break;
        printf("Passwords don't match\n");
    }

    g_config.salt = GenerateSalt();
    g_config.passwordHash = HashPassword(pw, g_config.salt);

    if(g_config.passwordHash.empty()) {
        ERR("SetupConfig: Password hashing failed");
        printf("Failed to hash password\n");
        SetupConfig();
        return;
    }

    if(SaveConfig()) { printf("Configuration saved\n\n"); }
    else { printf("Failed to save\n"); SetupConfig(); }
}

std::string ExtractSessionCookie(const httplib::Request& req) {
    auto it = req.headers.find("Cookie");
    if(it == req.headers.end()) return "";
    size_t pos = it->second.find("session=");
    if(pos == std::string::npos) return "";
    size_t start = pos + 8, end = it->second.find(';', start);
    return end != std::string::npos ? it->second.substr(start, end - start) : it->second.substr(start);
}

std::string GetClientIP(const httplib::Request& req) {
    auto it = req.headers.find("X-Forwarded-For");
    if(it != req.headers.end() && !it->second.empty()) {
        size_t c = it->second.find(',');
        return c != std::string::npos ? it->second.substr(0, c) : it->second;
    }
    return req.remote_addr;
}

void JsonError(httplib::Response& res, int status, const std::string& err) {
    res.status = status;
    res.set_content(json{{"error", err}}.dump(), "application/json");
    DBG("JsonError: %d - %s", status, err.c_str());
}

template<typename F>
auto AuthRequired(F h) {
    return [h](const httplib::Request& req, httplib::Response& res) {
        std::string token = ExtractSessionCookie(req), user;
        if(token.empty()) {
            DBG("AuthRequired: No session cookie");
            JsonError(res, 401, "Authentication required");
            return;
        }
        if(!g_jwt.ValidateToken(token, user)) {
            DBG("AuthRequired: Invalid token");
            JsonError(res, 401, "Invalid token");
            return;
        }
        h(req, res, user);
    };
}

void HandleAuth(const httplib::Request& req, httplib::Response& res) {
    json body;
    try { body = json::parse(req.body); }
    catch(const json::exception& e) {
        WARN("HandleAuth: JSON parse error: %s", e.what());
        JsonError(res, 400, "Invalid JSON");
        return;
    } catch(...) {
        WARN("HandleAuth: Unknown JSON parse error");
        JsonError(res, 400, "Invalid JSON");
        return;
    }

    std::string ip = GetClientIP(req);
    if(!g_rateLimiter.IsAllowed(ip)) {
        int lockout = g_rateLimiter.LockoutSeconds(ip);
        WARN("HandleAuth: Rate limited IP %s (lockout=%ds)", ip.c_str(), lockout);
        res.status = 429;
        res.set_content(json{{"error", "Too many attempts"}, {"lockoutSeconds", lockout}}.dump(),
                        "application/json");
        return;
    }

    std::string u = body.value("username", ""), p = body.value("password", "");
    if(u.empty() || p.empty()) {
        g_rateLimiter.RecordAttempt(ip, false);
        DBG("HandleAuth: Empty credentials from %s", ip.c_str());
        JsonError(res, 400, "Credentials required");
        return;
    }

    if(u != g_config.username || !VerifyPassword(p, g_config.salt, g_config.passwordHash)) {
        g_rateLimiter.RecordAttempt(ip, false);
        int remaining = g_rateLimiter.RemainingAttempts(ip);
        WARN("HandleAuth: Failed login attempt for '%s' from %s (%d attempts remaining)",
             u.c_str(), ip.c_str(), remaining);
        res.status = 401;
        res.set_content(json{{"error", "Invalid credentials"},
                             {"remainingAttempts", remaining}}.dump(), "application/json");
        return;
    }

    g_rateLimiter.RecordAttempt(ip, true);
    std::string token = g_jwt.CreateToken(u);
    if(token.empty()) {
        ERR("HandleAuth: Failed to create JWT token for '%s'", u.c_str());
        JsonError(res, 500, "Internal error");
        return;
    }

    LOG("HandleAuth: Successful login for '%s' from %s", u.c_str(), ip.c_str());
    res.set_header("Set-Cookie", "session=" + token +
                   "; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400");
    res.set_content(json{{"success", true}, {"username", u}}.dump(), "application/json");
}

void SetupCORS(const httplib::Request& req, httplib::Response& r) {
    r.set_header("X-Content-Type-Options", "nosniff");
    r.set_header("X-Frame-Options", "DENY");
    r.set_header("Referrer-Policy", "no-referrer");

    std::string origin = req.get_header_value("Origin");
    if(origin.empty()) return;

    std::string host = req.get_header_value("Host");
    size_t pe = origin.find("://");
    std::string oh = (pe != std::string::npos) ? origin.substr(pe + 3) : origin;

    size_t pp = oh.find(':'), hp = host.find(':');
    std::string ohn = (pp != std::string::npos) ? oh.substr(0, pp) : oh;
    std::string hn = (hp != std::string::npos) ? host.substr(0, hp) : host;

    if(ohn == "localhost" || ohn == "127.0.0.1" || hn == "localhost" || hn == "127.0.0.1" || ohn == hn) {
        r.set_header("Access-Control-Allow-Origin", origin);
        r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        r.set_header("Access-Control-Allow-Credentials", "true");
    }
}

std::string GetLocalIPAddress() {
    std::string result = "127.0.0.1";
    char hostname[256];

    if(gethostname(hostname, sizeof(hostname)) != 0) {
        WARN("GetLocalIPAddress: gethostname failed: %d", WSAGetLastError());
        return result;
    }

    addrinfo hints = {}, *info = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(hostname, nullptr, &hints, &info);
    if(ret != 0) {
        WARN("GetLocalIPAddress: getaddrinfo failed: %d (%s)", ret, gai_strerror(ret));
        return result;
    }
    if(!info) { WARN("GetLocalIPAddress: getaddrinfo returned null"); return result; }

    for(auto* p = info; p; p = p->ai_next) {
        if(p->ai_family == AF_INET) {
            char ip[INET_ADDRSTRLEN];
            if(inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip))) {
                std::string ipStr(ip);
                if(ipStr != "127.0.0.1" && ipStr.find("127.") != 0) {
                    result = ipStr;
                    DBG("GetLocalIPAddress: Found %s", result.c_str());
                    break;
                }
            } else {
                WARN("GetLocalIPAddress: inet_ntop failed: %d", WSAGetLastError());
            }
        }
    }
    freeaddrinfo(info);
    return result;
}

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
        std::string localIP = GetLocalIPAddress();

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

        srv.Get("/api/session", AuthRequired([](auto&, auto& res, const std::string& user) {
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
        printf("  Network: https://%s:%d\n", localIP.c_str(), PORT);
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

        while(g_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(100ms);
        }

        LOG("Initiating shutdown...");
        if(audio) audio->Stop();
        if(mic) mic->Stop();
        srv.stop();
        frameSlot.Wake();

        std::atomic<bool> forceExit{false};
        std::thread timeout([&] {
            std::this_thread::sleep_for(3s);
            if(!forceExit.load(std::memory_order_acquire)) {
                WARN("Shutdown timeout, forcing exit");
                ExitProcess(0);
            }
        });
        timeout.detach();

        if(encThread.joinable()) { DBG("Waiting for encoder thread..."); encThread.join(); }
        if(audioThread.joinable()) { DBG("Waiting for audio thread..."); audioThread.join(); }
        if(cursorThread.joinable()) { DBG("Waiting for cursor thread..."); cursorThread.join(); }
        if(srvThread.joinable()) { DBG("Waiting for server thread..."); srvThread.join(); }

        forceExit.store(true, std::memory_order_release);
        WSACleanup();
        LOG("Shutdown complete");

    } catch(const std::exception& e) {
        ERR("Fatal: %s", e.what());
        WSACleanup();
        getchar();
        return 1;
    } catch(...) {
        ERR("Fatal: Unknown exception");
        WSACleanup();
        getchar();
        return 1;
    }

    return 0;
}
