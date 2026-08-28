#pragma once

#include <cstdint>

namespace recamera
{

enum class RecoveryLevel
{
    Streaming,
    RequestingIdr,
    Restarting
};

struct StreamStatus
{
    bool running = false;
    std::uint64_t framesSent = 0;
    std::uint64_t writeErrors = 0;
    std::uint64_t slowWrites = 0;
    std::uint64_t lastFrameTimestamp = 0;
    double lastWriteLatencyMs = 0.0;
    double maxWriteLatencyMs = 0.0;
    int clients = 0;
    std::uint64_t totalConnections = 0;
    std::uint64_t totalDisconnections = 0;
    std::uint64_t idrRequests = 0;
    std::uint64_t restarts = 0;
    RecoveryLevel recoveryLevel = RecoveryLevel::Streaming;
};

const char *toString(RecoveryLevel level) noexcept;

} // namespace recamera
