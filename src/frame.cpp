#include <recamera/frame.hpp>

#include <stdexcept>
#include <utility>

namespace recamera
{

VideoFrame::VideoFrame(int width, int height, PixelFormat format, std::uint64_t timestamp,
                       std::vector<std::uint8_t> data, std::size_t stride)
    : width_(width), height_(height), stride_(stride), format_(format), timestamp_(timestamp), data_(std::move(data))
{
}

VideoFrame::VideoFrame(int width, int height, PixelFormat format, std::uint64_t timestamp, std::vector<Plane> planes,
                       std::shared_ptr<void> lease)
    : width_(width), height_(height), stride_(planes.empty() ? 0 : planes.front().stride), format_(format),
      timestamp_(timestamp), planes_(std::move(planes)), lease_(std::move(lease))
{
}

const std::uint8_t *VideoFrame::data() const noexcept
{
    return planes_.empty() ? data_.data() : planes_.front().data;
}
std::uint8_t *VideoFrame::data() noexcept
{
    return planes_.empty() ? data_.data() : planes_.front().data;
}
std::size_t VideoFrame::size() const noexcept
{
    if (planes_.empty())
        return data_.size();
    std::size_t result = 0;
    for (const auto &item : planes_)
        result += item.size;
    return result;
}
std::size_t VideoFrame::planeCount() const noexcept
{
    return planes_.empty() ? (data_.empty() ? 0 : 1) : planes_.size();
}
VideoFrame::Plane VideoFrame::plane(std::size_t index) const
{
    if (!planes_.empty())
        return planes_.at(index);
    if (index != 0 || data_.empty())
        throw std::out_of_range("VideoFrame plane index");
    return {const_cast<std::uint8_t *>(data_.data()), data_.size(), stride_};
}

EncodedFrame::EncodedFrame(PixelFormat format, std::uint64_t timestamp, std::vector<std::vector<std::uint8_t>> blocks)
    : format_(format), timestamp_(timestamp), blocks_(std::move(blocks))
{
}

std::size_t EncodedFrame::size() const noexcept
{
    std::size_t result = 0;
    for (const auto &block : blocks_)
        result += block.size();
    return result;
}

} // namespace recamera
