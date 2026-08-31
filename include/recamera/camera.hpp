#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include <recamera/error.hpp>
#include <recamera/frame.hpp>
#include <recamera/rtsp.hpp>

namespace recamera
{

struct CameraConfig
{
    int width = 1920;
    int height = 1080;
    int fps = 30;
    std::size_t queueDepth = 2;
    // Optional independent VPSS output used by read(). Zero values inherit
    // the encoded stream settings. RGB888 is useful for model input.
    int rawWidth = 0;
    int rawHeight = 0;
    int rawFps = 0;
    PixelFormat rawFormat = PixelFormat::NV21;
};

class Camera
{
  public:
    using FrameHandler = std::function<void(const Frame &)>;

    Camera();
    ~Camera();
    Camera(Camera &&) noexcept;
    Camera &operator=(Camera &&) noexcept;
    Camera(const Camera &) = delete;
    Camera &operator=(const Camera &) = delete;

    bool open(const CameraConfig &config = {});
    bool start();
    void stop() noexcept;
    void close() noexcept;

    bool isOpen() const noexcept;
    bool isRunning() const noexcept;

    // Encoding controls for applications that explicitly connect Camera to a
    // streaming protocol. setGop() must be called after open() and before start().
    bool setGop(int gop);
    bool requestIdr();

    std::optional<VideoFrame> read(std::chrono::milliseconds timeout);
    std::optional<EncodedFrame> readEncoded(std::chrono::milliseconds timeout);

    bool startRtsp(const RtspConfig &config = {});
    void stopRtsp() noexcept;
    bool isRtspRunning() const noexcept;
    StreamStatus streamStatus() const noexcept;

    // Receives encoded H.264 access units. The handler runs on the vendor VENC
    // worker thread and must return quickly.
    bool setFrameHandler(FrameHandler handler);

    Error lastError() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recamera
