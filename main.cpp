/**
 * @file main.cpp
 * @brief SlipStream Server - Main entry point
 * @copyright 2025-2026 Daniel Chrobak
 */

#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "audio.hpp"
#include "input.hpp"

#include <conio.h>

std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;

void RefreshMonitorList() {
    std::lock_guard<std::mutex> lock(g_monitorsMutex);
    g_monitors.clear();

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* mons = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
        MONITORINFOEXW mi{sizeof(mi)};
        DEVMODEW dm{.dmSize = sizeof(dm)};
        char name[64];

        if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name, sizeof(name), nullptr, nullptr);

        mons->push_back({hMon, static_cast<int>(mons->size()),
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            dm.dmDisplayFrequency ? static_cast<int>(dm.dmDisplayFrequency) : 60,
            (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, name});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&g_monitors));

    std::sort(g_monitors.begin(), g_monitors.end(), [](const auto& a, const auto& b) {
        return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index;
    });
    for (size_t i = 0; i < g_monitors.size(); i++) g_monitors[i].index = static_cast<int>(i);
    LOG("Found %zu monitor(s)", g_monitors.size());
}

std::string LoadFile(const char* path) {
    std::ifstream f(path);
    return f.is_open() ? std::string(std::istreambuf_iterator<char>(f), {}) : "";
}

// Configuration with hashed password
struct Config {
    std::string username;
    std::string passwordHash;
    std::string salt;
} g_config;

// Global authentication manager
AuthManager g_auth;

bool LoadConfig() {
    try {
        std::ifstream f("auth.json");
        if (!f.is_open()) return false;
        json c = json::parse(f);

        if (c.contains("username") && c.contains("passwordHash") && c.contains("salt")) {
            g_config.username = c["username"];
            g_config.passwordHash = c["passwordHash"];
            g_config.salt = c["salt"];
            return g_config.username.size() >= 3 &&
                   g_config.passwordHash.size() == 64 &&
                   g_config.salt.size() == 64;
        }

        // Handle legacy format with plaintext pin - migrate to new format
        if (c.contains("username") && c.contains("pin")) {
            std::string oldUsername = c["username"];
            std::string oldPin = c["pin"];

            g_config.username = oldUsername;
            g_config.salt = GenerateSalt();
            g_config.passwordHash = HashPassword(oldPin, g_config.salt);

            std::ofstream out("auth.json");
            out << json{
                {"username", g_config.username},
                {"passwordHash", g_config.passwordHash},
                {"salt", g_config.salt}
            }.dump(2);

            LOG("Migrated auth.json from plaintext PIN to hashed password");
            return true;
        }
    } catch (const std::exception& e) {
        ERR("Failed to load config: %s", e.what());
    }
    return false;
}

bool SaveConfig() {
    try {
        std::ofstream out("auth.json");
        out << json{
            {"username", g_config.username},
            {"passwordHash", g_config.passwordHash},
            {"salt", g_config.salt}
        }.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

bool ValidateUsername(const std::string& u) {
    if (u.length() < 3 || u.length() > 32) return false;
    for (char c : u) if (!isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

bool ValidatePassword(const std::string& p) {
    if (p.length() < 8 || p.length() > 128) return false;
    bool hasLetter = false, hasDigit = false;
    for (char c : p) {
        if (isalpha(c)) hasLetter = true;
        if (isdigit(c)) hasDigit = true;
    }
    return hasLetter && hasDigit;
}

std::string GetPasswordInput() {
    std::string password;
    int ch;

    while (true) {
        ch = _getch();

        if (ch == '\r' || ch == '\n') {
            printf("\n");
            break;
        }
        else if (ch == 8 || ch == 127) {
            if (!password.empty()) {
                password.pop_back();
                printf("\b \b");
            }
        }
        else if (ch == 27) {
            while (!password.empty()) {
                password.pop_back();
                printf("\b \b");
            }
        }
        else if (ch >= 32 && ch <= 126) {
            password += static_cast<char>(ch);
            printf("*");
        }
    }

    return password;
}

void SetupConfig() {
    if (LoadConfig()) {
        printf("\033[32mLoaded config (user: %s)\033[0m\n\n", g_config.username.c_str());
        return;
    }

    printf("\n\033[1;36m=== First Time Setup ===\033[0m\n\n\033[1mAuthentication\033[0m\n");

    while (true) {
        printf("  Username (3-32 chars): ");
        std::getline(std::cin, g_config.username);
        if (ValidateUsername(g_config.username)) break;
        printf("  \033[31mInvalid username (use letters, numbers, _ or -)\033[0m\n");
    }

    std::string password, confirm;
    while (true) {
        printf("  Password (8+ chars, must include letter and number): ");
        password = GetPasswordInput();
        if (!ValidatePassword(password)) {
            printf("  \033[31mPassword must be 8+ chars with at least one letter and one number\033[0m\n");
            continue;
        }
        printf("  Confirm password: ");
        confirm = GetPasswordInput();
        if (password == confirm) break;
        printf("  \033[31mPasswords don't match\033[0m\n");
    }

    g_config.salt = GenerateSalt();
    g_config.passwordHash = HashPassword(password, g_config.salt);

    if (SaveConfig()) {
        printf("\n\033[32mConfiguration saved to auth.json (password is hashed)\033[0m\n\n");
    } else {
        printf("\n\033[31mFailed to save configuration\033[0m\n");
        SetupConfig();
    }
}

// ============================================================================
// HTTP Helpers
// ============================================================================

std::string ExtractBearerToken(const httplib::Request& req) {
    auto it = req.headers.find("Authorization");
    if (it != req.headers.end() && it->second.substr(0, 7) == "Bearer ")
        return it->second.substr(7);
    return "";
}

std::string GetClientIP(const httplib::Request& req) {
    auto it = req.headers.find("X-Forwarded-For");
    if (it != req.headers.end() && !it->second.empty()) {
        size_t comma = it->second.find(',');
        return comma != std::string::npos ? it->second.substr(0, comma) : it->second;
    }
    return req.remote_addr;
}

void JsonError(httplib::Response& res, int status, const std::string& error) {
    res.status = status;
    res.set_content(json{{"error", error}}.dump(), "application/json");
}

// Middleware: Require valid session token
template<typename F>
auto AuthRequired(F handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        std::string token = ExtractBearerToken(req);
        if (token.empty() || !g_auth.ValidateSession(token)) {
            JsonError(res, 401, token.empty() ? "Authentication required" : "Session expired or invalid");
            return;
        }
        handler(req, res, token);
    };
}

// Middleware: Parse JSON body with error handling
template<typename F>
auto JsonHandler(F handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            handler(req, res, body);
        } catch (...) {
            JsonError(res, 400, "Invalid JSON");
        }
    };
}

int main() {
    try {
        SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD m = 0;
            if (GetConsoleMode(h, &m)) SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }

        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hIn != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hIn, &mode)) {
                mode &= ~ENABLE_QUICK_EDIT_MODE;
                mode |= ENABLE_EXTENDED_FLAGS;
                SetConsoleMode(hIn, mode);
            }
        }

        puts("\n\033[1;36m=== SlipStream Server ===\033[0m\n");
        SetupConfig();

        constexpr int PORT = 6060;
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        FrameSlot frameSlot;
        auto rtcServer = std::make_shared<WebRTCServer>();

        ScreenCapture capture(&frameSlot);
        std::unique_ptr<AV1Encoder> encoder;
        std::mutex encoderMutex;
        std::atomic<bool> encoderReady{false}, running{true};

        InputHandler inputHandler;
        inputHandler.Enable();

        auto updateInputBounds = [&](int idx) {
            std::lock_guard<std::mutex> lock(g_monitorsMutex);
            if (idx >= 0 && idx < static_cast<int>(g_monitors.size()))
                inputHandler.UpdateFromMonitorInfo(g_monitors[idx]);
        };

        updateInputBounds(capture.GetCurrentMonitorIndex());

        std::unique_ptr<AudioCapture> audioCapture;
        try { audioCapture = std::make_unique<AudioCapture>(); } catch (...) {}

        auto createEncoder = [&](int w, int h, int fps) {
            std::lock_guard<std::mutex> lock(encoderMutex);
            encoderReady = false;
            encoder.reset();
            try {
                encoder = std::make_unique<AV1Encoder>(w, h, fps, capture.GetDev(), capture.GetCtx(), capture.GetMT());
                encoderReady = true;
                LOG("Encoder: %dx%d @ %d FPS", w, h, fps);
            } catch (const std::exception& e) {
                ERR("Encoder: %s", e.what());
            }
        };

        createEncoder(capture.GetW(), capture.GetH(), capture.GetHostFPS());
        capture.SetResolutionChangeCallback([&](int w, int h, int fps) { createEncoder(w, h, fps); });

        // Initialize WebRTC with all callbacks at once
        rtcServer->Init({
            .inputHandler = &inputHandler,
            .onFpsChange = [&](int fps, uint8_t) {
                capture.SetFPS(fps);
                if (!capture.IsCapturing()) capture.StartCapture();
            },
            .getHostFps = [&] { return capture.RefreshHostFPS(); },
            .onMonitorChange = [&](int idx) {
                bool ok = capture.SwitchMonitor(idx);
                if (ok) {
                    updateInputBounds(idx);
                    std::thread([&] { std::this_thread::sleep_for(100ms); inputHandler.WiggleCenter(); }).detach();
                }
                return ok;
            },
            .getCurrentMonitor = [&] { return capture.GetCurrentMonitorIndex(); },
            .onDisconnect = [&] { capture.PauseCapture(); },
            .onConnected = [&] {
                std::thread([&] { std::this_thread::sleep_for(100ms); inputHandler.WiggleCenter(); }).detach();
            }
        });

        httplib::Server httpServer;

        // CORS and caching headers
        httpServer.set_post_routing_handler([](auto&, auto& r) {
            r.set_header("Access-Control-Allow-Origin", "*");
            r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            r.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            r.set_header("Cache-Control", "no-cache");
        });

        httpServer.Options(".*", [](auto&, auto& r) { r.status = 204; });

        // Static file routes
        httpServer.Get("/", [](auto&, auto& r) {
            auto c = LoadFile("index.html");
            r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html");
        });
        httpServer.Get("/styles.css", [](auto&, auto& r) {
            r.set_content(LoadFile("styles.css"), "text/css");
        });

        for (auto js : {"input", "media", "network", "renderer", "state", "ui"})
            httpServer.Get(std::string("/js/") + js + ".js", [js](auto&, auto& r) {
                r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()), "application/javascript");
            });

        // Authentication endpoint
        httpServer.Post("/api/auth", JsonHandler([&](const httplib::Request& req, httplib::Response& res, const json& body) {
            std::string clientIP = GetClientIP(req);

            if (!g_auth.IsAllowed(clientIP)) {
                res.status = 429;
                res.set_content(json{{"error", "Too many failed attempts"}, {"lockoutSeconds", g_auth.LockoutSecondsRemaining(clientIP)}}.dump(), "application/json");
                WARN("Rate limited auth attempt from %s", clientIP.c_str());
                return;
            }

            std::string username = body.value("username", "");
            std::string password = body.value("password", "");

            if (username.empty() || password.empty()) {
                g_auth.RecordAttempt(clientIP, false);
                JsonError(res, 400, "Username and password required");
                return;
            }

            bool valid = (username == g_config.username) && VerifyPassword(password, g_config.salt, g_config.passwordHash);

            if (!valid) {
                g_auth.RecordAttempt(clientIP, false);
                res.status = 401;
                res.set_content(json{{"error", "Invalid credentials"}, {"remainingAttempts", g_auth.RemainingAttempts(clientIP)}}.dump(), "application/json");
                WARN("Failed auth attempt from %s (user: %s)", clientIP.c_str(), username.c_str());
                return;
            }

            g_auth.RecordAttempt(clientIP, true);
            std::string token = g_auth.CreateSession(username, clientIP);
            res.set_content(json{{"token", token}, {"expiresIn", 24 * 60 * 60}}.dump(), "application/json");
            LOG("User '%s' authenticated from %s", username.c_str(), clientIP.c_str());
        }));

        // Logout endpoint
        httpServer.Post("/api/logout", [&](const httplib::Request& req, httplib::Response& res) {
            std::string token = ExtractBearerToken(req);
            if (!token.empty()) g_auth.InvalidateSession(token);
            res.set_content(json{{"success", true}}.dump(), "application/json");
        });

        // Session validation endpoint
        httpServer.Get("/api/session", AuthRequired([&](const httplib::Request&, httplib::Response& res, const std::string& token) {
            res.set_content(json{{"valid", true}, {"username", g_auth.GetUsername(token)}}.dump(), "application/json");
        }));

        // WebRTC offer endpoint
        httpServer.Post("/api/offer", AuthRequired([&](const httplib::Request& req, httplib::Response& res, const std::string& token) {
            try {
                auto body = json::parse(req.body);
                std::string offer = body["sdp"].get<std::string>();
                std::string username = g_auth.GetUsername(token);

                LOG("Received offer from authenticated user '%s' (%s)", username.c_str(), GetClientIP(req).c_str());

                rtcServer->SetRemote(offer, "offer");
                std::string answer = rtcServer->GetLocal();

                if (answer.empty()) {
                    JsonError(res, 500, "Failed to generate answer");
                    return;
                }

                if (size_t p = answer.find("a=setup:actpass"); p != std::string::npos)
                    answer.replace(p, 15, "a=setup:active");

                res.set_content(json{{"sdp", answer}, {"type", "answer"}}.dump(), "application/json");
                LOG("Sent answer to user '%s'", username.c_str());

            } catch (const std::exception& e) {
                ERR("Offer error: %s", e.what());
                JsonError(res, 400, "Invalid offer");
            }
        }));

        std::thread serverThread([&] { httpServer.listen("0.0.0.0", PORT); });
        std::this_thread::sleep_for(100ms);

        std::thread cleanupThread([&] {
            while (running) {
                std::this_thread::sleep_for(std::chrono::minutes(5));
                g_auth.Cleanup();
            }
        });

        printf("\n\033[1;36m==========================================\033[0m\n");
        printf("\033[1;36m            SLIPSTREAM SERVER             \033[0m\n");
        printf("\033[1;36m==========================================\033[0m\n\n");
        printf("  \033[1mLocal:\033[0m  http://localhost:%d\n\n", PORT);
        printf("  User: %s | Display: %dHz\n", g_config.username.c_str(), capture.GetHostFPS());
        printf("  Auth: HTTP-based with hashed passwords\n");
        printf("\033[1;36m==========================================\033[0m\n\n");

        if (audioCapture) audioCapture->Start();

        std::thread audioThread([&] {
            if (!audioCapture) return;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AudioPacket pkt;
            while (running) {
                if (!rtcServer->IsStreaming()) { std::this_thread::sleep_for(10ms); continue; }
                if (audioCapture->PopPacket(pkt, 5))
                    rtcServer->SendAudio(pkt.data, pkt.ts, pkt.samples);
            }
        });

        std::thread statsThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            uint64_t hist[10] = {};
            int idx = 0;
            while (running) {
                std::this_thread::sleep_for(1s);
                auto stats = rtcServer->GetStats();
                uint64_t enc = 0;
                {
                    std::lock_guard<std::mutex> lock(encoderMutex);
                    if (encoder) enc = encoder->GetEncoded();
                }
                hist[idx++ % 10] = enc;
                int cnt = std::min(idx, 10);
                uint64_t sum = 0;
                for (int i = 0; i < cnt; i++) sum += hist[i];

                const char* st = stats.connected
                    ? (rtcServer->IsStreaming() ? "\033[32m[LIVE]\033[0m" : "\033[33m[WAIT]\033[0m")
                    : "\033[33m[WAIT]\033[0m";

                printf("%s FPS: %3llu @ %d | %5.2f Mbps | V:%4llu A:%3llu | Avg: %.1f\n",
                       st, enc, capture.GetCurrentFPS(),
                       stats.bytes * 8.0 / 1048576.0,
                       stats.sent, rtcServer->GetAudioSent(),
                       cnt > 0 ? static_cast<double>(sum) / cnt : 0.0);
            }
        });

        std::thread encodeThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            FrameData fd;
            bool was = false;
            while (running) {
                if (!rtcServer->IsStreaming() || !encoderReady) {
                    std::this_thread::sleep_for(10ms);
                    was = false;
                    continue;
                }
                if (!frameSlot.Pop(fd)) continue;

                bool streaming = rtcServer->IsStreaming() && encoderReady;
                if (streaming && !was) {
                    LOG("Streaming at %d FPS", rtcServer->GetCurrentFps());
                    std::lock_guard<std::mutex> lock(encoderMutex);
                    if (encoder) encoder->Flush();
                }
                was = streaming;

                if (!streaming || !fd.tex) {
                    frameSlot.MarkReleased(fd.poolIdx);
                    fd.Release();
                    continue;
                }

                // Wait for GPU sync if needed (handles both fence and query modes)
                if (fd.needsSync && !capture.IsReady(fd.fence) && !capture.WaitReady(fd.fence)) {
                    frameSlot.MarkReleased(fd.poolIdx);
                    fd.Release();
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(encoderMutex);
                    if (encoder)
                        if (auto* out = encoder->Encode(fd.tex, fd.ts, rtcServer->NeedsKey()))
                            rtcServer->Send(*out);
                }
                frameSlot.MarkReleased(fd.poolIdx);
                fd.Release();
            }
        });

        serverThread.join();
        running = false;
        SetEvent(frameSlot.GetEvent());
        encodeThread.join();
        audioThread.join();
        statsThread.join();
        cleanupThread.join();
        if (audioCapture) audioCapture->Stop();
        LOG("Shutdown complete");

    } catch (const std::exception& e) {
        ERR("Fatal: %s", e.what());
        getchar();
        return 1;
    }
    return 0;
}
