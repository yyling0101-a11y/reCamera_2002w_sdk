#include <recamera/light.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace recamera {
namespace {
constexpr const char *kBrightness = "/sys/class/leds/white/brightness";
}

bool FillLight::setBrightness(std::uint8_t brightness) {
  const int fd = ::open(kBrightness, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    error_ = {ErrorCode::BackendError,
              std::string("cannot open fill light: ") + std::strerror(errno)};
    return false;
  }
  const std::string value = std::to_string(static_cast<unsigned>(brightness));
  const ssize_t written = ::write(fd, value.data(), value.size());
  const int saved = errno;
  ::close(fd);
  if (written != static_cast<ssize_t>(value.size())) {
    error_ = {ErrorCode::BackendError,
              std::string("cannot set fill light: ") + std::strerror(saved)};
    return false;
  }
  error_ = {};
  return true;
}

int FillLight::brightness() const {
  const int fd = ::open(kBrightness, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error_ = {ErrorCode::BackendError,
              std::string("cannot open fill light: ") + std::strerror(errno)};
    return -1;
  }
  char buffer[16]{};
  const ssize_t count = ::read(fd, buffer, sizeof(buffer) - 1);
  const int saved = errno;
  ::close(fd);
  if (count <= 0) {
    error_ = {ErrorCode::BackendError,
              std::string("cannot read fill light: ") + std::strerror(saved)};
    return -1;
  }
  char *end = nullptr;
  const long value = std::strtol(buffer, &end, 10);
  if (end == buffer || value < 0 || value > 255) {
    error_ = {ErrorCode::BackendError, "invalid fill light brightness"};
    return -1;
  }
  error_ = {};
  return static_cast<int>(value);
}

} // namespace recamera
