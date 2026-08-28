#pragma once

#include <functional>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

#include <recamera/camera.hpp>

namespace recamera::sg200x {

class CameraBackend {
public:
    using EncodedStreamHandler = std::function<int(void*, void*)>;

    bool open(const CameraConfig& config, int gop, Error& error);
    bool start(Error& error);
    bool setGop(int gop, Error& error);
    bool requestIdr(Error& error);
    void stop() noexcept;
    void close() noexcept;

    bool isOpen() const noexcept;
    bool isRunning() const noexcept;
    std::optional<VideoFrame> read(std::chrono::milliseconds timeout);
    std::optional<EncodedFrame> readEncoded(std::chrono::milliseconds timeout);

    void setRtspHandler(EncodedStreamHandler handler);
    void setFrameHandler(Camera::FrameHandler handler);
    int channel() const noexcept { return channel_; }

private:
    static int encodedThunk(void* data, void* args, void* userData);
    static int rawThunk(void* data, void* args, void* userData);
    int dispatchEncoded(void* data, void* args);
    int dispatchRaw(void* data);
    void pushRaw(VideoFrame frame);
    void pushEncoded(EncodedFrame frame);

    mutable std::mutex mutex_;
    CameraConfig config_;
    EncodedStreamHandler rtspHandler_;
    Camera::FrameHandler frameHandler_;
    std::condition_variable queueCv_;
    std::deque<VideoFrame> rawQueue_;
    std::deque<EncodedFrame> encodedQueue_;
    bool open_ = false;
    bool running_ = false;
    bool rawReadEnabled_ = false;
    bool encodedReadEnabled_ = false;
    int channel_ = 2;
    int rawChannel_ = 1;
};

}  // namespace recamera::sg200x
