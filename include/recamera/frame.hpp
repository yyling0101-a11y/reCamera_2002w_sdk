#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace recamera
{

enum class PixelFormat
{
    RGB888,
    NV21,
    H264,
    H265,
    JPEG
};

class VideoFrame
{
  public:
    struct Plane
    {
        std::uint8_t *data = nullptr;
        std::size_t size = 0;
        std::size_t stride = 0;
    };

    VideoFrame() = default;
    VideoFrame(int width, int height, PixelFormat format, std::uint64_t timestamp, std::vector<std::uint8_t> data,
               std::size_t stride = 0);
    VideoFrame(int width, int height, PixelFormat format, std::uint64_t timestamp, std::vector<Plane> planes,
               std::shared_ptr<void> lease);

    const std::uint8_t *data() const noexcept;
    std::uint8_t *data() noexcept;
    std::size_t size() const noexcept;
    // A leased frame may have non-contiguous planes. data() addresses plane 0;
    // use plane() for all NV21 planes. Do not retain a leased frame while
    // stopping or destroying its Camera.
    std::size_t planeCount() const noexcept;
    Plane plane(std::size_t index) const;
    int width() const noexcept
    {
        return width_;
    }
    int height() const noexcept
    {
        return height_;
    }
    std::size_t stride() const noexcept
    {
        return stride_;
    }
    PixelFormat format() const noexcept
    {
        return format_;
    }
    std::uint64_t timestamp() const noexcept
    {
        return timestamp_;
    }
    explicit operator bool() const noexcept
    {
        return !data_.empty() || !planes_.empty();
    }

  private:
    int width_ = 0;
    int height_ = 0;
    std::size_t stride_ = 0;
    PixelFormat format_ = PixelFormat::NV21;
    std::uint64_t timestamp_ = 0;
    std::vector<std::uint8_t> data_;
    std::vector<Plane> planes_;
    std::shared_ptr<void> lease_;
};

class EncodedFrame
{
  public:
    EncodedFrame() = default;
    EncodedFrame(PixelFormat format, std::uint64_t timestamp, std::vector<std::vector<std::uint8_t>> blocks);

    PixelFormat format() const noexcept
    {
        return format_;
    }
    std::uint64_t timestamp() const noexcept
    {
        return timestamp_;
    }
    const std::vector<std::vector<std::uint8_t>> &blocks() const noexcept
    {
        return blocks_;
    }
    std::size_t size() const noexcept;
    explicit operator bool() const noexcept
    {
        return !blocks_.empty();
    }

  private:
    PixelFormat format_ = PixelFormat::H264;
    std::uint64_t timestamp_ = 0;
    std::vector<std::vector<std::uint8_t>> blocks_;
};

// Compatibility view for callback-only code. Prefer VideoFrame or EncodedFrame.
struct Frame
{
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::H264;
    std::uint64_t timestamp = 0;
};

} // namespace recamera
