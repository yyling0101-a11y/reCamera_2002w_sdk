#pragma once

#include <cstdint>
#include <recamera/error.hpp>

namespace recamera {

class FillLight {
public:
  bool setBrightness(std::uint8_t brightness);
  bool on() { return setBrightness(255); }
  bool off() { return setBrightness(0); }
  int brightness() const;
  Error lastError() const { return error_; }

private:
  mutable Error error_;
};

} // namespace recamera
