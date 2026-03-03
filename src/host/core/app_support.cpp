#include "host/core/app_support.hpp"
#include "host/io/tray.hpp"
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace {
AppContext g_appContext;
}

AppContext& GetAppContext() {
    return g_appContext;
}

BOOL WINAPI ConsoleHandler(DWORD sig) {
    auto& app = GetAppContext();
    if (sig == CTRL_CLOSE_EVENT && !app.exitRequested.load(std::memory_order_acquire)) {
        HideAppToTray();
        return TRUE;
    }
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT || sig == CTRL_CLOSE_EVENT ||
        sig == CTRL_LOGOFF_EVENT || sig == CTRL_SHUTDOWN_EVENT) {
        printf("\n[Shutting down...]\n");
        app.running = false;
        return TRUE;
    }
    return FALSE;
}

namespace {

std::string GetMonitorFriendlyName(const wchar_t* gdiName) {
    UINT32 pathCnt = 0, modeCnt = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCnt, &modeCnt) != ERROR_SUCCESS || !pathCnt)
        return "";

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCnt);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCnt);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCnt, paths.data(), &modeCnt, modes.data(), nullptr) != ERROR_SUCCESS)
        return "";

    for (UINT32 i = 0; i < pathCnt; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(src),
                      paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, gdiName) != 0) continue;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(tgt),
                      paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;

        if (tgt.monitorFriendlyDeviceName[0]) {
            char name[64]{};
            WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1, name, sizeof(name), nullptr, nullptr);
            return name;
        }
    }
    return "";
}

bool LoadConfig() {
    try {
        auto authPath = GetSlipStreamDataFilePath("auth.json");
        std::ifstream f(authPath);
        if (!f) f.open("auth.json");
        if (!f) return false;

        json c = json::parse(f);
        if (!c.contains("username") || !c.contains("passwordHash") || !c.contains("salt")) return false;

        auto& config = GetAppContext().config;
        config = {c["username"], c["passwordHash"], c["salt"]};
        return config.username.size() >= 3 && config.passwordHash.size() == 64 && config.salt.size() == 32;
    } catch (const std::exception& e) {
        ERR("LoadConfig failed: %s", e.what());
        return false;
    } catch (...) {
        ERR("LoadConfig failed: unknown exception");
        return false;
    }
}

bool SaveConfig() {
    const auto& config = GetAppContext().config;
    auto authPath = GetSlipStreamDataFilePath("auth.json");
    std::ofstream f(authPath);
    if (!f) return false;
    f << json{{"username", config.username}, {"passwordHash", config.passwordHash}, {"salt", config.salt}}.dump(2);
    return f.good();
}

bool ValidateUsername(const std::string& u) {
    if (u.length() < 3 || u.length() > 32) return false;
    for (char c : u)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}

bool ValidatePassword(const std::string& p) {
    if (p.length() < 8 || p.length() > 128) return false;
    bool hasLetter = false, hasDigit = false;
    for (char c : p) {
        if (isalpha(static_cast<unsigned char>(c))) hasLetter = true;
        if (isdigit(static_cast<unsigned char>(c))) hasDigit = true;
    }
    return hasLetter && hasDigit;
}

std::string GetPasswordInput() {
    std::string pw;
    for (int ch; (ch = _getch()) != '\r' && ch != '\n';) {
        if (ch == 8 || ch == 127) {
            if (!pw.empty()) { pw.pop_back(); printf("\b \b"); }
        } else if (ch == 27) {
            while (!pw.empty()) { pw.pop_back(); printf("\b \b"); }
        } else if (ch >= 32 && ch <= 126) {
            pw += static_cast<char>(ch);
            printf("*");
        }
    }
    printf("\n");
    return pw;
}

std::string ExtractSessionCookie(const httplib::Request& req) {
    auto it = req.headers.find("Cookie");
    if (it == req.headers.end()) return "";
    size_t pos = it->second.find("session=");
    if (pos == std::string::npos) return "";
    size_t start = pos + 8, end = it->second.find(';', start);
    return end != std::string::npos ? it->second.substr(start, end - start) : it->second.substr(start);
}

std::string Trim(const std::string& s) {
    size_t start = 0, end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

bool IsPrivateIP(const std::string& ip) {
    if (ip == "127.0.0.1" || ip == "::1" || ip == "localhost") return true;

    in_addr addr{};
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) return false;
    uint32_t h = ntohl(addr.S_un.S_addr);

    return (h & 0xFF000000u) == 0x0A000000u ||   // 10.0.0.0/8
           (h & 0xFFF00000u) == 0xAC100000u ||   // 172.16.0.0/12
           (h & 0xFFFF0000u) == 0xC0A80000u ||   // 192.168.0.0/16
           (h & 0xFF000000u) == 0x7F000000u;     // 127.0.0.0/8
}

std::string GetClientIP(const httplib::Request& req) {
    std::string remote = Trim(req.remote_addr);
    if (!IsPrivateIP(remote)) return remote;

    auto it = req.headers.find("X-Forwarded-For");
    if (it != req.headers.end() && !it->second.empty()) {
        std::string fwd = it->second;
        size_t comma = fwd.find(',');
        if (comma != std::string::npos) fwd = fwd.substr(0, comma);
        fwd = Trim(fwd);
        if (!fwd.empty()) return fwd;
    }
    return remote;
}

} // namespace

void JsonError(httplib::Response& res, int status, const std::string& err) {
    res.status = status;
    res.set_content(json{{"error", err}}.dump(), "application/json");
}

std::function<void(const httplib::Request&, httplib::Response&)> AuthRequired(
    std::function<void(const httplib::Request&, httplib::Response&, const std::string&)> h) {
    return [h](const httplib::Request& req, httplib::Response& res) {
        std::string token = ExtractSessionCookie(req), user;
        if (token.empty() || !GetAppContext().jwt.ValidateToken(token, user)) {
            JsonError(res, 401, token.empty() ? "Authentication required" : "Invalid token");
            return;
        }
        h(req, res, user);
    };
}

void RefreshMonitorList() {
    std::lock_guard<std::mutex> lk(g_monitorsMutex);
    g_monitors.clear();

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* mons = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
        MONITORINFOEXW mi{sizeof(mi)};
        DEVMODEW dm{sizeof(dm)};

        if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);

        std::string friendly = GetMonitorFriendlyName(mi.szDevice);
        char name[64]{};
        if (!friendly.empty()) {
            strncpy(name, friendly.c_str(), sizeof(name) - 1);
        } else {
            WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name, sizeof(name), nullptr, nullptr);
        }

        mons->push_back({
            hMon, static_cast<int>(mons->size()),
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            dm.dmDisplayFrequency ? static_cast<int>(dm.dmDisplayFrequency) : 60,
            (mi.dwFlags & MONITORINFOF_PRIMARY) != 0,
            name
        });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&g_monitors));

    std::sort(g_monitors.begin(), g_monitors.end(), [](auto& a, auto& b) {
        return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index;
    });
    for (size_t i = 0; i < g_monitors.size(); i++) g_monitors[i].index = static_cast<int>(i);
    LOG("RefreshMonitorList: Found %zu monitors", g_monitors.size());
}

std::string LoadFile(const char* path) {
    static const std::filesystem::path baseDir = [] {
        char mp[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, mp, MAX_PATH) <= 0) return std::filesystem::path(".");
        auto exe = std::filesystem::path(mp).parent_path();
        for (auto d : {exe, exe / "client", exe.parent_path() / "client",
                       exe.parent_path().parent_path() / "client",
                       exe.parent_path().parent_path().parent_path() / "client"})
            if (std::filesystem::exists(d / "index.html")) return d;
        return exe;
    }();
    auto readText = [](const std::filesystem::path& p) -> std::string {
        std::ifstream f(p, std::ios::binary);
        return f ? std::string(std::istreambuf_iterator<char>(f), {}) : "";
    };
    if (auto r = readText(baseDir / path); !r.empty()) return r;
    return readText(path);
}

void SetupConfig() {
    auto& config = GetAppContext().config;
    if (LoadConfig()) {
        printf("Loaded config (user: %s)\n", config.username.c_str());
        return;
    }
    printf("\n=== First Time Setup ===\n");

    while (true) {
        printf("Username (3-32 chars): ");
        std::getline(std::cin, config.username);
        if (ValidateUsername(config.username)) break;
        printf("Invalid username\n");
    }

    while (true) {
        printf("Password (8+ chars, letter+number): ");
        std::string pw = GetPasswordInput();
        if (!ValidatePassword(pw)) { printf("Invalid password\n"); continue; }
        printf("Confirm password: ");
        if (pw == GetPasswordInput()) {
            config.salt = GenerateSalt();
            config.passwordHash = HashPassword(pw, config.salt);
            if (!config.passwordHash.empty() && SaveConfig()) {
                printf("Configuration saved\n\n");
                return;
            }
        }
        printf("Passwords don't match or save failed\n");
    }
}

void HandleAuth(const httplib::Request& req, httplib::Response& res) {
    auto& app = GetAppContext();
    auto& config = app.config;
    auto& rateLimiter = app.rateLimiter;

    json body;
    try {
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        WARN("Auth: Invalid JSON in request body: %s", e.what());
        JsonError(res, 400, "Invalid JSON");
        return;
    } catch (...) {
        WARN("Auth: Invalid JSON in request body");
        JsonError(res, 400, "Invalid JSON");
        return;
    }

    std::string ip = GetClientIP(req);
    if (!rateLimiter.IsAllowed(ip)) {
        res.status = 429;
        res.set_content(json{{"error", "Too many attempts"}, {"lockoutSeconds", rateLimiter.LockoutSeconds(ip)}}.dump(),
                        "application/json");
        return;
    }

    std::string u = body.value("username", ""), p = body.value("password", "");
    if (u.empty() || p.empty()) {
        rateLimiter.RecordAttempt(ip, false);
        JsonError(res, 400, "Credentials required");
        return;
    }

    if (u != config.username || !VerifyPassword(p, config.salt, config.passwordHash)) {
        rateLimiter.RecordAttempt(ip, false);
        res.status = 401;
        res.set_content(json{{"error", "Invalid credentials"},
                             {"remainingAttempts", rateLimiter.RemainingAttempts(ip)}}.dump(), "application/json");
        return;
    }

    rateLimiter.RecordAttempt(ip, true);
    std::string token = app.jwt.CreateToken(u);
    if (token.empty()) {
        JsonError(res, 500, "Internal error");
        return;
    }

    LOG("HandleAuth: Successful login for '%s' from %s", u.c_str(), ip.c_str());
    res.set_header("Set-Cookie", "session=" + token + "; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400");
    res.set_content(json{{"success", true}, {"username", u}}.dump(), "application/json");
}

void SetupCORS(const httplib::Request& req, httplib::Response& r) {
    r.set_header("X-Content-Type-Options", "nosniff");
    r.set_header("X-Frame-Options", "DENY");
    r.set_header("Referrer-Policy", "no-referrer");

    std::string origin = req.get_header_value("Origin");
    if (origin.empty()) return;

    std::string host = req.get_header_value("Host");
    auto extractHost = [](const std::string& s) {
        size_t pe = s.find("://");
        std::string h = (pe != std::string::npos) ? s.substr(pe + 3) : s;
        size_t pp = h.find(':');
        return (pp != std::string::npos) ? h.substr(0, pp) : h;
    };

    std::string oh = extractHost(origin), hh = extractHost(host);
    if (oh == "localhost" || oh == "127.0.0.1" || hh == "localhost" || hh == "127.0.0.1" || oh == hh) {
        r.set_header("Access-Control-Allow-Origin", origin);
        r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        r.set_header("Access-Control-Allow-Credentials", "true");
    }
}
