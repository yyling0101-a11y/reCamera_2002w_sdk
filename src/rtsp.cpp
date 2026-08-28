#include <recamera/rtsp.hpp>

#include <mutex>

#include "backend/sg200x/rtsp_backend.hpp"

namespace recamera
{

class RtspStream::Impl
{
  public:
    Impl() : backend(2)
    {
    }

    void setError(Error next)
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = std::move(next);
    }

    Error getError() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return error;
    }

    sg200x::RtspBackend backend;
    mutable std::mutex mutex;
    Error error;
};

RtspStream::RtspStream() : impl_(std::make_unique<Impl>())
{
}
RtspStream::~RtspStream() = default;
RtspStream::RtspStream(RtspStream &&) noexcept = default;
RtspStream &RtspStream::operator=(RtspStream &&) noexcept = default;

bool RtspStream::start(const RtspConfig &config)
{
    if (!impl_)
        return false;
    Error error;
    const bool ok = impl_->backend.start(config, error);
    impl_->setError(error);
    return ok;
}

bool RtspStream::write(const EncodedFrame &frame)
{
    if (!impl_)
        return false;
    Error error;
    const bool ok = impl_->backend.write(frame, error);
    impl_->setError(error);
    return ok;
}

void RtspStream::stop() noexcept
{
    if (impl_)
        impl_->backend.stop();
}
bool RtspStream::running() const noexcept
{
    return impl_ && impl_->backend.running();
}
StreamStatus RtspStream::status() const noexcept
{
    return impl_ ? impl_->backend.status() : StreamStatus{};
}
Error RtspStream::lastError() const
{
    return impl_ ? impl_->getError() : Error{ErrorCode::NotInitialized, "moved-from RtspStream"};
}

} // namespace recamera
