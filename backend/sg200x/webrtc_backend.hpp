#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <recamera/webrtc.hpp>

namespace rtc {
class PeerConnection;
class Track;
class RtpPacketizationConfig;
}

namespace recamera::sg200x {

class WebRtcBackend {
public:
    WebRtcBackend() = default;
    ~WebRtcBackend();

    bool start(const WebRtcConfig& config, Error& error);
    bool write(const EncodedFrame& frame, Error& error);
    void stop() noexcept;
    bool running() const noexcept;
    WebRtcStatus status() const noexcept;

private:
    struct Client;
    void serve() noexcept;
    void handleConnection(int fd) noexcept;
    std::string createAnswer(const std::string& offer, Error& error);
    void removeClosedClients();

    mutable std::mutex mutex_;
    WebRtcConfig config_;
    WebRtcStatus status_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::thread serverThread_;
    std::atomic<bool> running_{false};
    int listenFd_ = -1;
};

}  // namespace recamera::sg200x
