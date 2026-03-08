#include "host/net/port_mapper.hpp"

#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

namespace {

constexpr uint16_t kPortMapProtocolPort = 5351;
constexpr int kSocketTimeoutMs = 1500;
constexpr uint32_t kDefaultLeaseSeconds = 3600;
constexpr uint32_t kMinLeaseSeconds = 120;
constexpr uint32_t kMaxLeaseSeconds = 86400;

struct SocketHandle {
    SOCKET socket = INVALID_SOCKET;
    ~SocketHandle() { if (socket != INVALID_SOCKET) closesocket(socket); }
};

struct UpnpSession {
    UPNPUrls urls{};
    IGDdatas data{};
    std::string lanAddress;
    std::string wanAddress;
    bool valid = false;

    ~UpnpSession() {
        if (valid) {
            FreeUPNPUrls(&urls);
        }
    }
};

std::string Ipv4ToString(uint32_t addressNetworkOrder) {
    IN_ADDR address{};
    address.S_un.S_addr = addressNetworkOrder;
    char buffer[INET_ADDRSTRLEN] = {};
    return InetNtopA(AF_INET, &address, buffer, sizeof(buffer)) ? buffer : std::string{};
}

bool ParseIpv4(const std::string& text, IN_ADDR& address) { return InetPtonA(AF_INET, text.c_str(), &address) == 1; }

void EncodeIpv4Mapped(const IN_ADDR& address, uint8_t out[16]) {
    std::fill(out, out + 16, 0);
    out[10] = 0xff;
    out[11] = 0xff;
    std::memcpy(out + 12, &address.S_un.S_addr, 4);
}

std::string DecodeIpv4Mapped(const uint8_t* addressBytes) {
    static constexpr uint8_t kPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(addressBytes, kPrefix, sizeof(kPrefix)) != 0) {
        return {};
    }
    return Ipv4ToString(*reinterpret_cast<const uint32_t*>(addressBytes + 12));
}

bool ConnectUdpSocket(const std::string& addressText, uint16_t port, SocketHandle& socketHandle, std::string& error) {
    socketHandle.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle.socket == INVALID_SOCKET) return error = "failed to create UDP socket", false;
    const DWORD timeout = kSocketTimeoutMs;
    setsockopt(socketHandle.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socketHandle.socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (InetPtonA(AF_INET, addressText.c_str(), &address.sin_addr) != 1) return error = "invalid IPv4 address", false;
    return connect(socketHandle.socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 || (error = "failed to connect UDP socket to gateway", false);
}

bool GetSocketLocalIpv4(SOCKET socketValue, std::string& localAddress) {
    sockaddr_in address{};
    int addressLength = sizeof(address);
    char buffer[INET_ADDRSTRLEN] = {};
    return getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0 &&
           InetNtopA(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) &&
           ((localAddress = buffer), true);
}

bool SendAndReceive(SOCKET socketValue, const uint8_t* requestData, int requestSize, std::vector<uint8_t>& responseData) {
    if (send(socketValue, reinterpret_cast<const char*>(requestData), requestSize, 0) != requestSize) return false;
    std::array<uint8_t, 1100> buffer{};
    const int received = recv(socketValue, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
    if (received <= 0) return false;
    responseData.assign(buffer.begin(), buffer.begin() + received);
    return true;
}

bool InitMappedIpv4Socket(const std::string& gatewayAddress, const std::string& localAddress,
                          SocketHandle& socketHandle, IN_ADDR& localIpv4, std::string& error) {
    return ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, error) &&
           (ParseIpv4(localAddress, localIpv4) || (error = "failed to parse local IPv4 address", false));
}

bool DiscoverUpnp(UpnpSession& session, std::string& error) {
    int discoverError = 0;
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &discoverError);
    if (!devices) return error = discoverError != 0 ? std::string("discovery failed: ") + std::to_string(discoverError) : "discovery returned no IGD", false;
    char lanAddress[64] = {};
    char wanAddress[64] = {};
    const int igdResult = UPNP_GetValidIGD(devices, &session.urls, &session.data, lanAddress, sizeof(lanAddress), wanAddress, sizeof(wanAddress));
    freeUPNPDevlist(devices);
    if (igdResult <= 0) return error = "no valid IGD discovered", false;
    session.valid = true;
    session.lanAddress = lanAddress;
    session.wanAddress = wanAddress;
    return true;
}

void WriteBe16(uint8_t* out, uint16_t value) { value = htons(value); std::memcpy(out, &value, sizeof(value)); }
void WriteBe32(uint8_t* out, uint32_t value) { value = htonl(value); std::memcpy(out, &value, sizeof(value)); }
uint16_t ReadBe16(const uint8_t* in) { uint16_t value = 0; std::memcpy(&value, in, sizeof(value)); return ntohs(value); }
uint32_t ReadBe32(const uint8_t* in) { uint32_t value = 0; std::memcpy(&value, in, sizeof(value)); return ntohl(value); }
uint32_t ReadRaw32(const uint8_t* in) { uint32_t value = 0; std::memcpy(&value, in, sizeof(value)); return value; }

uint8_t TransportProtocolNumber(const char* transport) { return std::strcmp(transport, "TCP") == 0 ? 6 : 17; }
bool IsTcpTransport(const char* transport) { return std::strcmp(transport, "TCP") == 0; }
std::string FormatUpnpPort(uint16_t port) { return std::to_string(static_cast<unsigned int>(port)); }

std::array<uint8_t, 12> RandomNonce() {
    std::array<uint8_t, 12> nonce{};
    std::random_device device;
    for (auto& byte : nonce) {
        byte = static_cast<uint8_t>(device());
    }
    return nonce;
}

uint32_t EffectiveLease(uint32_t assignedLeaseSeconds, uint32_t requestedLeaseSeconds) { return assignedLeaseSeconds > 0 ? assignedLeaseSeconds : requestedLeaseSeconds; }
void UpdateMinLease(uint32_t& minLease, uint32_t assignedLeaseSeconds, uint32_t requestedLeaseSeconds) { minLease = std::min(minLease, EffectiveLease(assignedLeaseSeconds, requestedLeaseSeconds)); }

} // namespace

PortMapper::PortMapper(uint16_t httpsPort, uint16_t icePortBegin, uint16_t icePortEnd, bool iceTcpEnabled)
    : enabled_(GetEnvBool("SLIPSTREAM_ENABLE_PORT_MAPPING", true)),
      requestedLeaseSeconds_(GetEnvUInt("SLIPSTREAM_PORTMAP_LEASE_SECONDS", kDefaultLeaseSeconds, kMinLeaseSeconds, kMaxLeaseSeconds)),
      httpsPort_(httpsPort),
      icePortBegin_(icePortBegin),
      icePortEnd_(icePortEnd),
      iceTcpEnabled_(iceTcpEnabled),
      rules_([&]() {
          std::vector<MappingRule> rules{{httpsPort, "TCP", "SlipStream HTTPS", true, false}};
          const auto addRange = [&](const char* transport, const char* description) {
              for (uint16_t port = icePortBegin; port <= icePortEnd; ++port) {
                  rules.push_back({port, transport, description, false, true});
              }
          };
          addRange("UDP", "SlipStream WebRTC UDP");
          if (iceTcpEnabled) {
              addRange("TCP", "SlipStream WebRTC TCP");
          }
          return rules;
      }()) {
    status_.enabled = enabled_;
    status_.httpsPort = httpsPort_;
    status_.icePortBegin = icePortBegin_;
    status_.icePortEnd = icePortEnd_;
    status_.iceTcpEnabled = iceTcpEnabled_;
    status_.requestedLeaseSeconds = requestedLeaseSeconds_;
    for (const auto& rule : rules_) {
        auto& counter = IsTcpTransport(rule.transport) ? status_.requestedTcpPorts : status_.requestedUdpPorts;
        ++counter;
    }
}

PortMapper::~PortMapper() { Stop(); }

void PortMapper::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    stopRequested_ = false;
    started_ = true;
    worker_ = std::thread([this] { Run(); });
}

void PortMapper::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stopRequested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

PortMappingStatus PortMapper::GetStatus() const { std::lock_guard<std::mutex> lock(mutex_); return status_; }

void PortMapper::RecordRuleFailure(MethodOutcome& outcome, const MappingRule& rule, std::string error) {
    auto& counter = IsTcpTransport(rule.transport) ? outcome.failedTcpPorts : outcome.failedUdpPorts;
    ++counter;
    if (outcome.error.empty()) {
        outcome.error = std::move(error);
    }
}

void PortMapper::RecordRuleSuccess(MethodOutcome& outcome, const MappingRule& rule, uint16_t externalPort, uint32_t lifetimeSeconds,
                                   const std::array<uint8_t, 12>* nonce) {
    MappingLease lease;
    lease.rule = rule;
    lease.externalPort = externalPort;
    lease.lifetimeSeconds = lifetimeSeconds;
    if (nonce) {
        lease.nonce = *nonce;
    }
    outcome.leases.push_back(std::move(lease));

    if (!IsTcpTransport(rule.transport)) {
        ++outcome.mappedUdpPorts;
        return;
    }
    ++outcome.mappedTcpPorts;
    outcome.signalTcpMapped = outcome.signalTcpMapped || rule.signaling;
    outcome.webrtcTcpMapped = outcome.webrtcTcpMapped || rule.webrtc;
}

bool PortMapper::IsOutcomeActive(const MethodOutcome& outcome) { return outcome.signalTcpMapped || outcome.mappedUdpPorts > 0 || outcome.webrtcTcpMapped; }

std::array<PortMapper::Method, 3> PortMapper::GetMethodOrder(Method activeMethod) {
    if (activeMethod == Method::None) {
        return {Method::Upnp, Method::Pcp, Method::NatPmp};
    }
    return activeMethod == Method::Upnp ? std::array<Method, 3>{Method::Upnp, Method::Pcp, Method::NatPmp}
         : activeMethod == Method::Pcp  ? std::array<Method, 3>{Method::Pcp, Method::Upnp, Method::NatPmp}
                                        : std::array<Method, 3>{Method::NatPmp, Method::Upnp, Method::Pcp};
}

const char* PortMapper::MethodName(Method method) {
    switch (method) {
    case Method::Upnp:
        return "UPnP";
    case Method::Pcp:
        return "PCP";
    case Method::NatPmp:
        return "NAT-PMP";
    case Method::None:
    default:
        return "none";
    }
}

bool PortMapper::WaitForStop(std::chrono::milliseconds delay) {
    std::unique_lock<std::mutex> lock(mutex_);
    return delay.count() <= 0 ? stopRequested_ : cv_.wait_for(lock, delay, [this] { return stopRequested_; });
}

bool PortMapper::ResolveGatewayAndLocal(std::string& gatewayAddress, std::string& localAddress, std::string& error) const {
    MIB_IPFORWARDROW route{};
    IN_ADDR destinationAddress{};
    if (!ParseIpv4("1.1.1.1", destinationAddress)) {
        error = "failed to parse route probe IPv4 address";
        return false;
    }
    if (GetBestRoute(destinationAddress.S_un.S_addr, 0, &route) != NO_ERROR) {
        error = "failed to resolve default gateway";
        return false;
    }

    gatewayAddress = Ipv4ToString(route.dwForwardNextHop);
    if (gatewayAddress.empty() || gatewayAddress == "0.0.0.0") {
        error = "default gateway has no IPv4 address";
        return false;
    }

    SocketHandle socketHandle;
    return ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, error) &&
           (GetSocketLocalIpv4(socketHandle.socket, localAddress) || (error = "failed to resolve local interface IPv4 address", false));
}

void PortMapper::ApplyOutcomeLocked(Method method, const std::string& gatewayAddress, const std::string& localAddress, const MethodOutcome& outcome) {
    activeMethod_ = method;
    activeGatewayAddress_ = gatewayAddress;
    activeLocalAddress_ = localAddress;
    activeLeases_ = outcome.leases;

    status_.running = true;
    status_.active = IsOutcomeActive(outcome);
    status_.signalTcpMapped = outcome.signalTcpMapped;
    status_.webrtcUdpAvailable = outcome.mappedUdpPorts > 0;
    status_.webrtcTcpAvailable = outcome.webrtcTcpMapped;
    status_.assignedLeaseSeconds = outcome.assignedLeaseSeconds;
    status_.mappedUdpPorts = outcome.mappedUdpPorts;
    status_.mappedTcpPorts = outcome.mappedTcpPorts;
    status_.failedUdpPorts = outcome.failedUdpPorts;
    status_.failedTcpPorts = outcome.failedTcpPorts;
    status_.method = MethodName(method);
    status_.gatewayAddress = gatewayAddress;
    status_.localAddress = localAddress;
    status_.externalAddress = outcome.externalAddress;
    status_.lastError.clear();
}

void PortMapper::SetFailureLocked(const std::string& gatewayAddress, const std::string& localAddress, const std::string& error) {
    activeMethod_ = Method::None;
    activeGatewayAddress_.clear();
    activeLocalAddress_.clear();
    activeLeases_.clear();

    status_.running = true;
    status_.active = status_.signalTcpMapped = status_.webrtcUdpAvailable = status_.webrtcTcpAvailable = false;
    status_.mappedUdpPorts = status_.mappedTcpPorts = status_.failedUdpPorts = status_.failedTcpPorts = status_.assignedLeaseSeconds = 0;
    status_.method = MethodName(Method::None);
    status_.gatewayAddress = gatewayAddress;
    status_.localAddress = localAddress;
    status_.externalAddress.clear();
    status_.lastError = error;
}

void PortMapper::Run() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.running = true;
    }
    if (!enabled_) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.running = false;
        status_.method = MethodName(Method::None);
        status_.lastError = "disabled by SLIPSTREAM_ENABLE_PORT_MAPPING";
        return;
    }

    const size_t requestedUdpPorts = status_.requestedUdpPorts;
    const size_t requestedTcpPorts = status_.requestedTcpPorts;
    const auto tryMethod = [&](Method method, const std::string& gatewayAddress, const std::string& localAddress) {
        switch (method) {
        case Method::Upnp:
            return TryUpnp(gatewayAddress, localAddress, requestedLeaseSeconds_);
        case Method::Pcp:
            return TryPcp(gatewayAddress, localAddress, requestedLeaseSeconds_);
        case Method::NatPmp:
            return TryNatPmp(gatewayAddress, localAddress, requestedLeaseSeconds_);
        case Method::None:
        default:
            return MethodOutcome{};
        }
    };

    std::chrono::milliseconds nextDelay(0);
    while (!WaitForStop(nextDelay)) {
        std::string gatewayAddress;
        std::string localAddress;
        std::string discoveryError;
        if (!ResolveGatewayAndLocal(gatewayAddress, localAddress, discoveryError)) {
            std::lock_guard<std::mutex> lock(mutex_);
            SetFailureLocked(gatewayAddress, localAddress, discoveryError);
            WARN("PortMapper: %s", discoveryError.c_str());
            nextDelay = std::chrono::seconds(30);
            continue;
        }

        std::array<Method, 3> order;
        { std::lock_guard<std::mutex> lock(mutex_); order = GetMethodOrder(activeMethod_); }

        Method successfulMethod = Method::None;
        MethodOutcome successfulOutcome;
        for (Method method : order) {
            auto outcome = tryMethod(method, gatewayAddress, localAddress);
            if (outcome.success) {
                successfulMethod = method;
                successfulOutcome = std::move(outcome);
                break;
            }
            if (!outcome.error.empty()) {
                DBG("PortMapper: %s unavailable: %s", MethodName(method), outcome.error.c_str());
            }
        }

        if (successfulMethod == Method::None) {
            std::lock_guard<std::mutex> lock(mutex_);
            SetFailureLocked(gatewayAddress, localAddress, "automatic router mapping failed for UPnP, PCP, and NAT-PMP");
            nextDelay = std::chrono::seconds(30);
            continue;
        }

        { std::lock_guard<std::mutex> lock(mutex_); ApplyOutcomeLocked(successfulMethod, gatewayAddress, localAddress, successfulOutcome); }

        LOG("PortMapper: %s active gateway=%s local=%s external=%s signal=%d udp=%zu/%zu tcp=%zu/%zu lease=%us",
            MethodName(successfulMethod),
            gatewayAddress.c_str(),
            localAddress.c_str(),
            successfulOutcome.externalAddress.empty() ? "unknown" : successfulOutcome.externalAddress.c_str(),
            successfulOutcome.signalTcpMapped ? 1 : 0,
            successfulOutcome.mappedUdpPorts,
            requestedUdpPorts,
            successfulOutcome.mappedTcpPorts,
            requestedTcpPorts,
            successfulOutcome.assignedLeaseSeconds);

        const uint32_t refreshSeconds = std::max<uint32_t>(120, std::min<uint32_t>(EffectiveLease(successfulOutcome.assignedLeaseSeconds, requestedLeaseSeconds_) / 2, 1800));
        nextDelay = std::chrono::seconds(refreshSeconds);
    }

    DeleteActiveMappings();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.running = false;
}

PortMapper::MethodOutcome PortMapper::TryUpnp(const std::string&, const std::string& localAddress, uint32_t requestedLeaseSeconds) const {
    MethodOutcome outcome;
    UpnpSession session;
    if (!DiscoverUpnp(session, outcome.error)) return outcome;

    outcome.externalAddress = session.wanAddress;
    const std::string leaseText = std::to_string(requestedLeaseSeconds);
    const char* preferredLocalAddress = session.lanAddress.empty() ? localAddress.c_str() : session.lanAddress.c_str();
    for (const auto& rule : rules_) {
        const std::string port = FormatUpnpPort(rule.internalPort);
        const int result = UPNP_AddPortMapping(session.urls.controlURL, session.data.first.servicetype, port.c_str(), port.c_str(),
                                               preferredLocalAddress, rule.description.c_str(), rule.transport, nullptr, leaseText.c_str());
        if (result == UPNPCOMMAND_SUCCESS) {
            RecordRuleSuccess(outcome, rule, rule.internalPort, requestedLeaseSeconds);
        } else {
            RecordRuleFailure(outcome, rule, std::string("UPnP AddPortMapping failed: ") + strupnperror(result));
        }
    }

    outcome.assignedLeaseSeconds = requestedLeaseSeconds;
    outcome.success = IsOutcomeActive(outcome);
    return outcome;
}

PortMapper::MethodOutcome PortMapper::TryPcp(const std::string& gatewayAddress, const std::string& localAddress, uint32_t requestedLeaseSeconds) const {
    MethodOutcome outcome;
    SocketHandle socketHandle;
    IN_ADDR localIpv4{};
    if (!InitMappedIpv4Socket(gatewayAddress, localAddress, socketHandle, localIpv4, outcome.error)) return outcome;

    uint32_t minLease = requestedLeaseSeconds;
    for (const auto& rule : rules_) {
        std::array<uint8_t, 60> request{};
        request[0] = 2;
        request[1] = 1;
        WriteBe32(request.data() + 4, requestedLeaseSeconds);
        EncodeIpv4Mapped(localIpv4, request.data() + 8);

        const auto nonce = RandomNonce();
        std::memcpy(request.data() + 24, nonce.data(), nonce.size());
        request[36] = TransportProtocolNumber(rule.transport);
        WriteBe16(request.data() + 40, rule.internalPort);
        WriteBe16(request.data() + 42, rule.internalPort);

        std::vector<uint8_t> response;
        if (!SendAndReceive(socketHandle.socket, request.data(), static_cast<int>(request.size()), response)) {
            RecordRuleFailure(outcome, rule, "no PCP response from gateway");
            continue;
        }
        if (!response.empty() && response[0] == 0) {
            outcome.error = "gateway replied with NAT-PMP version to PCP request";
            return outcome;
        }
        if (response.size() < request.size() || response[0] != 2 || response[1] != 0x81) {
            RecordRuleFailure(outcome, rule, "invalid PCP MAP response");
            continue;
        }

        const uint16_t resultCode = ReadBe16(response.data() + 2);
        if (resultCode != 0) {
            RecordRuleFailure(outcome, rule, std::string("PCP MAP failed with code ") + std::to_string(resultCode));
            continue;
        }
        if (ReadBe16(response.data() + 40) != rule.internalPort) {
            RecordRuleFailure(outcome, rule, "PCP response did not match requested internal port");
            continue;
        }

        const uint16_t externalPort = ReadBe16(response.data() + 42);
        const uint32_t assignedLease = ReadBe32(response.data() + 4);
        RecordRuleSuccess(outcome, rule, externalPort, assignedLease, &nonce);
        UpdateMinLease(minLease, assignedLease, requestedLeaseSeconds);
        if (outcome.externalAddress.empty()) {
            outcome.externalAddress = DecodeIpv4Mapped(response.data() + 44);
        }
    }

    outcome.assignedLeaseSeconds = minLease;
    outcome.success = IsOutcomeActive(outcome);
    return outcome;
}

PortMapper::MethodOutcome PortMapper::TryNatPmp(const std::string& gatewayAddress, const std::string&, uint32_t requestedLeaseSeconds) const {
    MethodOutcome outcome;
    SocketHandle socketHandle;
    std::string socketError;
    if (!ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, socketError)) return outcome.error = socketError, outcome;

    const std::array<uint8_t, 2> addressRequest = {0, 0};
    std::vector<uint8_t> addressResponse;
    if (!SendAndReceive(socketHandle.socket, addressRequest.data(), static_cast<int>(addressRequest.size()), addressResponse) || addressResponse.size() < 12) {
        outcome.error = "no NAT-PMP address response from gateway";
        return outcome;
    }
    if (addressResponse[0] != 0 || addressResponse[1] != 128) return outcome.error = "invalid NAT-PMP address response", outcome;

    const uint16_t addressResult = ReadBe16(addressResponse.data() + 2);
    if (addressResult != 0) return outcome.error = std::string("NAT-PMP address request failed with code ") + std::to_string(addressResult), outcome;
    outcome.externalAddress = Ipv4ToString(ReadRaw32(addressResponse.data() + 8));

    uint32_t minLease = requestedLeaseSeconds;
    for (const auto& rule : rules_) {
        std::array<uint8_t, 12> request{};
        request[0] = 0;
        request[1] = IsTcpTransport(rule.transport) ? 2 : 1;
        WriteBe16(request.data() + 4, rule.internalPort);
        WriteBe16(request.data() + 6, rule.internalPort);
        WriteBe32(request.data() + 8, requestedLeaseSeconds);

        std::vector<uint8_t> response;
        if (!SendAndReceive(socketHandle.socket, request.data(), static_cast<int>(request.size()), response) || response.size() < 16) {
            RecordRuleFailure(outcome, rule, "no NAT-PMP mapping response from gateway");
            continue;
        }
        if (response[0] != 0 || response[1] != static_cast<uint8_t>(128 + request[1])) {
            RecordRuleFailure(outcome, rule, "invalid NAT-PMP mapping response");
            continue;
        }

        const uint16_t resultCode = ReadBe16(response.data() + 2);
        if (resultCode != 0 || ReadBe16(response.data() + 8) != rule.internalPort) {
            RecordRuleFailure(outcome, rule, std::string("NAT-PMP mapping failed with code ") + std::to_string(resultCode));
            continue;
        }

        const uint16_t externalPort = ReadBe16(response.data() + 10);
        const uint32_t assignedLease = ReadBe32(response.data() + 12);
        RecordRuleSuccess(outcome, rule, externalPort, assignedLease);
        UpdateMinLease(minLease, assignedLease, requestedLeaseSeconds);
    }

    outcome.assignedLeaseSeconds = minLease;
    outcome.success = IsOutcomeActive(outcome);
    return outcome;
}

void PortMapper::DeleteUpnpMappings(const std::vector<MappingLease>& leases, const std::string&, const std::string&) const {
    if (leases.empty()) {
        return;
    }
    UpnpSession session;
    std::string error;
    if (!DiscoverUpnp(session, error)) return;
    for (const auto& lease : leases) {
        const std::string port = FormatUpnpPort(lease.externalPort);
        UPNP_DeletePortMapping(session.urls.controlURL, session.data.first.servicetype, port.c_str(), lease.rule.transport, nullptr);
    }
}

void PortMapper::DeletePcpMappings(const std::vector<MappingLease>& leases, const std::string& gatewayAddress, const std::string& localAddress) const {
    if (leases.empty()) {
        return;
    }
    SocketHandle socketHandle;
    IN_ADDR localIpv4{};
    std::string error;
    if (!InitMappedIpv4Socket(gatewayAddress, localAddress, socketHandle, localIpv4, error)) return;

    for (const auto& lease : leases) {
        std::array<uint8_t, 60> request{};
        request[0] = 2;
        request[1] = 1;
        EncodeIpv4Mapped(localIpv4, request.data() + 8);
        std::memcpy(request.data() + 24, lease.nonce.data(), lease.nonce.size());
        request[36] = TransportProtocolNumber(lease.rule.transport);
        WriteBe16(request.data() + 40, lease.rule.internalPort);

        std::vector<uint8_t> response;
        SendAndReceive(socketHandle.socket, request.data(), static_cast<int>(request.size()), response);
    }
}

void PortMapper::DeleteNatPmpMappings(const std::vector<MappingLease>& leases, const std::string& gatewayAddress) const {
    if (leases.empty()) {
        return;
    }
    SocketHandle socketHandle;
    std::string error;
    if (!ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, error)) return;

    for (const auto& lease : leases) {
        std::array<uint8_t, 12> request{};
        request[0] = 0;
        request[1] = IsTcpTransport(lease.rule.transport) ? 2 : 1;
        WriteBe16(request.data() + 4, lease.rule.internalPort);

        std::vector<uint8_t> response;
        SendAndReceive(socketHandle.socket, request.data(), static_cast<int>(request.size()), response);
    }
}

void PortMapper::DeleteActiveMappings() {
    Method method = Method::None;
    std::string gatewayAddress;
    std::string localAddress;
    std::vector<MappingLease> leases;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        method = activeMethod_;
        gatewayAddress = activeGatewayAddress_;
        localAddress = activeLocalAddress_;
        leases = activeLeases_;
        activeMethod_ = Method::None;
        activeGatewayAddress_.clear();
        activeLocalAddress_.clear();
        activeLeases_.clear();
        status_.active = status_.signalTcpMapped = status_.webrtcUdpAvailable = status_.webrtcTcpAvailable = false;
        status_.mappedUdpPorts = status_.mappedTcpPorts = status_.assignedLeaseSeconds = 0;
    }

    switch (method) {
    case Method::Upnp:
        DeleteUpnpMappings(leases, gatewayAddress, localAddress);
        break;
    case Method::Pcp:
        DeletePcpMappings(leases, gatewayAddress, localAddress);
        break;
    case Method::NatPmp:
        DeleteNatPmpMappings(leases, gatewayAddress);
        break;
    case Method::None:
    default:
        break;
    }
}