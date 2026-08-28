#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <recamera/error.hpp>
#include <recamera/frame.hpp>
#include <recamera/status.hpp>

namespace recamera
{

struct RtspConfig
{
    std::uint16_t port = 8554;
    std::string path = "/live0";
    int gop = 60;
    bool requestIdrOnConnect = true;
    bool autoRecover = true;
    double slowWriteThresholdMs = 100.0;
    unsigned errorsBeforeIdr = 3;
    unsigned errorsBeforeRestart = 12;
    bool authenticationEnabled = false;
    std::string username;
    std::string password;
    std::string realm = "reCamera";
};

class RtspStream
{
  public:
    RtspStream();
    ~RtspStream();
    RtspStream(RtspStream &&) noexcept;
    RtspStream &operator=(RtspStream &&) noexcept;
    RtspStream(const RtspStream &) = delete;
    RtspStream &operator=(const RtspStream &) = delete;

    bool start(const RtspConfig &config = {});
    bool write(const EncodedFrame &frame);
    void stop() noexcept;
    bool running() const noexcept;
    StreamStatus status() const noexcept;
    Error lastError() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recamera
