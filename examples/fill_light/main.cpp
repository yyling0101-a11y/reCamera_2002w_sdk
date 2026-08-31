#include <recamera/light.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  recamera::FillLight light;
  if (!light.setBrightness(64)) {
    std::fprintf(stderr, "fill light: %s\n", light.lastError().message.c_str());
    return 1;
  }
  std::fprintf(stderr, "fill light brightness=%d\n", light.brightness());
  std::this_thread::sleep_for(std::chrono::seconds(2));
  if (!light.off())
    return 1;
  return 0;
}
