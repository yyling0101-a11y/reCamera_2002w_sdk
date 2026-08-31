#include <recamera/audio.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

int main() {
  constexpr int seconds = 3;
  recamera::AudioConfig config{16000, 1, 1024};
  std::vector<std::int16_t> samples(
      static_cast<std::size_t>(config.sampleRate) * seconds * config.channels);
  recamera::Microphone microphone;
  if (!microphone.open(config) ||
      !microphone.read(samples.data(), config.sampleRate * seconds)) {
    std::fprintf(stderr, "microphone: %s\n",
                 microphone.lastError().message.c_str());
    return 1;
  }
  microphone.close();
  std::int16_t peak = 0;
  long double sumSquares = 0;
  for (const auto sample : samples) {
    const auto magnitude = static_cast<std::int16_t>(
        std::min(std::abs(static_cast<int>(sample)), 32767));
    peak = std::max(peak, magnitude);
    sumSquares += static_cast<long double>(sample) * sample;
  }
  const auto rms = std::sqrt(sumSquares / samples.size());
  std::fprintf(
      stderr,
      "captured %d seconds (peak=%d, rms=%.1Lf); playing through speaker\n",
      seconds, static_cast<int>(peak), rms);
  recamera::Speaker speaker;
  if (!speaker.open(config) ||
      !speaker.write(samples.data(), config.sampleRate * seconds) ||
      !speaker.drain()) {
    std::fprintf(stderr, "speaker: %s\n", speaker.lastError().message.c_str());
    return 1;
  }
  return 0;
}
