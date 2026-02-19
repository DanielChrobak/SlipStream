#include "app_support.hpp"
#include "tray.hpp"
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

std::atomic<bool> g_running{true};
std::atomic<bool> g_exitRequested{false};
std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;
Config g_config;
JWTAuth g_jwt;
RateLimiter g_rateLimiter;

BOOL WINAPI ConsoleHandler(DWORD sig) {
    if(sig==CTRL_CLOSE_EVENT) {
        if(!g_exitRequested.load(std::memory_order_acquire)) {
            HideAppToTray();
            return TRUE;
        }
        printf("\n[Shutting down...]\n");
        g_running = false;
        return TRUE;
    }
    if(sig==CTRL_C_EVENT || sig==CTRL_BREAK_EVENT || sig==CTRL_LOGOFF_EVENT || sig==CTRL_SHUTDOWN_EVENT) {
        printf("\n[Shutting down...]\n");
        g_running = false;
        return TRUE;
    }
    DBG("ConsoleHandler: Received signal %lu", sig);
    return FALSE;
}

namespace {
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
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCnt, paths.data(), &modeCnt, modes.data(), nullptr);
    if(result != ERROR_SUCCESS) {
        DBG("GetMonitorFriendlyName: QueryDisplayConfig failed: %ld", result);
        return "";
    }

    for(UINT32 i=0; i<pathCnt; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(src), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
        LONG srcResult = DisplayConfigGetDeviceInfo(&src.header);
        if(srcResult != ERROR_SUCCESS) {
            DBG("GetMonitorFriendlyName: DisplayConfigGetDeviceInfo (source) failed: %ld", srcResult);
            continue;
        }
        if(wcscmp(src.viewGdiDeviceName, gdiName) == 0) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
            tgt.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(tgt), paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
            LONG tgtResult = DisplayConfigGetDeviceInfo(&tgt.header);
            if(tgtResult != ERROR_SUCCESS) {
                DBG("GetMonitorFriendlyName: DisplayConfigGetDeviceInfo (target) failed: %ld", tgtResult);
                continue;
            }
            if(tgt.monitorFriendlyDeviceName[0]) {
                char name[64] = {};
                int converted = WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1, name, sizeof(name), nullptr, nullptr);
                if(converted == 0) { WARN("GetMonitorFriendlyName: WideCharToMultiByte failed: %lu", GetLastError()); }
                return name;
            }
        }
    }
    return "";
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
            bool valid = g_config.username.size() >= 3 && g_config.passwordHash.size() == 64 && g_config.salt.size() == 32;
            if(!valid) {
                WARN("LoadConfig: Config validation failed (username=%zu, hash=%zu, salt=%zu)", g_config.username.size(), g_config.passwordHash.size(), g_config.salt.size());
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
        f << json{{"username", g_config.username}, {"passwordHash", g_config.passwordHash}, {"salt", g_config.salt}}.dump(2);
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

std::string ExtractSessionCookie(const httplib::Request& req) {
    auto it = req.headers.find("Cookie");
    if(it == req.headers.end()) return "";
    size_t pos = it->second.find("session=");
    if(pos == std::string::npos) return "";
    size_t start = pos + 8, end = it->second.find(';', start);
    return end != std::string::npos ? it->second.substr(start, end - start) : it->second.substr(start);
}

std::string Trim(const std::string& value) {
    size_t start = 0;
    while(start < value.size() && std::isspace((unsigned char)value[start])) start++;

    size_t end = value.size();
    while(end > start && std::isspace((unsigned char)value[end - 1])) end--;

    return value.substr(start, end - start);
}

bool IsIPv4InCidr(const std::string& ip, uint32_t network, uint32_t mask) {
    in_addr addr{};
    if(inet_pton(AF_INET, ip.c_str(), &addr) != 1) return false;
    uint32_t hostOrder = ntohl(addr.S_un.S_addr);
    return (hostOrder & mask) == network;
}

bool IsTrustedProxyAddress(const std::string& ip) {
    if(ip == "127.0.0.1" || ip == "::1" || ip == "localhost") return true;

    return IsIPv4InCidr(ip, 0x0A000000u, 0xFF000000u) ||      // 10.0.0.0/8
           IsIPv4InCidr(ip, 0xAC100000u, 0xFFF00000u) ||      // 172.16.0.0/12
           IsIPv4InCidr(ip, 0xC0A80000u, 0xFFFF0000u) ||      // 192.168.0.0/16
           IsIPv4InCidr(ip, 0x7F000000u, 0xFF000000u);        // 127.0.0.0/8
}

std::string GetClientIP(const httplib::Request& req) {
    std::string remote = Trim(req.remote_addr);
    if(!IsTrustedProxyAddress(remote)) return remote;

    auto it = req.headers.find("X-Forwarded-For");
    if(it != req.headers.end() && !it->second.empty()) {
        std::string forwarded = it->second;
        size_t comma = forwarded.find(',');
        if(comma != std::string::npos) forwarded = forwarded.substr(0, comma);
        forwarded = Trim(forwarded);
        if(!forwarded.empty()) return forwarded;
    }

    return remote;
}

}

void JsonError(httplib::Response& res, int status, const std::string& err) {
    res.status = status;
    res.set_content(json{{"error", err}}.dump(), "application/json");
    DBG("JsonError: %d - %s", status, err.c_str());
}

std::function<void(const httplib::Request&, httplib::Response&)> AuthRequired(
    std::function<void(const httplib::Request&, httplib::Response&, const std::string&)> h) {
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

void RefreshMonitorList() {
    std::lock_guard<std::mutex> lk(g_monitorsMutex);
    g_monitors.clear();

    BOOL enumResult = EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* mons = (std::vector<MonitorInfo>*)lp;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);

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
            int converted = WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name, sizeof(name), nullptr, nullptr);
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
        DBG("RefreshMonitorList: Found monitor %zu: %s (%dx%d @ %dHz, primary=%d)", mons->size()-1, name, width, height, refreshRate, isPrimary);
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

    if(SaveConfig()) printf("Configuration saved\n\n");
    else { printf("Failed to save\n"); SetupConfig(); }
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
        res.set_content(json{{"error", "Too many attempts"}, {"lockoutSeconds", lockout}}.dump(), "application/json");
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
        WARN("HandleAuth: Failed login attempt for '%s' from %s (%d attempts remaining)", u.c_str(), ip.c_str(), remaining);
        res.status = 401;
        res.set_content(json{{"error", "Invalid credentials"}, {"remainingAttempts", remaining}}.dump(), "application/json");
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
    res.set_header("Set-Cookie", "session=" + token + "; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400");
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
    auto ips = GetLocalIPAddresses();
    return ips.empty() ? "127.0.0.1" : ips.front();
}

std::vector<std::string> GetLocalIPAddresses() {
    std::vector<std::string> results;
    std::unordered_set<std::string> seen;
    char hostname[256];

    if(gethostname(hostname, sizeof(hostname)) != 0) {
        WARN("GetLocalIPAddresses: gethostname failed: %d", WSAGetLastError());
        return results;
    }

    addrinfo hints = {}, *info = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(hostname, nullptr, &hints, &info);
    if(ret != 0) {
        WARN("GetLocalIPAddresses: getaddrinfo failed: %d (%s)", ret, gai_strerror(ret));
        return results;
    }
    if(!info) {
        WARN("GetLocalIPAddresses: getaddrinfo returned null");
        return results;
    }

    for(auto* p = info; p; p = p->ai_next) {
        if(p->ai_family == AF_INET) {
            char ip[INET_ADDRSTRLEN];
            if(inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip))) {
                std::string ipStr(ip);
                bool isLoopback = (ipStr == "127.0.0.1" || ipStr.rfind("127.", 0) == 0);
                bool isUnspecified = (ipStr == "0.0.0.0");
                bool isLinkLocal = (ipStr.rfind("169.254.", 0) == 0);
                if(!isLoopback && !isUnspecified && !isLinkLocal && !seen.count(ipStr)) {
                    seen.insert(ipStr);
                    results.push_back(ipStr);
                    DBG("GetLocalIPAddresses: Found %s", ipStr.c_str());
                }
            } else {
                WARN("GetLocalIPAddresses: inet_ntop failed: %d", WSAGetLastError());
            }
        }
    }
    freeaddrinfo(info);
    return results;
}
