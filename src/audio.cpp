#include <recamera/audio.hpp>

#include <alsa/asoundlib.h>

#include <mutex>
#include <string>
#include <utility>

namespace recamera {
namespace {
bool valid(const AudioConfig &config) {
  return config.sampleRate >= 8000 && config.sampleRate <= 48000 &&
         config.channels >= 1 && config.channels <= 2 &&
         config.periodFrames >= 64;
}

bool configure(snd_pcm_t *pcm, const AudioConfig &config, Error &error) {
  snd_pcm_hw_params_t *params = nullptr;
  snd_pcm_hw_params_alloca(&params);
  int rc = snd_pcm_hw_params_any(pcm, params);
  if (rc >= 0)
    rc = snd_pcm_hw_params_set_access(pcm, params,
                                      SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc >= 0)
    rc = snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
  unsigned rate = static_cast<unsigned>(config.sampleRate);
  int rateDirection = 0;
  if (rc >= 0)
    rc = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, &rateDirection);
  if (rc >= 0 && rate != static_cast<unsigned>(config.sampleRate))
    rc = -EINVAL;
  if (rc >= 0)
    rc = snd_pcm_hw_params_set_channels(pcm, params,
                                        static_cast<unsigned>(config.channels));
  snd_pcm_uframes_t period = config.periodFrames;
  int periodDirection = 0;
  if (rc >= 0)
    rc = snd_pcm_hw_params_set_period_size_near(pcm, params, &period,
                                                &periodDirection);
  if (rc >= 0)
    rc = snd_pcm_hw_params(pcm, params);
  if (rc < 0) {
    error = {ErrorCode::BackendError,
             std::string("ALSA hardware configuration failed: ") +
                 snd_strerror(rc)};
    return false;
  }
  error = {};
  return true;
}
} // namespace

class Microphone::Impl {
public:
  mutable std::mutex mutex;
  snd_pcm_t *pcm = nullptr;
  AudioConfig config;
  Error error;
};

Microphone::Microphone() : impl_(std::make_unique<Impl>()) {}
Microphone::~Microphone() { close(); }
Microphone::Microphone(Microphone &&) noexcept = default;
Microphone &Microphone::operator=(Microphone &&) noexcept = default;

bool Microphone::open(const AudioConfig &config) {
  close();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!valid(config)) {
    impl_->error = {
        ErrorCode::InvalidArgument,
        "audio requires 8-48 kHz, 1-2 channels, and periodFrames >= 64"};
    return false;
  }
  int rc = snd_pcm_open(&impl_->pcm, "hw:0,0", SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    impl_->pcm = nullptr;
    impl_->error = {ErrorCode::BackendError,
                    std::string("cannot open microphone hw:0,0: ") +
                        snd_strerror(rc)};
    return false;
  }
  if (!configure(impl_->pcm, config, impl_->error)) {
    snd_pcm_close(impl_->pcm);
    impl_->pcm = nullptr;
    return false;
  }
  impl_->config = config;
  return true;
}

bool Microphone::read(std::int16_t *samples, std::size_t frames) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->pcm || !samples || frames == 0) {
    impl_->error = {ErrorCode::InvalidArgument,
                    "microphone is closed or the destination is invalid"};
    return false;
  }
  std::size_t done = 0;
  while (done < frames) {
    snd_pcm_sframes_t rc = snd_pcm_readi(
        impl_->pcm, samples + done * impl_->config.channels, frames - done);
    if (rc < 0)
      rc = snd_pcm_recover(impl_->pcm, static_cast<int>(rc), 1);
    if (rc < 0) {
      impl_->error = {ErrorCode::BackendError,
                      std::string("microphone read failed: ") +
                          snd_strerror(rc)};
      return false;
    }
    done += static_cast<std::size_t>(rc);
  }
  impl_->error = {};
  return true;
}

void Microphone::close() noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->pcm)
    snd_pcm_close(impl_->pcm);
  impl_->pcm = nullptr;
}
bool Microphone::isOpen() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->pcm;
}
AudioConfig Microphone::config() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->config;
}
Error Microphone::lastError() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->error;
}

class Speaker::Impl {
public:
  mutable std::mutex mutex;
  snd_pcm_t *pcm = nullptr;
  AudioConfig config;
  Error error;
};

Speaker::Speaker() : impl_(std::make_unique<Impl>()) {}
Speaker::~Speaker() { close(); }
Speaker::Speaker(Speaker &&) noexcept = default;
Speaker &Speaker::operator=(Speaker &&) noexcept = default;

bool Speaker::open(const AudioConfig &config) {
  close();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!valid(config)) {
    impl_->error = {
        ErrorCode::InvalidArgument,
        "audio requires 8-48 kHz, 1-2 channels, and periodFrames >= 64"};
    return false;
  }
  int rc = snd_pcm_open(&impl_->pcm, "hw:1,0", SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    impl_->pcm = nullptr;
    impl_->error = {ErrorCode::BackendError,
                    std::string("cannot open speaker hw:1,0: ") +
                        snd_strerror(rc)};
    return false;
  }
  if (!configure(impl_->pcm, config, impl_->error)) {
    snd_pcm_close(impl_->pcm);
    impl_->pcm = nullptr;
    return false;
  }
  impl_->config = config;
  return true;
}

bool Speaker::write(const std::int16_t *samples, std::size_t frames) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->pcm || !samples || frames == 0) {
    impl_->error = {ErrorCode::InvalidArgument,
                    "speaker is closed or the source is invalid"};
    return false;
  }
  std::size_t done = 0;
  while (done < frames) {
    snd_pcm_sframes_t rc = snd_pcm_writei(
        impl_->pcm, samples + done * impl_->config.channels, frames - done);
    if (rc < 0)
      rc = snd_pcm_recover(impl_->pcm, static_cast<int>(rc), 1);
    if (rc < 0) {
      impl_->error = {ErrorCode::BackendError,
                      std::string("speaker write failed: ") + snd_strerror(rc)};
      return false;
    }
    done += static_cast<std::size_t>(rc);
  }
  impl_->error = {};
  return true;
}

bool Speaker::drain() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->pcm) {
    impl_->error = {ErrorCode::NotInitialized, "speaker is closed"};
    return false;
  }
  const int rc = snd_pcm_drain(impl_->pcm);
  if (rc < 0) {
    impl_->error = {ErrorCode::BackendError,
                    std::string("speaker drain failed: ") + snd_strerror(rc)};
    return false;
  }
  impl_->error = {};
  return true;
}

void Speaker::close() noexcept {
  if (!impl_)
    return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->pcm)
    snd_pcm_close(impl_->pcm);
  impl_->pcm = nullptr;
}
bool Speaker::isOpen() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->pcm;
}
AudioConfig Speaker::config() const noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->config;
}
Error Speaker::lastError() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->error;
}

} // namespace recamera
