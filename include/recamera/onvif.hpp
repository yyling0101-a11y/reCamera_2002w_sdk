#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <recamera/error.hpp>

namespace recamera
{

struct OnvifConfig
{
    std::string address; // Advertised IPv4 address; empty selects a non-loopback interface.
    std::uint16_t port = 8000;
    std::string username;
    std::string password;
    std::string rtspPath = "/onvif";
    std::uint16_t rtspPort = 8554;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrateKbps = 4096;
    int gop = 30;
    std::string manufacturer = "Seeed Studio";
    std::string model = "reCamera 2002W";
    std::string firmwareVersion = "1.0";
    std::string serialNumber = "recamera2002w";
    std::string hardwareId = "SG2002W";
    std::string deviceName = "reCamera";
    std::string deviceUuid = "550e8400-e29b-41d4-a716-446655440000";
    // Called for ONVIF SetSynchronizationPoint. Usually bind this to
    // Camera::requestIdr so NVRs can obtain a fresh decodable H.264 keyframe.
    std::function<bool()> requestKeyframe;
};

struct OnvifStatus
{
    bool running = false;
    std::uint64_t discoveryRequests = 0;
    std::uint64_t soapRequests = 0;
    std::uint64_t authenticationFailures = 0;
};

class OnvifService
{
  public:
    OnvifService();
    ~OnvifService();
    OnvifService(OnvifService &&) noexcept;
    OnvifService &operator=(OnvifService &&) noexcept;
    OnvifService(const OnvifService &) = delete;
    OnvifService &operator=(const OnvifService &) = delete;

    bool start(const OnvifConfig &config);
    void stop() noexcept;
    bool running() const noexcept;
    OnvifStatus status() const noexcept;
    Error lastError() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recamera
