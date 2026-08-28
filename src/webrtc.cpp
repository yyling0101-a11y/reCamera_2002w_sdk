#include <recamera/webrtc.hpp>

#include <mutex>
#include <utility>

#include "backend/sg200x/webrtc_backend.hpp"

namespace recamera
{

class WebRtcStream::Impl
{
  public:
    void setError(Error value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = std::move(value);
    }
    Error getError() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return error;
    }
    sg200x::WebRtcBackend backend;
    mutable std::mutex mutex;
    Error error;
};

WebRtcStream::WebRtcStream() : impl_(std::make_unique<Impl>())
{
}
WebRtcStream::~WebRtcStream() = default;
WebRtcStream::WebRtcStream(WebRtcStream &&) noexcept = default;
WebRtcStream &WebRtcStream::operator=(WebRtcStream &&) noexcept = default;

bool WebRtcStream::start(const WebRtcConfig &config)
{
    if (!impl_)
        return false;
    Error error;
    const bool ok = impl_->backend.start(config, error);
    impl_->setError(error);
    return ok;
}
bool WebRtcStream::write(const EncodedFrame &frame)
{
    if (!impl_)
        return false;
    Error error;
    const bool ok = impl_->backend.write(frame, error);
    impl_->setError(error);
    return ok;
}
void WebRtcStream::stop() noexcept
{
    if (impl_)
        impl_->backend.stop();
}
bool WebRtcStream::running() const noexcept
{
    return impl_ && impl_->backend.running();
}
WebRtcStatus WebRtcStream::status() const noexcept
{
    return impl_ ? impl_->backend.status() : WebRtcStatus{};
}
Error WebRtcStream::lastError() const
{
    return impl_ ? impl_->getError() : Error{ErrorCode::NotInitialized, "moved-from WebRtcStream"};
}

} // namespace recamera
