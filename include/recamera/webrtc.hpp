#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <recamera/error.hpp>
#include <recamera/frame.hpp>

namespace recamera
{

struct WebRtcConfig
{
    std::uint16_t port = 8080;
    std::string path = "/live0";
    std::string bindAddress = "0.0.0.0";
    std::string stunServer;
    int fps = 30;
    std::size_t maxClients = 2;
};

struct WebRtcStatus
{
    bool running = false;
    int clients = 0;
    std::uint64_t totalConnections = 0;
    std::uint64_t totalDisconnections = 0;
    std::uint64_t framesSent = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t writeErrors = 0;
};

class WebRtcStream
{
  public:
    WebRtcStream();
    ~WebRtcStream();
    WebRtcStream(WebRtcStream &&) noexcept;
    WebRtcStream &operator=(WebRtcStream &&) noexcept;
    WebRtcStream(const WebRtcStream &) = delete;
    WebRtcStream &operator=(const WebRtcStream &) = delete;

    bool start(const WebRtcConfig &config = {});
    bool write(const EncodedFrame &frame);
    void stop() noexcept;
    bool running() const noexcept;
    WebRtcStatus status() const noexcept;
    Error lastError() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recamera
