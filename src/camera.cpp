#include <recamera/camera.hpp>

#include <mutex>
#include <utility>

#include "backend/sg200x/camera_backend.hpp"
#include "backend/sg200x/rtsp_backend.hpp"

namespace recamera
{

class Camera::Impl
{
  public:
    Impl() : rtsp(camera.channel())
    {
        camera.setRtspHandler([this](void *stream, void *context) { return rtsp.writeEncodedFrame(stream, context); });
    }

    ~Impl()
    {
        close();
    }

    bool open(const CameraConfig &config)
    {
        Error nextError;
        const bool ok = camera.open(config, 60, nextError);
        setError(nextError);
        return ok;
    }

    bool start()
    {
        Error nextError;
        const bool ok = camera.start(nextError);
        setError(nextError);
        return ok;
    }

    bool setGop(int gop)
    {
        Error nextError;
        const bool ok = camera.setGop(gop, nextError);
        setError(nextError);
        return ok;
    }

    bool requestIdr()
    {
        Error nextError;
        const bool ok = camera.requestIdr(nextError);
        setError(nextError);
        return ok;
    }

    void stop() noexcept
    {
        camera.stop();
        rtsp.stop();
    }

    void close() noexcept
    {
        camera.stop();
        rtsp.stop();
        camera.close();
    }

    bool startRtsp(const RtspConfig &config)
    {
        Error nextError;
        if (!camera.setGop(config.gop, nextError))
        {
            setError(nextError);
            return false;
        }
        const bool ok = rtsp.start(config, nextError);
        setError(nextError);
        return ok;
    }

    void setError(const Error &value)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = value;
    }

    Error getError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        return error;
    }

    sg200x::CameraBackend camera;
    sg200x::RtspBackend rtsp;
    mutable std::mutex errorMutex;
    Error error;
};

Camera::Camera() : impl_(std::make_unique<Impl>())
{
}
Camera::~Camera() = default;
Camera::Camera(Camera &&) noexcept = default;
Camera &Camera::operator=(Camera &&) noexcept = default;

bool Camera::open(const CameraConfig &config)
{
    return impl_ && impl_->open(config);
}
bool Camera::start()
{
    return impl_ && impl_->start();
}
void Camera::stop() noexcept
{
    if (impl_)
        impl_->stop();
}
void Camera::close() noexcept
{
    if (impl_)
        impl_->close();
}
bool Camera::isOpen() const noexcept
{
    return impl_ && impl_->camera.isOpen();
}
bool Camera::isRunning() const noexcept
{
    return impl_ && impl_->camera.isRunning();
}
bool Camera::setGop(int gop)
{
    return impl_ && impl_->setGop(gop);
}
bool Camera::requestIdr()
{
    return impl_ && impl_->requestIdr();
}
std::optional<VideoFrame> Camera::read(std::chrono::milliseconds timeout)
{
    return impl_ ? impl_->camera.read(timeout) : std::nullopt;
}
std::optional<EncodedFrame> Camera::readEncoded(std::chrono::milliseconds timeout)
{
    return impl_ ? impl_->camera.readEncoded(timeout) : std::nullopt;
}

bool Camera::startRtsp(const RtspConfig &config)
{
    return impl_ && impl_->startRtsp(config);
}
void Camera::stopRtsp() noexcept
{
    if (impl_)
        impl_->rtsp.stop();
}
bool Camera::isRtspRunning() const noexcept
{
    return impl_ && impl_->rtsp.running();
}
StreamStatus Camera::streamStatus() const noexcept
{
    return impl_ ? impl_->rtsp.status() : StreamStatus{};
}

bool Camera::setFrameHandler(FrameHandler handler)
{
    if (!impl_)
        return false;
    impl_->camera.setFrameHandler(std::move(handler));
    return true;
}

Error Camera::lastError() const
{
    return impl_ ? impl_->getError() : Error{ErrorCode::NotInitialized, "moved-from Camera"};
}

} // namespace recamera
