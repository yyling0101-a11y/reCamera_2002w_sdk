#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <recamera/error.hpp>

namespace recamera {

struct AudioConfig {
  int sampleRate = 16000;
  int channels = 1;
  std::size_t periodFrames = 1024;
};

// Blocking signed 16-bit interleaved PCM capture from the on-board microphone.
class Microphone {
public:
  Microphone();
  ~Microphone();
  Microphone(Microphone &&) noexcept;
  Microphone &operator=(Microphone &&) noexcept;
  Microphone(const Microphone &) = delete;
  Microphone &operator=(const Microphone &) = delete;

  bool open(const AudioConfig &config = {});
  // samples must hold at least frames * channels elements.
  bool read(std::int16_t *samples, std::size_t frames);
  void close() noexcept;
  bool isOpen() const noexcept;
  AudioConfig config() const noexcept;
  Error lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Blocking signed 16-bit interleaved PCM playback through the speaker output.
class Speaker {
public:
  Speaker();
  ~Speaker();
  Speaker(Speaker &&) noexcept;
  Speaker &operator=(Speaker &&) noexcept;
  Speaker(const Speaker &) = delete;
  Speaker &operator=(const Speaker &) = delete;

  bool open(const AudioConfig &config = {});
  bool write(const std::int16_t *samples, std::size_t frames);
  bool drain();
  void close() noexcept;
  bool isOpen() const noexcept;
  AudioConfig config() const noexcept;
  Error lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace recamera
