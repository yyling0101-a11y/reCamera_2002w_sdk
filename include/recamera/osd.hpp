#pragma once

#include <memory>
#include <vector>

#include <recamera/error.hpp>

namespace recamera
{

struct DetectionBox
{
    // Normalized center coordinates and size in [0, 1].
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float score = 0.0F;
    int classId = -1;
};

struct OverlayConfig
{
    int width = 1920;
    int height = 1080;
    int videoChannel = 2;
    int regionHandle = 12;
    int layer = 7;
    int lineWidth = 4;
};

// Hardware ARGB1555 Region overlay. Attach after Camera::start() and detach
// before stopping the camera.
class Overlay
{
  public:
    Overlay();
    ~Overlay();
    Overlay(Overlay &&) noexcept;
    Overlay &operator=(Overlay &&) noexcept;
    Overlay(const Overlay &) = delete;
    Overlay &operator=(const Overlay &) = delete;

    bool start(const OverlayConfig &config = {});
    bool update(const std::vector<DetectionBox> &boxes);
    void clear();
    void stop() noexcept;
    bool running() const noexcept;
    Error lastError() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recamera
