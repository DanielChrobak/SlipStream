#pragma once

#include "host/core/common.hpp"

struct PortMappingStatus {
    bool enabled = false;
    bool running = false;
    bool active = false;
    bool signalTcpMapped = false;
    bool webrtcUdpAvailable = false;
    bool webrtcTcpAvailable = false;
    uint16_t httpsPort = 0;
    uint16_t icePortBegin = 0;
    uint16_t icePortEnd = 0;
    bool iceTcpEnabled = false;
    uint32_t requestedLeaseSeconds = 0;
    uint32_t assignedLeaseSeconds = 0;
    size_t requestedUdpPorts = 0;
    size_t requestedTcpPorts = 0;
    size_t mappedUdpPorts = 0;
    size_t mappedTcpPorts = 0;
    size_t failedUdpPorts = 0;
    size_t failedTcpPorts = 0;
    std::string method;
    std::string gatewayAddress;
    std::string localAddress;
    std::string externalAddress;
    std::string lastError;
};

class PortMapper {
public:
    PortMapper(uint16_t httpsPort, uint16_t icePortBegin, uint16_t icePortEnd, bool iceTcpEnabled);
    ~PortMapper();

    void Start();
    void Stop();
    [[nodiscard]] bool IsEnabled() const { return enabled_; }
    [[nodiscard]] PortMappingStatus GetStatus() const;

private:
    enum class Method {
        None,
        Upnp,
        Pcp,
        NatPmp,
    };

    struct MappingRule {
        uint16_t internalPort = 0;
        const char* transport = "UDP";
        std::string description;
        bool signaling = false;
        bool webrtc = false;
    };

    struct MappingLease {
        MappingRule rule;
        uint16_t externalPort = 0;
        uint32_t lifetimeSeconds = 0;
        std::array<uint8_t, 12> nonce{};
    };

    struct MethodOutcome {
        bool success = false;
        bool signalTcpMapped = false;
        bool webrtcTcpMapped = false;
        uint32_t assignedLeaseSeconds = 0;
        size_t mappedUdpPorts = 0;
        size_t mappedTcpPorts = 0;
        size_t failedUdpPorts = 0;
        size_t failedTcpPorts = 0;
        std::string externalAddress;
        std::string error;
        std::vector<MappingLease> leases;
    };

    void Run();
    bool WaitForStop(std::chrono::milliseconds delay);
    bool ResolveGatewayAndLocal(std::string& gatewayAddress, std::string& localAddress, std::string& error) const;
    MethodOutcome TryUpnp(const std::string& gatewayAddress, const std::string& localAddress, uint32_t requestedLeaseSeconds) const;
    MethodOutcome TryPcp(const std::string& gatewayAddress, const std::string& localAddress, uint32_t requestedLeaseSeconds) const;
    MethodOutcome TryNatPmp(const std::string& gatewayAddress, const std::string& localAddress, uint32_t requestedLeaseSeconds) const;
    void DeleteActiveMappings();
    void DeleteUpnpMappings(const std::vector<MappingLease>& leases, const std::string& gatewayAddress, const std::string& localAddress) const;
    void DeletePcpMappings(const std::vector<MappingLease>& leases, const std::string& gatewayAddress, const std::string& localAddress) const;
    void DeleteNatPmpMappings(const std::vector<MappingLease>& leases, const std::string& gatewayAddress) const;
    void ApplyOutcomeLocked(Method method, const std::string& gatewayAddress, const std::string& localAddress, const MethodOutcome& outcome);
    void SetFailureLocked(const std::string& gatewayAddress, const std::string& localAddress, const std::string& error);
    static void RecordRuleFailure(MethodOutcome& outcome, const MappingRule& rule, std::string error);
    static void RecordRuleSuccess(MethodOutcome& outcome, const MappingRule& rule, uint16_t externalPort, uint32_t lifetimeSeconds,
                                  const std::array<uint8_t, 12>* nonce = nullptr);
    static bool IsOutcomeActive(const MethodOutcome& outcome);
    static std::array<Method, 3> GetMethodOrder(Method activeMethod);
    static const char* MethodName(Method method);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopRequested_ = false;
    bool started_ = false;
    const bool enabled_;
    const uint32_t requestedLeaseSeconds_;
    const uint16_t httpsPort_;
    const uint16_t icePortBegin_;
    const uint16_t icePortEnd_;
    const bool iceTcpEnabled_;
    const std::vector<MappingRule> rules_;
    Method activeMethod_ = Method::None;
    std::string activeGatewayAddress_;
    std::string activeLocalAddress_;
    std::vector<MappingLease> activeLeases_;
    PortMappingStatus status_;
};