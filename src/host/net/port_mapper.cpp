#include "host/net/port_mapper.hpp"

#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <cctype>
#include <cstdlib>
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

    ~SocketHandle() {
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
        }
    }
};

bool GetEnvBool(const char* name, bool fallback) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return fallback;
    }

    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

uint32_t GetEnvUInt(const char* name, uint32_t fallback, uint32_t minValue, uint32_t maxValue) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (!end || *end != '\0') {
        return fallback;
    }
    if (parsed < minValue || parsed > maxValue) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

std::string Ipv4ToString(uint32_t addressNetworkOrder) {
    IN_ADDR address{};
    address.S_un.S_addr = addressNetworkOrder;
    char buffer[INET_ADDRSTRLEN] = {};
    if (!InetNtopA(AF_INET, &address, buffer, sizeof(buffer))) {
        return {};
    }
    return buffer;
}

bool ParseIpv4(const std::string& text, IN_ADDR& address) {
    return InetPtonA(AF_INET, text.c_str(), &address) == 1;
}

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

    IN_ADDR address{};
    std::memcpy(&address.S_un.S_addr, addressBytes + 12, 4);
    char buffer[INET_ADDRSTRLEN] = {};
    if (!InetNtopA(AF_INET, &address, buffer, sizeof(buffer))) {
        return {};
    }
    return buffer;
}

bool ConnectUdpSocket(const std::string& addressText, uint16_t port, SocketHandle& socketHandle, std::string& error) {
    socketHandle.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle.socket == INVALID_SOCKET) {
        error = "failed to create UDP socket";
        return false;
    }

    DWORD timeout = kSocketTimeoutMs;
    setsockopt(socketHandle.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socketHandle.socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (InetPtonA(AF_INET, addressText.c_str(), &address.sin_addr) != 1) {
        error = "invalid IPv4 address";
        return false;
    }

    if (connect(socketHandle.socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        error = "failed to connect UDP socket to gateway";
        return false;
    }
    return true;
}

bool GetSocketLocalIpv4(SOCKET socketValue, std::string& localAddress) {
    sockaddr_in address{};
    int addressLength = sizeof(address);
    if (getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
        return false;
    }
    char buffer[INET_ADDRSTRLEN] = {};
    if (!InetNtopA(AF_INET, &address.sin_addr, buffer, sizeof(buffer))) {
        return false;
    }
    localAddress = buffer;
    return true;
}

bool SendAndReceive(SOCKET socketValue, const uint8_t* requestData, int requestSize, std::vector<uint8_t>& responseData) {
    if (send(socketValue, reinterpret_cast<const char*>(requestData), requestSize, 0) != requestSize) {
        return false;
    }

    std::array<uint8_t, 1100> buffer{};
    const int received = recv(socketValue, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
    if (received <= 0) {
        return false;
    }

    responseData.assign(buffer.begin(), buffer.begin() + received);
    return true;
}

uint8_t TransportProtocolNumber(const char* transport) {
    return std::strcmp(transport, "TCP") == 0 ? 6 : 17;
}

bool IsTcpTransport(const char* transport) {
    return std::strcmp(transport, "TCP") == 0;
}

std::string FormatUpnpPort(uint16_t port) {
    return std::to_string(static_cast<unsigned int>(port));
}

std::array<uint8_t, 12> RandomNonce() {
    std::array<uint8_t, 12> nonce{};
    std::random_device device;
    for (auto& byte : nonce) {
        byte = static_cast<uint8_t>(device());
    }
    return nonce;
}

} // namespace

PortMapper::PortMapper(uint16_t httpsPort, uint16_t icePortBegin, uint16_t icePortEnd, bool iceTcpEnabled)
    : enabled_(GetEnvBool("SLIPSTREAM_ENABLE_PORT_MAPPING", true)),
      requestedLeaseSeconds_(GetEnvUInt("SLIPSTREAM_PORTMAP_LEASE_SECONDS", kDefaultLeaseSeconds, kMinLeaseSeconds, kMaxLeaseSeconds)),
      httpsPort_(httpsPort),
      icePortBegin_(icePortBegin),
      icePortEnd_(icePortEnd),
      iceTcpEnabled_(iceTcpEnabled),
      rules_([&]() {
          std::vector<MappingRule> rules;
          rules.push_back({httpsPort, "TCP", "SlipStream HTTPS", true, false});
          for (uint16_t port = icePortBegin; port <= icePortEnd; ++port) {
              rules.push_back({port, "UDP", "SlipStream WebRTC UDP", false, true});
          }
          if (iceTcpEnabled) {
              for (uint16_t port = icePortBegin; port <= icePortEnd; ++port) {
                  rules.push_back({port, "TCP", "SlipStream WebRTC TCP", false, true});
              }
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
        if (IsTcpTransport(rule.transport)) {
            status_.requestedTcpPorts++;
        } else {
            status_.requestedUdpPorts++;
        }
    }
}

PortMapper::~PortMapper() {
    Stop();
}

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

PortMappingStatus PortMapper::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
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
    if (delay.count() <= 0) {
        return stopRequested_;
    }
    return cv_.wait_for(lock, delay, [this] { return stopRequested_; });
}

bool PortMapper::ResolveGatewayAndLocal(std::string& gatewayAddress, std::string& localAddress, std::string& error) const {
    MIB_IPFORWARDROW route{};
    IN_ADDR destinationAddress{};
    if (!ParseIpv4("1.1.1.1", destinationAddress)) {
        error = "failed to parse route probe IPv4 address";
        return false;
    }
    const DWORD destination = destinationAddress.S_un.S_addr;
    if (GetBestRoute(destination, 0, &route) != NO_ERROR) {
        error = "failed to resolve default gateway";
        return false;
    }

    gatewayAddress = Ipv4ToString(route.dwForwardNextHop);
    if (gatewayAddress.empty() || gatewayAddress == "0.0.0.0") {
        error = "default gateway has no IPv4 address";
        return false;
    }

    SocketHandle socketHandle;
    if (!ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, error)) {
        return false;
    }
    if (!GetSocketLocalIpv4(socketHandle.socket, localAddress)) {
        error = "failed to resolve local interface IPv4 address";
        return false;
    }
    return true;
}

void PortMapper::ApplyOutcomeLocked(Method method, const std::string& gatewayAddress, const std::string& localAddress, const MethodOutcome& outcome) {
    activeMethod_ = method;
    activeGatewayAddress_ = gatewayAddress;
    activeLocalAddress_ = localAddress;
    activeLeases_ = outcome.leases;

    status_.running = true;
    status_.method = MethodName(method);
    status_.gatewayAddress = gatewayAddress;
    status_.localAddress = localAddress;
    status_.externalAddress = outcome.externalAddress;
    status_.assignedLeaseSeconds = outcome.assignedLeaseSeconds;
    status_.signalTcpMapped = outcome.signalTcpMapped;
    status_.webrtcUdpAvailable = outcome.mappedUdpPorts > 0;
    status_.webrtcTcpAvailable = outcome.webrtcTcpMapped;
    status_.mappedUdpPorts = outcome.mappedUdpPorts;
    status_.mappedTcpPorts = outcome.mappedTcpPorts;
    status_.failedUdpPorts = outcome.failedUdpPorts;
    status_.failedTcpPorts = outcome.failedTcpPorts;
    status_.lastError.clear();
    status_.active = outcome.signalTcpMapped || outcome.mappedUdpPorts > 0 || outcome.webrtcTcpMapped;
}

void PortMapper::SetFailureLocked(const std::string& gatewayAddress, const std::string& localAddress, const std::string& error) {
    activeMethod_ = Method::None;
    activeGatewayAddress_.clear();
    activeLocalAddress_.clear();
    activeLeases_.clear();

    status_.running = true;
    status_.active = false;
    status_.signalTcpMapped = false;
    status_.webrtcUdpAvailable = false;
    status_.webrtcTcpAvailable = false;
    status_.mappedUdpPorts = 0;
    status_.mappedTcpPorts = 0;
    status_.failedUdpPorts = 0;
    status_.failedTcpPorts = 0;
    status_.assignedLeaseSeconds = 0;
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

    std::chrono::milliseconds nextDelay(0);
    while (!WaitForStop(nextDelay)) {
        size_t requestedUdpPorts = 0;
        size_t requestedTcpPorts = 0;
        for (const auto& rule : rules_) {
            if (IsTcpTransport(rule.transport)) {
                requestedTcpPorts++;
            } else {
                requestedUdpPorts++;
            }
        }

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

        std::array<Method, 3> order = {Method::Upnp, Method::Pcp, Method::NatPmp};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeMethod_ != Method::None) {
                order[0] = activeMethod_;
                if (activeMethod_ == Method::Upnp) {
                    order[1] = Method::Pcp;
                    order[2] = Method::NatPmp;
                } else if (activeMethod_ == Method::Pcp) {
                    order[1] = Method::Upnp;
                    order[2] = Method::NatPmp;
                } else if (activeMethod_ == Method::NatPmp) {
                    order[1] = Method::Upnp;
                    order[2] = Method::Pcp;
                }
            }
        }

        Method successfulMethod = Method::None;
        MethodOutcome successfulOutcome;
        for (Method method : order) {
            MethodOutcome outcome;
            if (method == Method::Upnp) {
                outcome = TryUpnp(gatewayAddress, localAddress, requestedLeaseSeconds_);
            } else if (method == Method::Pcp) {
                outcome = TryPcp(gatewayAddress, localAddress, requestedLeaseSeconds_);
            } else if (method == Method::NatPmp) {
                outcome = TryNatPmp(gatewayAddress, localAddress, requestedLeaseSeconds_);
            }

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

        {
            std::lock_guard<std::mutex> lock(mutex_);
            ApplyOutcomeLocked(successfulMethod, gatewayAddress, localAddress, successfulOutcome);
        }

        LOG(
            "PortMapper: %s active gateway=%s local=%s external=%s signal=%d udp=%zu/%zu tcp=%zu/%zu lease=%us",
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

        const uint32_t assignedLease = successfulOutcome.assignedLeaseSeconds > 0 ? successfulOutcome.assignedLeaseSeconds : requestedLeaseSeconds_;
        const uint32_t refreshSeconds = std::max<uint32_t>(120, std::min<uint32_t>(assignedLease / 2, 1800));
        nextDelay = std::chrono::seconds(refreshSeconds);
    }

    DeleteActiveMappings();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.running = false;
}

PortMapper::MethodOutcome PortMapper::TryUpnp(const std::string& gatewayAddress, const std::string& localAddress, uint32_t requestedLeaseSeconds) const {
    MethodOutcome outcome;

    int error = 0;
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    if (!devices) {
        outcome.error = error != 0 ? std::string("discovery failed: ") + std::to_string(error) : "discovery returned no IGD";
        return outcome;
    }

    UPNPUrls urls{};
    IGDdatas data{};
    char lanAddress[64] = {};
    char wanAddress[64] = {};
    const int igdResult = UPNP_GetValidIGD(devices, &urls, &data, lanAddress, sizeof(lanAddress), wanAddress, sizeof(wanAddress));
    freeUPNPDevlist(devices);
    if (igdResult <= 0) {
        outcome.error = "no valid IGD discovered";
        return outcome;
    }

    outcome.externalAddress = wanAddress;
    std::string leaseText = std::to_string(requestedLeaseSeconds);
    const std::string preferredLocalAddress = std::strlen(lanAddress) > 0 ? std::string(lanAddress) : localAddress;

    for (const auto& rule : rules_) {
        const std::string externalPortText = FormatUpnpPort(rule.internalPort);
        const std::string internalPortText = FormatUpnpPort(rule.internalPort);
        const int result = UPNP_AddPortMapping(
            urls.controlURL,
            data.first.servicetype,
            externalPortText.c_str(),
            internalPortText.c_str(),
            preferredLocalAddress.c_str(),
            rule.description.c_str(),
            rule.transport,
            nullptr,
            leaseText.c_str());

        if (result == UPNPCOMMAND_SUCCESS) {
            MappingLease lease;
            lease.rule = rule;
            lease.externalPort = rule.internalPort;
            lease.lifetimeSeconds = requestedLeaseSeconds;
            outcome.leases.push_back(lease);
            if (IsTcpTransport(rule.transport)) {
                outcome.mappedTcpPorts++;
                if (rule.signaling) {
                    outcome.signalTcpMapped = true;
                }
                if (rule.webrtc) {
                    outcome.webrtcTcpMapped = true;
                }
            } else {
                outcome.mappedUdpPorts++;
            }
        } else {
            if (IsTcpTransport(rule.transport)) {
                outcome.failedTcpPorts++;
            } else {
                outcome.failedUdpPorts++;
            }
            if (outcome.error.empty()) {
                outcome.error = std::string("UPnP AddPortMapping failed: ") + strupnperror(result);
            }
        }
    }

    FreeUPNPUrls(&urls);

    outcome.assignedLeaseSeconds = requestedLeaseSeconds;
    outcome.success = outcome.signalTcpMapped || outcome.mappedUdpPorts > 0 || outcome.webrtcTcpMapped;
    return outcome;
}

PortMapper::MethodOutcome PortMapper::TryPcp(const std::string& gatewayAddress, const std::string& localAddress, uint32_t requestedLeaseSeconds) const {
    MethodOutcome outcome;

    SocketHandle socketHandle;
    std::string socketError;
    if (!ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, socketError)) {
        outcome.error = socketError;
        return outcome;
    }

    IN_ADDR localIpv4{};
    if (!ParseIpv4(localAddress, localIpv4)) {
        outcome.error = "failed to parse local IPv4 address";
        return outcome;
    }

    uint32_t minLease = requestedLeaseSeconds;
    for (const auto& rule : rules_) {
        std::array<uint8_t, 60> request{};
        request[0] = 2;
        request[1] = 1;

        const uint32_t lifetimeNetwork = htonl(requestedLeaseSeconds);
        std::memcpy(request.data() + 4, &lifetimeNetwork, sizeof(lifetimeNetwork));
        EncodeIpv4Mapped(localIpv4, request.data() + 8);

        const auto nonce = RandomNonce();
        std::memcpy(request.data() + 24, nonce.data(), nonce.size());
        request[36] = TransportProtocolNumber(rule.transport);

        const uint16_t internalPortNetwork = htons(rule.internalPort);
        std::memcpy(request.data() + 40, &internalPortNetwork, sizeof(internalPortNetwork));
        std::memcpy(request.data() + 42, &internalPortNetwork, sizeof(internalPortNetwork));

        std::vector<uint8_t> response;
        if (!SendAndReceive(socketHandle.socket, request.data(), static_cast<int>(request.size()), response)) {
            if (IsTcpTransport(rule.transport)) {
                outcome.failedTcpPorts++;
            } else {
                outcome.failedUdpPorts++;
            }
            if (outcome.error.empty()) {
                outcome.error = "no PCP response from gateway";
            }
            continue;
        }

        if (!response.empty() && response[0] == 0) {
            outcome.error = "gateway replied with NAT-PMP version to PCP request";
            return outcome;
        }

        if (response.size() < 60 || response[0] != 2 || response[1] != 0x81) {
            if (IsTcpTransport(rule.transport)) {
                outcome.failedTcpPorts++;
            } else {
                outcome.failedUdpPorts++;
            }
            if (outcome.error.empty()) {
                outcome.error = "invalid PCP MAP response";
            }
            continue;
        }

        uint16_t resultCode = 0;
        std::memcpy(&resultCode, response.data() + 2, sizeof(resultCode));
        resultCode = ntohs(resultCode);
        if (resultCode != 0) {
            if (IsTcpTransport(rule.transport)) {
                outcome.failedTcpPorts++;
            } else {
                outcome.failedUdpPorts++;
            }
            if (outcome.error.empty()) {
                outcome.error = std::string("PCP MAP failed with code ") + std::to_string(resultCode);
            }
            continue;
        }

        uint16_t internalPortResponse = 0;
        uint16_t externalPortResponse = 0;
        uint32_t assignedLease = 0;
        std::memcpy(&internalPortResponse, response.data() + 40, sizeof(internalPortResponse));
        std::memcpy(&externalPortResponse, response.data() + 42, sizeof(externalPortResponse));
        std::memcpy(&assignedLease, response.data() + 4, sizeof(assignedLease));
        internalPortResponse = ntohs(internalPortResponse);
        externalPortResponse = ntohs(externalPortResponse);
        assignedLease = ntohl(assignedLease);

        if (internalPortResponse != rule.internalPort) {
            if (IsTcpTransport(rule.transport)) {
                outcome.failedTcpPorts++;
            } else {
                outcome.failedUdpPorts++;
            }
            if (outcome.error.empty()) {
                outcome.error = "PCP response did not match requested internal port";
            }
            continue;
        }

        MappingLease lease;
        lease.rule = rule;
        lease.externalPort = externalPortResponse;
        lease.lifetimeSeconds = assignedLease;
        lease.nonce = nonce;
        outcome.leases.push_back(lease);

        if (IsTcpTransport(rule.transport)) {
            outcome.mappedTcpPorts++;
            if (rule.signaling) {
                outcome.signalTcpMapped = true;
            }
            if (rule.webrtc) {
                outcome.webrtcTcpMapped = true;
            }
        } else {
            outcome.mappedUdpPorts++;
        }

        minLease = std::min(minLease, assignedLease > 0 ? assignedLease : requestedLeaseSeconds);
        if (outcome.externalAddress.empty()) {
            outcome.externalAddress = DecodeIpv4Mapped(response.data() + 44);
        }
    }

    outcome.assignedLeaseSeconds = minLease;
    outcome.success = outcome.signalTcpMapped || outcome.mappedUdpPorts > 0 || outcome.webrtcTcpMapped;
    return outcome;
}

PortMapper::MethodOutcome PortMapper::TryNatPmp(const std::string& gatewayAddress, const std::string&, uint32_t requestedLeaseSeconds) const {
    MethodOutcome outcome;

    SocketHandle socketHandle;
    std::string socketError;
    if (!ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, socketError)) {
        outcome.error = socketError;
        return outcome;
    }

    const std::array<uint8_t, 2> addressRequest = {0, 0};
    std::vector<uint8_t> addressResponse;
    if (!SendAndReceive(socketHandle.socket, addressRequest.data(), static_cast<int>(addressRequest.size()), addressResponse) || addressResponse.size() < 12) {
        outcome.error = "no NAT-PMP address response from gateway";
        return outcome;
    }

    if (addressResponse[0] != 0 || addressResponse[1] != 128) {
        outcome.error = "invalid NAT-PMP address response";
        return outcome;
    }

    uint16_t addressResult = 0;
    std::memcpy(&addressResult, addressResponse.data() + 2, sizeof(addressResult));
    addressResult = ntohs(addressResult);
    if (addressResult != 0) {
        outcome.error = std::string("NAT-PMP address request failed with code ") + std::to_string(addressResult);
        return outcome;
    }
    uint32_t externalAddressNetworkOrder = 0;
    std::memcpy(&externalAddressNetworkOrder, addressResponse.data() + 8, sizeof(externalAddressNetworkOrder));
    outcome.externalAddress = Ipv4ToString(externalAddressNetworkOrder);

    uint32_t minLease = requestedLeaseSeconds;
    for (const auto& rule : rules_) {
        std::array<uint8_t, 12> request{};
        request[0] = 0;
        request[1] = IsTcpTransport(rule.transport) ? 2 : 1;

        const uint16_t internalPortNetwork = htons(rule.internalPort);
        std::memcpy(request.data() + 4, &internalPortNetwork, sizeof(internalPortNetwork));
        std::memcpy(request.data() + 6, &internalPortNetwork, sizeof(internalPortNetwork));

        const uint32_t lifetimeNetwork = htonl(requestedLeaseSeconds);
        std::memcpy(request.data() + 8, &lifetimeNetwork, sizeof(lifetimeNetwork));

        std::vector<uint8_t> response;
        if (!SendAndReceive(socketHandle.socket, request.data(), static_cast<int>(request.size()), response) || response.size() < 16) {
            if (IsTcpTransport(rule.transport)) {
                outcome.failedTcpPorts++;
            } else {
                outcome.failedUdpPorts++;
            }
            if (outcome.error.empty()) {
                outcome.error = "no NAT-PMP mapping response from gateway";
            }
            continue;
        }

        if (response[0] != 0 || response[1] != static_cast<uint8_t>(128 + request[1])) {
            if (IsTcpTransport(rule.transport)) {
                outcome.failedTcpPorts++;
            } else {
                outcome.failedUdpPorts++;
            }
            if (outcome.error.empty()) {
                outcome.error = "invalid NAT-PMP mapping response";
            }
            continue;
        }

        uint16_t resultCode = 0;
        uint16_t internalPortResponse = 0;
        uint16_t externalPortResponse = 0;
        uint32_t assignedLease = 0;
        std::memcpy(&resultCode, response.data() + 2, sizeof(resultCode));
        std::memcpy(&internalPortResponse, response.data() + 8, sizeof(internalPortResponse));
        std::memcpy(&externalPortResponse, response.data() + 10, sizeof(externalPortResponse));
        std::memcpy(&assignedLease, response.data() + 12, sizeof(assignedLease));
        resultCode = ntohs(resultCode);
        internalPortResponse = ntohs(internalPortResponse);
        externalPortResponse = ntohs(externalPortResponse);
        assignedLease = ntohl(assignedLease);

        if (resultCode != 0 || internalPortResponse != rule.internalPort) {
            if (IsTcpTransport(rule.transport)) {
                outcome.failedTcpPorts++;
            } else {
                outcome.failedUdpPorts++;
            }
            if (outcome.error.empty()) {
                outcome.error = std::string("NAT-PMP mapping failed with code ") + std::to_string(resultCode);
            }
            continue;
        }

        MappingLease lease;
        lease.rule = rule;
        lease.externalPort = externalPortResponse;
        lease.lifetimeSeconds = assignedLease;
        outcome.leases.push_back(lease);

        if (IsTcpTransport(rule.transport)) {
            outcome.mappedTcpPorts++;
            if (rule.signaling) {
                outcome.signalTcpMapped = true;
            }
            if (rule.webrtc) {
                outcome.webrtcTcpMapped = true;
            }
        } else {
            outcome.mappedUdpPorts++;
        }

        minLease = std::min(minLease, assignedLease > 0 ? assignedLease : requestedLeaseSeconds);
    }

    outcome.assignedLeaseSeconds = minLease;
    outcome.success = outcome.signalTcpMapped || outcome.mappedUdpPorts > 0 || outcome.webrtcTcpMapped;
    return outcome;
}

void PortMapper::DeleteUpnpMappings(const std::vector<MappingLease>& leases, const std::string&, const std::string&) const {
    if (leases.empty()) {
        return;
    }

    int error = 0;
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    if (!devices) {
        return;
    }

    UPNPUrls urls{};
    IGDdatas data{};
    char lanAddress[64] = {};
    char wanAddress[64] = {};
    const int igdResult = UPNP_GetValidIGD(devices, &urls, &data, lanAddress, sizeof(lanAddress), wanAddress, sizeof(wanAddress));
    freeUPNPDevlist(devices);
    if (igdResult <= 0) {
        return;
    }

    for (const auto& lease : leases) {
        const std::string externalPort = FormatUpnpPort(lease.externalPort);
        UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, externalPort.c_str(), lease.rule.transport, nullptr);
    }

    FreeUPNPUrls(&urls);
}

void PortMapper::DeletePcpMappings(const std::vector<MappingLease>& leases, const std::string& gatewayAddress, const std::string& localAddress) const {
    if (leases.empty()) {
        return;
    }

    SocketHandle socketHandle;
    std::string socketError;
    if (!ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, socketError)) {
        return;
    }

    IN_ADDR localIpv4{};
    if (!ParseIpv4(localAddress, localIpv4)) {
        return;
    }

    for (const auto& lease : leases) {
        std::array<uint8_t, 60> request{};
        request[0] = 2;
        request[1] = 1;
        EncodeIpv4Mapped(localIpv4, request.data() + 8);
        std::memcpy(request.data() + 24, lease.nonce.data(), lease.nonce.size());
        request[36] = TransportProtocolNumber(lease.rule.transport);

        const uint16_t internalPortNetwork = htons(lease.rule.internalPort);
        std::memcpy(request.data() + 40, &internalPortNetwork, sizeof(internalPortNetwork));

        std::vector<uint8_t> response;
        SendAndReceive(socketHandle.socket, request.data(), static_cast<int>(request.size()), response);
    }
}

void PortMapper::DeleteNatPmpMappings(const std::vector<MappingLease>& leases, const std::string& gatewayAddress) const {
    if (leases.empty()) {
        return;
    }

    SocketHandle socketHandle;
    std::string socketError;
    if (!ConnectUdpSocket(gatewayAddress, kPortMapProtocolPort, socketHandle, socketError)) {
        return;
    }

    for (const auto& lease : leases) {
        std::array<uint8_t, 12> request{};
        request[0] = 0;
        request[1] = IsTcpTransport(lease.rule.transport) ? 2 : 1;

        const uint16_t internalPortNetwork = htons(lease.rule.internalPort);
        std::memcpy(request.data() + 4, &internalPortNetwork, sizeof(internalPortNetwork));

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
        status_.active = false;
        status_.signalTcpMapped = false;
        status_.webrtcUdpAvailable = false;
        status_.webrtcTcpAvailable = false;
        status_.mappedUdpPorts = 0;
        status_.mappedTcpPorts = 0;
        status_.assignedLeaseSeconds = 0;
    }

    if (method == Method::Upnp) {
        DeleteUpnpMappings(leases, gatewayAddress, localAddress);
    } else if (method == Method::Pcp) {
        DeletePcpMappings(leases, gatewayAddress, localAddress);
    } else if (method == Method::NatPmp) {
        DeleteNatPmpMappings(leases, gatewayAddress);
    }
}