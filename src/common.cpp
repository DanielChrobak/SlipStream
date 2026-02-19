#include "common.hpp"

#include <unordered_set>
#include <ws2tcpip.h>

namespace {
std::vector<std::string> GetCertificateSANIPs() {
    std::vector<std::string> ips;
    std::unordered_set<std::string> seen;
    char hostname[256]{};

    if(gethostname(hostname, sizeof(hostname)) != 0) {
        WARN("GetCertificateSANIPs: gethostname failed: %d", WSAGetLastError());
        return ips;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* info = nullptr;
    int ret = getaddrinfo(hostname, nullptr, &hints, &info);
    if(ret != 0) {
        WARN("GetCertificateSANIPs: getaddrinfo failed: %d (%s)", ret, gai_strerror(ret));
        return ips;
    }

    for(auto* p = info; p; p = p->ai_next) {
        if(p->ai_family != AF_INET) continue;
        char ip[INET_ADDRSTRLEN]{};
        if(!inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, ip, sizeof(ip))) continue;

        std::string ipStr(ip);
        bool isLoopback = (ipStr == "127.0.0.1" || ipStr.rfind("127.", 0) == 0);
        bool isUnspecified = (ipStr == "0.0.0.0");
        bool isLinkLocal = (ipStr.rfind("169.254.", 0) == 0);
        if(!isLoopback && !isUnspecified && !isLinkLocal && !seen.count(ipStr)) {
            seen.insert(ipStr);
            ips.push_back(ipStr);
        }
    }

    freeaddrinfo(info);
    return ips;
}

std::string BuildCertificateSAN() {
    std::string san = "DNS:localhost,IP:127.0.0.1";

    char hostname[256]{};
    if(gethostname(hostname, sizeof(hostname)) == 0 && hostname[0] != '\0' && _stricmp(hostname, "localhost") != 0) {
        san += ",DNS:";
        san += hostname;
    }

    for(const auto& ip : GetCertificateSANIPs()) {
        san += ",IP:";
        san += ip;
    }
    return san;
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
        if(ec) {
            WARN("Failed to create data directory '%s': %s", p.string().c_str(), ec.message().c_str());
            return std::filesystem::path(".").string();
        }
        return p.string();
    }();
    return dir;
}

std::string GetSlipStreamDataFilePath(const char* fileName) {
    return (std::filesystem::path(GetSlipStreamDataDir()) / fileName).string();
}

std::string BytesToHex(const unsigned char* d, size_t n) {
    std::ostringstream o;
    for(size_t i = 0; i < n; i++) o << std::hex << std::setw(2) << std::setfill('0') << (int)d[i];
    return o.str();
}

std::string GenerateSalt(size_t n) {
    std::vector<unsigned char> s(n);
    if(RAND_bytes(s.data(), (int)n) != 1) {
        ERR("RAND_bytes failed for salt generation");
        for(size_t i = 0; i < n; i++) s[i] = (unsigned char)(rand() % 256);
    }
    return BytesToHex(s.data(), s.size());
}

std::string HashPassword(const std::string& pw, const std::string& salt) {
    std::vector<unsigned char> sb(salt.size() / 2);
    for(size_t i = 0; i < sb.size(); i++) {
        unsigned int b;
        std::sscanf(salt.c_str() + i * 2, "%02x", &b);
        sb[i] = (unsigned char)b;
    }
    unsigned char h[PBKDF2_KLEN];
    if(PKCS5_PBKDF2_HMAC(pw.c_str(), (int)pw.size(), sb.data(), (int)sb.size(),
                         PBKDF2_ITER, EVP_sha256(), PBKDF2_KLEN, h) != 1) {
        ERR("PKCS5_PBKDF2_HMAC failed");
        return "";
    }
    return BytesToHex(h, PBKDF2_KLEN);
}

bool VerifyPassword(const std::string& pw, const std::string& salt, const std::string& stored) {
    if(salt.empty() || stored.empty()) {
        WARN("VerifyPassword called with empty salt or stored hash");
        return false;
    }
    std::string c = HashPassword(pw, salt);
    if(c.empty()) {
        ERR("HashPassword failed during verification");
        return false;
    }
    if(c.size() != stored.size()) return false;
    volatile int r = 0;
    for(size_t i = 0; i < c.size(); i++) r |= c[i] ^ stored[i];
    return r == 0;
}

void JWTAuth::LoadOrGen() {
    auto secretPath = GetSlipStreamDataFilePath("jwt_secret.dat");
    std::ifstream f(secretPath);
    if(!f) {
        f.open("jwt_secret.dat");
        if(f) {
            LOG("Using legacy JWT secret file from working directory");
        }
    }
    if(f) {
        std::getline(f, sec);
        if(sec.size() == 64) {
            DBG("Loaded existing JWT secret");
            return;
        }
        WARN("JWT secret file exists but has invalid size (%zu), regenerating", sec.size());
    }
    unsigned char b[32];
    if(RAND_bytes(b, sizeof(b)) != 1) {
        ERR("RAND_bytes failed for JWT secret generation");
        throw std::runtime_error("Failed to generate secure JWT secret");
    }
    sec = BytesToHex(b, sizeof(b));
    std::ofstream out(secretPath);
    if(!out) {
        ERR("Failed to save JWT secret to file: %s", secretPath.c_str());
    } else {
        out << sec;
        LOG("Generated new JWT secret");
    }
}

JWTAuth::JWTAuth() {
    LoadOrGen();
}

std::string JWTAuth::CreateToken(const std::string& u) {
    try {
        return jwt::create()
            .set_issuer("slipstream")
            .set_subject(u)
            .set_issued_at(system_clock::now())
            .set_expires_at(system_clock::now() + hours(24))
            .sign(jwt::algorithm::hs256{sec});
    } catch(const std::exception& e) {
        ERR("Failed to create JWT token: %s", e.what());
        return "";
    }
}

bool JWTAuth::ValidateToken(const std::string& t, std::string& u) {
    if(t.empty()) {
        DBG("ValidateToken called with empty token");
        return false;
    }
    try {
        auto d = jwt::decode(t);
        jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{sec})
            .with_issuer("slipstream")
            .verify(d);
        u = d.get_subject();
        return true;
    } catch(const jwt::error::token_verification_exception& e) {
        DBG("Token verification failed: %s", e.what());
        return false;
    } catch(const std::exception& e) {
        WARN("Token validation error: %s", e.what());
        return false;
    }
}

bool RateLimiter::IsAllowed(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = lim.find(ip);
    if(it == lim.end()) return true;
    auto now = steady_clock::now();
    if(it->second.lockout > steady_clock::time_point{} && now < it->second.lockout) {
        DBG("Rate limit: IP %s is locked out", ip.c_str());
        return false;
    }
    if(now - it->second.first > minutes(15)) {
        lim.erase(it);
        return true;
    }
    return it->second.att < MAX;
}

void RateLimiter::RecordAttempt(const std::string& ip, bool ok) {
    std::lock_guard<std::mutex> lk(mtx);
    if(ok) {
        lim.erase(ip);
        DBG("Rate limit: Cleared attempts for IP %s (successful auth)", ip.c_str());
        return;
    }
    auto now = steady_clock::now();
    auto& r = lim[ip];
    if(r.att == 0 || now - r.first > minutes(15)) {
        r = {1, now, {}};
    } else if(++r.att >= MAX) {
        r.lockout = now + minutes(30);
        WARN("Rate limit: IP %s locked out for 30 minutes after %d failed attempts", ip.c_str(), MAX);
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
    if(it == lim.end() || it->second.lockout <= steady_clock::now()) return 0;
    return (int)duration_cast<seconds>(it->second.lockout - steady_clock::now()).count();
}

std::string GetSSLCertFilePath() {
    return GetSlipStreamDataFilePath("server.crt");
}

std::string GetSSLKeyFilePath() {
    return GetSlipStreamDataFilePath("server.key");
}

bool SSLCertExists() {
    auto certPath = GetSSLCertFilePath();
    auto keyPath = GetSSLKeyFilePath();
    std::ifstream c(certPath), k(keyPath);
    return c.good() && k.good();
}

bool GenerateSSLCert(int days, int bits) {
    LOG("Generating self-signed SSL certificate...");
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    EVP_PKEY_CTX* ctx = nullptr;
    bool ok = false;

    ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if(!ctx) {
        ERR("EVP_PKEY_CTX_new_id failed");
        goto end;
    }
    if(EVP_PKEY_keygen_init(ctx) <= 0) {
        ERR("EVP_PKEY_keygen_init failed");
        goto end;
    }
    if(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0) {
        ERR("EVP_PKEY_CTX_set_rsa_keygen_bits failed");
        goto end;
    }
    if(EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        ERR("EVP_PKEY_keygen failed");
        goto end;
    }

    x509 = X509_new();
    if(!x509) {
        ERR("X509_new failed");
        goto end;
    }
    if(X509_set_version(x509, 2) != 1) {
        ERR("X509_set_version failed");
        goto end;
    }

    {
        unsigned char sb[16];
        if(RAND_bytes(sb, sizeof(sb)) != 1) {
            ERR("RAND_bytes failed for cert serial");
            goto end;
        }
        BIGNUM* bn = BN_bin2bn(sb, sizeof(sb), nullptr);
        if(bn) {
            ASN1_INTEGER* s = ASN1_INTEGER_new();
            BN_to_ASN1_INTEGER(bn, s);
            X509_set_serialNumber(x509, s);
            ASN1_INTEGER_free(s);
            BN_free(bn);
        }
    }

    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), days * 24 * 3600);
    if(X509_set_pubkey(x509, pkey) != 1) {
        ERR("X509_set_pubkey failed");
        goto end;
    }

    {
        X509_NAME* n = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(n, "C", MBSTRING_ASC, (unsigned char*)"US", -1, -1, 0);
        X509_NAME_add_entry_by_txt(n, "O", MBSTRING_ASC, (unsigned char*)"SlipStream", -1, -1, 0);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (unsigned char*)"localhost", -1, -1, 0);
        if(X509_set_issuer_name(x509, n) != 1) {
            ERR("X509_set_issuer_name failed");
            goto end;
        }
    }

    {
        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, x509, x509, nullptr, nullptr, 0);
        auto add = [&](int nid, const char* val) {
            if(auto e = X509V3_EXT_conf_nid(nullptr, &v3, nid, val)) {
                X509_add_ext(x509, e, -1);
                X509_EXTENSION_free(e);
            } else {
                WARN("Failed to add X509 extension NID %d", nid);
            }
        };
        add(NID_basic_constraints, "CA:FALSE");
        add(NID_key_usage, "digitalSignature,keyEncipherment");
        add(NID_ext_key_usage, "serverAuth");
        std::string san = BuildCertificateSAN();
        add(NID_subject_alt_name, san.c_str());
    }

    if(!X509_sign(x509, pkey, EVP_sha256())) {
        ERR("X509_sign failed");
        goto end;
    }

    {
        auto keyPath = GetSSLKeyFilePath();
        BIO* kb = BIO_new_file(keyPath.c_str(), "wb");
        if(!kb) {
            ERR("Failed to create key file: %s", keyPath.c_str());
            goto end;
        }
        if(!PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr)) {
            ERR("PEM_write_bio_PrivateKey failed");
            BIO_free(kb);
            goto end;
        }
        BIO_free(kb);
    }

    {
        auto certPath = GetSSLCertFilePath();
        BIO* cb = BIO_new_file(certPath.c_str(), "wb");
        if(!cb) {
            ERR("Failed to create cert file: %s", certPath.c_str());
            goto end;
        }
        if(!PEM_write_bio_X509(cb, x509)) {
            ERR("PEM_write_bio_X509 failed");
            BIO_free(cb);
            goto end;
        }
        BIO_free(cb);
    }

    {
        auto certPath = GetSSLCertFilePath();
        auto keyPath = GetSSLKeyFilePath();
        LOG("SSL certificate generated: %s, %s", certPath.c_str(), keyPath.c_str());
    }
    ok = true;

end:
    if(x509) X509_free(x509);
    if(pkey) EVP_PKEY_free(pkey);
    if(ctx) EVP_PKEY_CTX_free(ctx);
    return ok;
}

bool EnsureSSLCert() {
    return SSLCertExists() ? (LOG("Using existing SSL certificates"), true) : GenerateSSLCert();
}
