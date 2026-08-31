#include <recamera/osd.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <utility>

extern "C" {
#include "cvi_comm_region.h"
#include "cvi_comm_sys.h"
#include "cvi_comm_video.h"
#include "cvi_region.h"
}

namespace recamera
{
namespace
{
constexpr std::uint16_t kTransparent = 0;
constexpr std::uint16_t kColors[] = {0x83e0, 0xfc00, 0x801f, 0xffe0, 0x83ff, 0xfc1f, 0xffff};

void pixel(std::vector<std::uint16_t> &image, int w, int h, int x, int y, std::uint16_t color)
{
    if (x >= 0 && x < w && y >= 0 && y < h) image[static_cast<std::size_t>(y) * w + x] = color;
}

void line(std::vector<std::uint16_t> &image, int w, int h, int x0, int y0, int x1, int y1,
          std::uint16_t color, int thickness)
{
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        for (int yy = -thickness / 2; yy <= thickness / 2; ++yy)
            for (int xx = -thickness / 2; xx <= thickness / 2; ++xx)
                pixel(image, w, h, x0 + xx, y0 + yy, color);
        if (x0 == x1 && y0 == y1) break;
        const int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}
} // namespace

class Overlay::Impl
{
  public:
    mutable std::mutex mutex;
    OverlayConfig config;
    std::vector<std::uint16_t> bitmap;
    bool ready = false;
    Error error;
};

Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}
Overlay::~Overlay() { stop(); }
Overlay::Overlay(Overlay &&) noexcept = default;
Overlay &Overlay::operator=(Overlay &&) noexcept = default;

bool Overlay::start(const OverlayConfig &config)
{
    stop();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (config.width <= 0 || config.height <= 0 || config.videoChannel < 0 ||
        config.regionHandle < 0 || config.lineWidth <= 0) {
        impl_->error = {ErrorCode::InvalidArgument, "invalid overlay configuration"};
        return false;
    }
    impl_->config = config;
    impl_->bitmap.assign(static_cast<std::size_t>(config.width) * config.height, kTransparent);
    RGN_ATTR_S attr{};
    attr.enType = OVERLAY_RGN;
    attr.unAttr.stOverlay.enPixelFormat = PIXEL_FORMAT_ARGB_1555;
    attr.unAttr.stOverlay.stSize.u32Width = config.width;
    attr.unAttr.stOverlay.stSize.u32Height = config.height;
    attr.unAttr.stOverlay.u32CanvasNum = 1;
    CVI_S32 rc = CVI_RGN_Create(config.regionHandle, &attr);
    if (rc != CVI_SUCCESS) {
        impl_->error = {ErrorCode::BackendError, "CVI_RGN_Create failed (rc=" + std::to_string(rc) + ")"};
        return false;
    }
    MMF_CHN_S channel{};
    channel.enModId = CVI_ID_VPSS;
    channel.s32DevId = 0;
    channel.s32ChnId = config.videoChannel;
    RGN_CHN_ATTR_S channelAttr{};
    channelAttr.bShow = CVI_TRUE;
    channelAttr.enType = OVERLAY_RGN;
    channelAttr.unChnAttr.stOverlayChn.stPoint.s32X = 0;
    channelAttr.unChnAttr.stOverlayChn.stPoint.s32Y = 0;
    channelAttr.unChnAttr.stOverlayChn.u32Layer = config.layer;
    rc = CVI_RGN_AttachToChn(config.regionHandle, &channel, &channelAttr);
    if (rc != CVI_SUCCESS) {
        CVI_RGN_Destroy(config.regionHandle);
        impl_->error = {ErrorCode::BackendError, "CVI_RGN_AttachToChn failed (rc=" + std::to_string(rc) + ")"};
        return false;
    }
    impl_->ready = true;
    impl_->error = {};
    return true;
}

bool Overlay::update(const std::vector<DetectionBox> &boxes)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->ready) {
        impl_->error = {ErrorCode::NotInitialized, "overlay is not running"};
        return false;
    }
    const auto &cfg = impl_->config;
    std::fill(impl_->bitmap.begin(), impl_->bitmap.end(), kTransparent);
    for (const auto &box : boxes) {
        const int x1 = std::max(0, static_cast<int>((box.x - box.width * 0.5F) * cfg.width));
        const int y1 = std::max(0, static_cast<int>((box.y - box.height * 0.5F) * cfg.height));
        const int x2 = std::min(cfg.width - 1, static_cast<int>((box.x + box.width * 0.5F) * cfg.width));
        const int y2 = std::min(cfg.height - 1, static_cast<int>((box.y + box.height * 0.5F) * cfg.height));
        if (x2 <= x1 || y2 <= y1) continue;
        const auto color = kColors[static_cast<unsigned>(std::max(0, box.classId)) %
                                   (sizeof(kColors) / sizeof(kColors[0]))];
        line(impl_->bitmap, cfg.width, cfg.height, x1, y1, x2, y1, color, cfg.lineWidth);
        line(impl_->bitmap, cfg.width, cfg.height, x2, y1, x2, y2, color, cfg.lineWidth);
        line(impl_->bitmap, cfg.width, cfg.height, x2, y2, x1, y2, color, cfg.lineWidth);
        line(impl_->bitmap, cfg.width, cfg.height, x1, y2, x1, y1, color, cfg.lineWidth);
    }
    BITMAP_S bitmap{};
    bitmap.enPixelFormat = PIXEL_FORMAT_ARGB_1555;
    bitmap.u32Width = cfg.width;
    bitmap.u32Height = cfg.height;
    bitmap.pData = impl_->bitmap.data();
    const CVI_S32 rc = CVI_RGN_SetBitMap(cfg.regionHandle, &bitmap);
    if (rc != CVI_SUCCESS) {
        impl_->error = {ErrorCode::BackendError, "CVI_RGN_SetBitMap failed (rc=" + std::to_string(rc) + ")"};
        return false;
    }
    impl_->error = {};
    return true;
}

void Overlay::clear() { update({}); }

void Overlay::stop() noexcept
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->ready) return;
    MMF_CHN_S channel{};
    channel.enModId = CVI_ID_VPSS;
    channel.s32DevId = 0;
    channel.s32ChnId = impl_->config.videoChannel;
    CVI_RGN_DetachFromChn(impl_->config.regionHandle, &channel);
    CVI_RGN_Destroy(impl_->config.regionHandle);
    impl_->bitmap.clear();
    impl_->ready = false;
}

bool Overlay::running() const noexcept { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->ready; }
Error Overlay::lastError() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->error; }

} // namespace recamera
