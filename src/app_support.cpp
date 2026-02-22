#include "app_support.hpp"
#include "tray.hpp"
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

std::atomic<bool> g_running{true};
std::atomic<bool> g_exitRequested{false};
Config g_config;
JWTAuth g_jwt;
RateLimiter g_rateLimiter;

BOOL WINAPI ConsoleHandler(DWORD sig) {
    if (sig == CTRL_CLOSE_EVENT && !g_exitRequested.load(std::memory_order_acquire)) {
        HideAppToTray();
        return TRUE;
    }
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT || sig == CTRL_CLOSE_EVENT ||
        sig == CTRL_LOGOFF_EVENT || sig == CTRL_SHUTDOWN_EVENT) {
        printf("\n[Shutting down...]\n");
        g_running = false;
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

        g_config = {c["username"], c["passwordHash"], c["salt"]};
        return g_config.username.size() >= 3 && g_config.passwordHash.size() == 64 && g_config.salt.size() == 32;
    } catch (...) {
        return false;
    }
}

bool SaveConfig() {
    auto authPath = GetSlipStreamDataFilePath("auth.json");
    std::ofstream f(authPath);
    if (!f) return false;
    f << json{{"username", g_config.username}, {"passwordHash", g_config.passwordHash}, {"salt", g_config.salt}}.dump(2);
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
        if (token.empty() || !g_jwt.ValidateToken(token, user)) {
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
    auto readText = [](const std::filesystem::path& p) -> std::string {
        std::ifstream f(p, std::ios::binary);
        return f ? std::string(std::istreambuf_iterator<char>(f), {}) : "";
    };

    std::filesystem::path requested(path);

    if (auto direct = readText(requested); !direct.empty()) return direct;

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();
        std::vector<std::filesystem::path> candidates = {
            exeDir / requested,
            exeDir / "client" / requested,
            exeDir.parent_path() / requested,
            exeDir.parent_path() / "client" / requested,
            exeDir.parent_path().parent_path() / requested,
            exeDir.parent_path().parent_path() / "client" / requested,
            exeDir.parent_path().parent_path().parent_path() / "client" / requested
        };

        for (const auto& candidate : candidates) {
            if (auto content = readText(candidate); !content.empty()) return content;
        }
    }

    return "";
}

void SetupConfig() {
    if (LoadConfig()) {
        printf("Loaded config (user: %s)\n", g_config.username.c_str());
        return;
    }
    printf("\n=== First Time Setup ===\n");

    while (true) {
        printf("Username (3-32 chars): ");
        std::getline(std::cin, g_config.username);
        if (ValidateUsername(g_config.username)) break;
        printf("Invalid username\n");
    }

    while (true) {
        printf("Password (8+ chars, letter+number): ");
        std::string pw = GetPasswordInput();
        if (!ValidatePassword(pw)) { printf("Invalid password\n"); continue; }
        printf("Confirm password: ");
        if (pw == GetPasswordInput()) {
            g_config.salt = GenerateSalt();
            g_config.passwordHash = HashPassword(pw, g_config.salt);
            if (!g_config.passwordHash.empty() && SaveConfig()) {
                printf("Configuration saved\n\n");
                return;
            }
        }
        printf("Passwords don't match or save failed\n");
    }
}

void HandleAuth(const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        JsonError(res, 400, "Invalid JSON");
        return;
    }

    std::string ip = GetClientIP(req);
    if (!g_rateLimiter.IsAllowed(ip)) {
        res.status = 429;
        res.set_content(json{{"error", "Too many attempts"}, {"lockoutSeconds", g_rateLimiter.LockoutSeconds(ip)}}.dump(),
                        "application/json");
        return;
    }

    std::string u = body.value("username", ""), p = body.value("password", "");
    if (u.empty() || p.empty()) {
        g_rateLimiter.RecordAttempt(ip, false);
        JsonError(res, 400, "Credentials required");
        return;
    }

    if (u != g_config.username || !VerifyPassword(p, g_config.salt, g_config.passwordHash)) {
        g_rateLimiter.RecordAttempt(ip, false);
        res.status = 401;
        res.set_content(json{{"error", "Invalid credentials"},
                             {"remainingAttempts", g_rateLimiter.RemainingAttempts(ip)}}.dump(), "application/json");
        return;
    }

    g_rateLimiter.RecordAttempt(ip, true);
    std::string token = g_jwt.CreateToken(u);
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

std::vector<std::string> GetLocalIPAddresses() {
    std::vector<std::string> results;
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) return results;

    addrinfo hints{}, *info = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &info) != 0 || !info) return results;

    std::unordered_set<std::string> seen;
    for (auto* p = info; p; p = p->ai_next) {
        if (p->ai_family != AF_INET) continue;
        char ip[INET_ADDRSTRLEN];
        const auto* addr = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
        if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) continue;
        std::string ipStr(ip);
        if (ipStr.rfind("127.", 0) == 0 || ipStr == "0.0.0.0" || ipStr.rfind("169.254.", 0) == 0) continue;
        if (!seen.count(ipStr)) {
            seen.insert(ipStr);
            results.push_back(ipStr);
        }
    }
    freeaddrinfo(info);
    return results;
}
