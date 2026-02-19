#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>
#include <rtc/rtc.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <cstdlib>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

constexpr const char* SLIPSTREAM_VERSION = "1.0.0";

using json = nlohmann::json;
using namespace std::chrono;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;

#define LOG(f,...) printf("[LOG] " f "\n",##__VA_ARGS__)
#define ERR(f,...) fprintf(stderr,"[ERR] " f "\n",##__VA_ARGS__)
#define WARN(f,...) fprintf(stderr,"[WARN] " f "\n",##__VA_ARGS__)
#define DBG(f,...) do { if(g_debugLogging) printf("[DBG] " f "\n",##__VA_ARGS__); } while(0)

inline bool g_debugLogging = false;

inline const std::string& GetSlipStreamDataDir() {
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

inline std::string GetSlipStreamDataFilePath(const char* fileName) {
    return (std::filesystem::path(GetSlipStreamDataDir()) / fileName).string();
}

enum MsgType : uint32_t {
    MSG_PING=0x504E4750, MSG_FPS_SET=0x46505343, MSG_HOST_INFO=0x484F5354, MSG_FPS_ACK=0x46505341,
    MSG_REQUEST_KEY=0x4B455952, MSG_MONITOR_LIST=0x4D4F4E4C, MSG_MONITOR_SET=0x4D4F4E53,
    MSG_AUDIO_DATA=0x41554449, MSG_MOUSE_MOVE=0x4D4F5645, MSG_MOUSE_BTN=0x4D42544E,
    MSG_MOUSE_WHEEL=0x4D57484C, MSG_KEY=0x4B455920, MSG_CODEC_SET=0x434F4443, MSG_CODEC_ACK=0x434F4441,
    MSG_CODEC_CAPS=0x434F4350, MSG_MOUSE_MOVE_REL=0x4D4F5652, MSG_CLIPBOARD_DATA=0x434C4950,
    MSG_CLIPBOARD_GET=0x434C4754, MSG_KICKED=0x4B49434B, MSG_CURSOR_CAPTURE=0x43555243,
    MSG_CURSOR_SHAPE=0x43555253, MSG_AUDIO_ENABLE=0x41554445, MSG_MIC_DATA=0x4D494344,
    MSG_MIC_ENABLE=0x4D494345, MSG_VERSION=0x56455253
};

enum CodecType : uint8_t { CODEC_AV1=0, CODEC_H265=1, CODEC_H264=2 };

enum CursorType : uint8_t {
    CURSOR_DEFAULT=0, CURSOR_TEXT, CURSOR_POINTER, CURSOR_WAIT, CURSOR_PROGRESS, CURSOR_CROSSHAIR,
    CURSOR_MOVE, CURSOR_EW_RESIZE, CURSOR_NS_RESIZE, CURSOR_NWSE_RESIZE, CURSOR_NESW_RESIZE,
    CURSOR_NOT_ALLOWED, CURSOR_HELP, CURSOR_NONE, CURSOR_CUSTOM=255
};

#pragma pack(push,1)
struct MicPacketHeader { uint32_t magic; int64_t timestamp; uint16_t samples, dataLength; };
#pragma pack(pop)

inline int64_t GetTimestamp() {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u={{ft.dwLowDateTime,ft.dwHighDateTime}};
    return (int64_t)((u.QuadPart-116444736000000000ULL)/10);
}

template<typename...T> void SafeRelease(T*&...p) { ((p?(p->Release(),p=nullptr):nullptr),...); }
template<typename Q,typename M> void ClearQueue(Q& q,M& mtx) { std::lock_guard<M> lk(mtx); while(!q.empty()) q.pop(); }

template<typename T>
class LinearResampler {
    int srcRate, dstRate, channels;
    double ratio, accum=0.0;
    std::vector<T> prev;
public:
    std::vector<T> buf;
    LinearResampler(int src,int dst,int ch)
        : srcRate(src), dstRate(dst), channels(ch), ratio((double)src/dst), prev(ch,T{}) {
        buf.reserve(480*ch*8);
    }
    void Reset() { accum=0.0; std::fill(prev.begin(),prev.end(),T{}); buf.clear(); }
    void Process(const T* in, size_t frames) {
        if(!frames) return;
        if(srcRate==dstRate) {
            buf.insert(buf.end(), in, in+frames*channels);
            for(int c=0; c<channels; c++) prev[c]=in[(frames-1)*channels+c];
            return;
        }
        while(accum < frames) {
            size_t i0=(size_t)accum, i1=i0+1;
            double f=accum-i0;
            for(int c=0; c<channels; c++) {
                T s0 = (i0==0 && accum<1.0) ? prev[c] : in[i0*channels+c];
                T s1 = (i1<frames) ? in[i1*channels+c] : s0;
                buf.push_back((T)(s0+(s1-s0)*f));
            }
            accum += ratio;
        }
        accum -= frames;
        for(int c=0; c<channels; c++) prev[c]=in[(frames-1)*channels+c];
    }
    void ProcessMono(const T* in, size_t frames, int outCh) {
        if(!frames) return;
        double srcRatio = (double)dstRate/srcRate;
        while(accum < frames) {
            size_t i0=(size_t)accum, i1=i0+1;
            double f=accum-i0;
            T s0 = (i0==0 && accum<1.0) ? prev[0] : in[i0];
            T s1 = (i1<frames) ? in[i1] : s0;
            T s = (T)(s0+(s1-s0)*f);
            for(int c=0; c<outCh; c++) buf.push_back(s);
            accum += 1.0/srcRatio;
        }
        accum -= frames;
        prev[0] = in[frames-1];
    }
};

struct MTLock {
    ID3D11Multithread* m;
    MTLock(ID3D11Multithread* mt) : m(mt) { if(m) m->Enter(); }
    ~MTLock() { if(m) m->Leave(); }
    MTLock(const MTLock&)=delete;
    MTLock& operator=(const MTLock&)=delete;
};

struct MonitorInfo {
    HMONITOR hMon; int index, width, height, refreshRate; bool isPrimary; std::string name;
};
extern std::vector<MonitorInfo> g_monitors;
extern std::mutex g_monitorsMutex;
void RefreshMonitorList();

constexpr int PBKDF2_ITER=600000, PBKDF2_KLEN=32, SALT_LEN=16;

inline std::string BytesToHex(const unsigned char* d, size_t n) {
    std::ostringstream o;
    for(size_t i=0; i<n; i++) o << std::hex << std::setw(2) << std::setfill('0') << (int)d[i];
    return o.str();
}

inline std::string GenerateSalt(size_t n=SALT_LEN) {
    std::vector<unsigned char> s(n);
    if(RAND_bytes(s.data(),(int)n) != 1) {
        ERR("RAND_bytes failed for salt generation");
        for(size_t i=0; i<n; i++) s[i] = (unsigned char)(rand()%256);
    }
    return BytesToHex(s.data(), s.size());
}

inline std::string HashPassword(const std::string& pw, const std::string& salt) {
    std::vector<unsigned char> sb(salt.size()/2);
    for(size_t i=0; i<sb.size(); i++) {
        unsigned int b; std::sscanf(salt.c_str()+i*2, "%02x", &b);
        sb[i] = (unsigned char)b;
    }
    unsigned char h[PBKDF2_KLEN];
    if(PKCS5_PBKDF2_HMAC(pw.c_str(),(int)pw.size(),sb.data(),(int)sb.size(),
                          PBKDF2_ITER,EVP_sha256(),PBKDF2_KLEN,h) != 1) {
        ERR("PKCS5_PBKDF2_HMAC failed");
        return "";
    }
    return BytesToHex(h, PBKDF2_KLEN);
}

inline bool VerifyPassword(const std::string& pw, const std::string& salt, const std::string& stored) {
    if(salt.empty() || stored.empty()) {
        WARN("VerifyPassword called with empty salt or stored hash");
        return false;
    }
    std::string c = HashPassword(pw, salt);
    if(c.empty()) { ERR("HashPassword failed during verification"); return false; }
    if(c.size() != stored.size()) return false;
    volatile int r=0;
    for(size_t i=0; i<c.size(); i++) r |= c[i]^stored[i];
    return r==0;
}

class JWTAuth {
    std::string sec;
    void LoadOrGen() {
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
            if(sec.size()==64) { DBG("Loaded existing JWT secret"); return; }
            WARN("JWT secret file exists but has invalid size (%zu), regenerating", sec.size());
        }
        unsigned char b[32];
        if(RAND_bytes(b, sizeof(b)) != 1) {
            ERR("RAND_bytes failed for JWT secret generation");
            throw std::runtime_error("Failed to generate secure JWT secret");
        }
        sec = BytesToHex(b, sizeof(b));
        std::ofstream out(secretPath);
        if(!out) { ERR("Failed to save JWT secret to file: %s", secretPath.c_str()); }
        else { out << sec; LOG("Generated new JWT secret"); }
    }
public:
    JWTAuth() { LoadOrGen(); }
    std::string CreateToken(const std::string& u) {
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
    bool ValidateToken(const std::string& t, std::string& u) {
        if(t.empty()) { DBG("ValidateToken called with empty token"); return false; }
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
};

class RateLimiter {
    struct RL { int att=0; steady_clock::time_point first, lockout; };
    std::unordered_map<std::string, RL> lim;
    std::mutex mtx;
    static constexpr int MAX=5;
public:
    bool IsAllowed(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = lim.find(ip);
        if(it == lim.end()) return true;
        auto now = steady_clock::now();
        if(it->second.lockout > steady_clock::time_point{} && now < it->second.lockout) {
            DBG("Rate limit: IP %s is locked out", ip.c_str());
            return false;
        }
        if(now - it->second.first > minutes(15)) { lim.erase(it); return true; }
        return it->second.att < MAX;
    }
    void RecordAttempt(const std::string& ip, bool ok) {
        std::lock_guard<std::mutex> lk(mtx);
        if(ok) { lim.erase(ip); DBG("Rate limit: Cleared attempts for IP %s (successful auth)", ip.c_str()); return; }
        auto now = steady_clock::now();
        auto& r = lim[ip];
        if(r.att==0 || now-r.first > minutes(15)) r = {1, now, {}};
        else if(++r.att >= MAX) {
            r.lockout = now + minutes(30);
            WARN("Rate limit: IP %s locked out for 30 minutes after %d failed attempts", ip.c_str(), MAX);
        }
    }
    int RemainingAttempts(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = lim.find(ip);
        return it==lim.end() ? MAX : std::max(0, MAX - it->second.att);
    }
    int LockoutSeconds(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = lim.find(ip);
        if(it==lim.end() || it->second.lockout <= steady_clock::now()) return 0;
        return (int)duration_cast<seconds>(it->second.lockout - steady_clock::now()).count();
    }
};

inline std::string GetSSLCertFilePath() { return GetSlipStreamDataFilePath("server.crt"); }
inline std::string GetSSLKeyFilePath() { return GetSlipStreamDataFilePath("server.key"); }

inline bool SSLCertExists() {
    auto certPath = GetSSLCertFilePath();
    auto keyPath = GetSSLKeyFilePath();
    std::ifstream c(certPath), k(keyPath);
    return c.good() && k.good();
}

inline bool GenerateSSLCert(int days=3650, int bits=2048) {
    LOG("Generating self-signed SSL certificate...");
    EVP_PKEY* pkey=nullptr; X509* x509=nullptr; EVP_PKEY_CTX* ctx=nullptr; bool ok=false;

    ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if(!ctx) { ERR("EVP_PKEY_CTX_new_id failed"); goto end; }
    if(EVP_PKEY_keygen_init(ctx) <= 0) { ERR("EVP_PKEY_keygen_init failed"); goto end; }
    if(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0) {
        ERR("EVP_PKEY_CTX_set_rsa_keygen_bits failed"); goto end;
    }
    if(EVP_PKEY_keygen(ctx, &pkey) <= 0) { ERR("EVP_PKEY_keygen failed"); goto end; }

    x509 = X509_new();
    if(!x509) { ERR("X509_new failed"); goto end; }
    if(X509_set_version(x509, 2) != 1) { ERR("X509_set_version failed"); goto end; }

    {
        unsigned char sb[16];
        if(RAND_bytes(sb, sizeof(sb)) != 1) { ERR("RAND_bytes failed for cert serial"); goto end; }
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
    X509_gmtime_adj(X509_getm_notAfter(x509), days*24*3600);
    if(X509_set_pubkey(x509, pkey) != 1) { ERR("X509_set_pubkey failed"); goto end; }

    {
        X509_NAME* n = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(n, "C", MBSTRING_ASC, (unsigned char*)"US", -1, -1, 0);
        X509_NAME_add_entry_by_txt(n, "O", MBSTRING_ASC, (unsigned char*)"SlipStream", -1, -1, 0);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (unsigned char*)"localhost", -1, -1, 0);
        if(X509_set_issuer_name(x509, n) != 1) { ERR("X509_set_issuer_name failed"); goto end; }
    }

    {
        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, x509, x509, nullptr, nullptr, 0);
        auto add = [&](int nid, const char* val) {
            if(auto e = X509V3_EXT_conf_nid(nullptr, &v3, nid, val)) {
                X509_add_ext(x509, e, -1);
                X509_EXTENSION_free(e);
            } else { WARN("Failed to add X509 extension NID %d", nid); }
        };
        add(NID_basic_constraints, "CA:FALSE");
        add(NID_key_usage, "digitalSignature,keyEncipherment");
        add(NID_ext_key_usage, "serverAuth");
        add(NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1,IP:0.0.0.0");
    }

    if(!X509_sign(x509, pkey, EVP_sha256())) { ERR("X509_sign failed"); goto end; }

    {
        auto keyPath = GetSSLKeyFilePath();
        BIO* kb = BIO_new_file(keyPath.c_str(), "wb");
        if(!kb) { ERR("Failed to create key file: %s", keyPath.c_str()); goto end; }
        if(!PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr)) {
            ERR("PEM_write_bio_PrivateKey failed");
            BIO_free(kb); goto end;
        }
        BIO_free(kb);
    }

    {
        auto certPath = GetSSLCertFilePath();
        BIO* cb = BIO_new_file(certPath.c_str(), "wb");
        if(!cb) { ERR("Failed to create cert file: %s", certPath.c_str()); goto end; }
        if(!PEM_write_bio_X509(cb, x509)) {
            ERR("PEM_write_bio_X509 failed");
            BIO_free(cb); goto end;
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

inline bool EnsureSSLCert() {
    return SSLCertExists() ? (LOG("Using existing SSL certificates"), true) : GenerateSSLCert();
}
