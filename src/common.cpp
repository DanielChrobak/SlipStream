#include "common.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>

std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;

namespace {
    std::mutex g_logMutex;
    FILE* g_logFile = nullptr;
    std::atomic<bool> g_loggingInit{false};
}

void ShutdownLogging() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_loggingInit = false;
}

void InitLogging() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_loggingInit) return;
    g_loggingInit = true;

    const char* appData = std::getenv("APPDATA");
    std::filesystem::path dir = appData && *appData
        ? std::filesystem::path(appData) / "SlipStream"
        : std::filesystem::path(".") / "SlipStream";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string logPath = (dir / "slipstream.log").string();

    g_logFile = fopen(logPath.c_str(), "a");
    if (g_logFile) {
        setvbuf(g_logFile, nullptr, _IONBF, 0);
        std::atexit(ShutdownLogging);
    }
}

void LogPrint(const char* level, bool toStderr, const char* fmt, ...) {
    char message[4096];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    FILE* out = toStderr ? stderr : stdout;
    fprintf(out, "[%s] %s\n", level, message);
    fflush(out);

    if (!g_loggingInit) InitLogging();

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level, message);
        fflush(g_logFile);
    }
}

const std::string& GetSlipStreamDataDir() {
    static const std::string dir = [] {
        const char* appData = std::getenv("APPDATA");
        std::filesystem::path p = appData && *appData
            ? std::filesystem::path(appData) / "SlipStream"
            : std::filesystem::path(".") / "SlipStream";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return ec ? std::filesystem::path(".").string() : p.string();
    }();
    return dir;
}

std::string GetSlipStreamDataFilePath(const char* fileName) {
    return (std::filesystem::path(GetSlipStreamDataDir()) / fileName).string();
}

std::string BytesToHex(const unsigned char* d, size_t n) {
    std::ostringstream o;
    for (size_t i = 0; i < n; i++)
        o << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(d[i]);
    return o.str();
}

std::string GenerateSalt(size_t n) {
    std::vector<unsigned char> s(n);
    if (RAND_bytes(s.data(), static_cast<int>(n)) != 1) {
        ERR("RAND_bytes failed for salt generation");
        for (size_t i = 0; i < n; i++) s[i] = static_cast<unsigned char>(rand() % 256);
    }
    return BytesToHex(s.data(), s.size());
}

std::string HashPassword(const std::string& pw, const std::string& salt) {
    std::vector<unsigned char> sb(salt.size() / 2);
    for (size_t i = 0; i < sb.size(); i++) {
        unsigned int b;
        std::sscanf(salt.c_str() + i * 2, "%02x", &b);
        sb[i] = static_cast<unsigned char>(b);
    }
    unsigned char h[32];
    if (PKCS5_PBKDF2_HMAC(pw.c_str(), static_cast<int>(pw.size()), sb.data(), static_cast<int>(sb.size()),
                          600000, EVP_sha256(), 32, h) != 1) {
        ERR("PKCS5_PBKDF2_HMAC failed");
        return "";
    }
    return BytesToHex(h, 32);
}

bool VerifyPassword(const std::string& pw, const std::string& salt, const std::string& stored) {
    if (salt.empty() || stored.empty()) {
        WARN("VerifyPassword called with empty salt or stored hash");
        return false;
    }
    std::string c = HashPassword(pw, salt);
    if (c.empty() || c.size() != stored.size()) return false;
    volatile int r = 0;
    for (size_t i = 0; i < c.size(); i++) r |= c[i] ^ stored[i];
    return r == 0;
}

void JWTAuth::LoadOrGen() {
    auto secretPath = GetSlipStreamDataFilePath("jwt_secret.dat");
    std::ifstream f(secretPath);
    if (!f) f.open("jwt_secret.dat");
    if (f) {
        std::getline(f, sec);
        if (sec.size() == 64) return;
        WARN("JWT secret file invalid size (%zu), regenerating", sec.size());
    }
    unsigned char b[32];
    if (RAND_bytes(b, sizeof(b)) != 1) throw std::runtime_error("Failed to generate JWT secret");
    sec = BytesToHex(b, sizeof(b));
    std::ofstream out(secretPath);
    if (out) out << sec;
    LOG("Generated new JWT secret");
}

JWTAuth::JWTAuth() { LoadOrGen(); }

std::string JWTAuth::CreateToken(const std::string& u) {
    try {
        return jwt::create()
            .set_issuer("slipstream")
            .set_subject(u)
            .set_issued_at(system_clock::now())
            .set_expires_at(system_clock::now() + hours(24))
            .sign(jwt::algorithm::hs256{sec});
    } catch (const std::exception& e) {
        ERR("Failed to create JWT token: %s", e.what());
        return "";
    }
}

bool JWTAuth::ValidateToken(const std::string& t, std::string& u) {
    if (t.empty()) return false;
    try {
        auto d = jwt::decode(t);
        jwt::verify().allow_algorithm(jwt::algorithm::hs256{sec}).with_issuer("slipstream").verify(d);
        u = d.get_subject();
        return true;
    } catch (const std::exception& e) {
        DBG("Token validation error: %s", e.what());
        return false;
    }
}

bool RateLimiter::IsAllowed(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = lim.find(ip);
    if (it == lim.end()) return true;
    auto now = steady_clock::now();
    if (it->second.lockout > steady_clock::time_point{} && now < it->second.lockout) return false;
    if (now - it->second.first > minutes(15)) { lim.erase(it); return true; }
    return it->second.att < MAX;
}

void RateLimiter::RecordAttempt(const std::string& ip, bool ok) {
    std::lock_guard<std::mutex> lk(mtx);
    if (ok) { lim.erase(ip); return; }
    auto now = steady_clock::now();
    auto& r = lim[ip];
    if (r.att == 0 || now - r.first > minutes(15)) {
        r = {1, now, {}};
    } else if (++r.att >= MAX) {
        r.lockout = now + minutes(30);
        WARN("Rate limit: IP %s locked out for 30 minutes", ip.c_str());
    }
}

int RateLimiter::RemainingAttempts(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = lim.find(ip);
    return it == lim.end() ? MAX : std::max(0, MAX - it->second.att);
}

int RateLimiter::LockoutSeconds(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = lim.find(ip);
    if (it == lim.end() || it->second.lockout <= steady_clock::now()) return 0;
    return static_cast<int>(duration_cast<seconds>(it->second.lockout - steady_clock::now()).count());
}

std::string GetSSLCertFilePath() { return GetSlipStreamDataFilePath("server.crt"); }
std::string GetSSLKeyFilePath() { return GetSlipStreamDataFilePath("server.key"); }

namespace {
std::vector<std::string> GetLocalIPs() {
    std::vector<std::string> ips;
    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname)) != 0) return ips;

    addrinfo hints{}, *info = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &info) != 0 || !info) return ips;

    std::unordered_set<std::string> seen;
    for (auto* p = info; p; p = p->ai_next) {
        if (p->ai_family != AF_INET) continue;
        char ip[INET_ADDRSTRLEN]{};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
        if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) continue;
        std::string ipStr(ip);
        if (ipStr.rfind("127.", 0) == 0 || ipStr == "0.0.0.0" || ipStr.rfind("169.254.", 0) == 0) continue;
        if (!seen.count(ipStr)) { seen.insert(ipStr); ips.push_back(ipStr); }
    }
    freeaddrinfo(info);
    return ips;
}

std::string BuildCertSAN() {
    std::string san = "DNS:localhost,IP:127.0.0.1";
    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname)) == 0 && hostname[0] && _stricmp(hostname, "localhost") != 0)
        san += std::string(",DNS:") + hostname;
    for (const auto& ip : GetLocalIPs()) san += ",IP:" + ip;
    return san;
}
}

bool EnsureSSLCert() {
    auto certPath = GetSSLCertFilePath();
    auto keyPath = GetSSLKeyFilePath();

    std::ifstream c(certPath), k(keyPath);
    if (c.good() && k.good()) {
        LOG("Using existing SSL certificates");
        return true;
    }

    LOG("Generating self-signed SSL certificate...");

    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    bool ok = false;

    auto cleanup = [&] { if (x509) X509_free(x509); if (pkey) EVP_PKEY_free(pkey); if (ctx) EVP_PKEY_CTX_free(ctx); };

    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0 ||
        EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        ERR("Key generation failed");
        cleanup();
        return false;
    }

    x509 = X509_new();
    if (!x509 || X509_set_version(x509, 2) != 1) { cleanup(); return false; }

    unsigned char sb[16];
    if (RAND_bytes(sb, sizeof(sb)) == 1) {
        if (BIGNUM* bn = BN_bin2bn(sb, sizeof(sb), nullptr)) {
            ASN1_INTEGER* s = ASN1_INTEGER_new();
            BN_to_ASN1_INTEGER(bn, s);
            X509_set_serialNumber(x509, s);
            ASN1_INTEGER_free(s);
            BN_free(bn);
        }
    }

    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    constexpr int CERT_VALIDITY_SECONDS = 3650 * 24 * 3600;
    X509_gmtime_adj(X509_getm_notAfter(x509), CERT_VALIDITY_SECONDS);
    X509_set_pubkey(x509, pkey);

    X509_NAME* n = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(n, "C", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(n, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("SlipStream"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x509, n);

    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x509, x509, nullptr, nullptr, 0);

    auto addExt = [&](int nid, const char* val) {
        if (auto e = X509V3_EXT_conf_nid(nullptr, &v3, nid, val)) {
            X509_add_ext(x509, e, -1);
            X509_EXTENSION_free(e);
        }
    };
    addExt(NID_basic_constraints, "CA:FALSE");
    addExt(NID_key_usage, "digitalSignature,keyEncipherment");
    addExt(NID_ext_key_usage, "serverAuth");
    addExt(NID_subject_alt_name, BuildCertSAN().c_str());

    if (!X509_sign(x509, pkey, EVP_sha256())) { cleanup(); return false; }

    BIO* kb = BIO_new_file(keyPath.c_str(), "wb");
    BIO* cb = BIO_new_file(certPath.c_str(), "wb");
    if (kb && cb) {
        ok = PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr) &&
             PEM_write_bio_X509(cb, x509);
    }
    if (kb) BIO_free(kb);
    if (cb) BIO_free(cb);
    cleanup();

    if (ok) LOG("SSL certificate generated: %s, %s", certPath.c_str(), keyPath.c_str());
    return ok;
}
