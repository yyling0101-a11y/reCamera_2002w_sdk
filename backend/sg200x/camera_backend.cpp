#include "backend/sg200x/camera_backend.hpp"

#include <utility>
#include <cstring>
#include <condition_variable>
#include <memory>

extern "C" {
#include "app_ipcam_venc.h"
#include "cvi_sys.h"
#include "cvi_venc.h"
#include "video.h"
}

namespace recamera::sg200x {
namespace {

struct FrameLeaseState {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
};

void setError(Error& error, ErrorCode code, const char* message) {
    error = {code, message};
}

}  // namespace

bool CameraBackend::open(const CameraConfig& config, int gop, Error& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_) {
        setError(error, ErrorCode::AlreadyRunning, "camera is already open");
        return false;
    }
    if (config.width <= 0 || config.height <= 0 || config.fps <= 0 || config.fps > 120 ||
        config.queueDepth == 0 || config.queueDepth > 16 || gop <= 0) {
        setError(error, ErrorCode::InvalidArgument, "invalid width, height, fps, or GOP");
        return false;
    }

    int rc = initVideo();
    if (rc != 0) {
        setError(error, ErrorCode::BackendError, "initVideo failed");
        return false;
    }

    video_ch_param_t rawParam{};
    rawParam.format = VIDEO_FORMAT_NV21;
    rawParam.width = static_cast<uint32_t>(config.width);
    rawParam.height = static_cast<uint32_t>(config.height);
    rawParam.fps = static_cast<uint8_t>(config.fps);
    rc = setupVideo(static_cast<video_ch_index_t>(rawChannel_), &rawParam);
    if (rc != 0) {
        setError(error, ErrorCode::BackendError, "setupVideo raw channel failed");
        return false;
    }

    video_ch_param_t param{};
    param.format = VIDEO_FORMAT_H264;
    param.width = static_cast<uint32_t>(config.width);
    param.height = static_cast<uint32_t>(config.height);
    param.fps = static_cast<uint8_t>(config.fps);
    rc = setupVideo(static_cast<video_ch_index_t>(channel_), &param);
    if (rc != 0) {
        setError(error, ErrorCode::BackendError, "setupVideo failed");
        return false;
    }

    APP_PARAM_VENC_CTX_S* venc = app_ipcam_Venc_Param_Get();
    if (venc == nullptr || channel_ >= venc->s32VencChnCnt) {
        setError(error, ErrorCode::BackendError, "VENC channel configuration is unavailable");
        return false;
    }
    venc->astVencChnCfg[channel_].u32Gop = static_cast<CVI_U32>(gop);

    rc = registerVideoFrameHandler(static_cast<video_ch_index_t>(channel_), 0, &CameraBackend::encodedThunk, this);
    if (rc != 0) {
        setError(error, ErrorCode::BackendError, "registerVideoFrameHandler failed");
        return false;
    }
    rc = registerVideoFrameHandler(static_cast<video_ch_index_t>(rawChannel_), 0, &CameraBackend::rawThunk, this);
    if (rc != 0) {
        registerVideoFrameHandler(static_cast<video_ch_index_t>(channel_), 0, nullptr, nullptr);
        setError(error, ErrorCode::BackendError, "register raw frame handler failed");
        return false;
    }

    config_ = config;
    open_ = true;
    error = {};
    return true;
}

bool CameraBackend::start(Error& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
        setError(error, ErrorCode::NotInitialized, "camera is not open");
        return false;
    }
    if (running_) {
        setError(error, ErrorCode::AlreadyRunning, "camera is already running");
        return false;
    }
    const int rc = startVideo();
    if (rc != 0) {
        setError(error, ErrorCode::BackendError, "startVideo failed");
        return false;
    }
    running_ = true;
    error = {};
    return true;
}

bool CameraBackend::setGop(int gop, Error& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
        setError(error, ErrorCode::NotInitialized, "camera is not open");
        return false;
    }
    if (running_) {
        setError(error, ErrorCode::AlreadyRunning, "GOP cannot be changed while camera is running");
        return false;
    }
    if (gop <= 0) {
        setError(error, ErrorCode::InvalidArgument, "GOP must be positive");
        return false;
    }
    APP_PARAM_VENC_CTX_S* venc = app_ipcam_Venc_Param_Get();
    if (venc == nullptr || channel_ >= venc->s32VencChnCnt) {
        setError(error, ErrorCode::BackendError, "VENC channel configuration is unavailable");
        return false;
    }
    venc->astVencChnCfg[channel_].u32Gop = static_cast<CVI_U32>(gop);
    error = {};
    return true;
}

bool CameraBackend::requestIdr(Error& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        setError(error, ErrorCode::NotInitialized, "camera encoder is not running");
        return false;
    }
    const int rc = CVI_VENC_RequestIDR(channel_, CVI_TRUE);
    if (rc != CVI_SUCCESS) {
        setError(error, ErrorCode::BackendError, "CVI_VENC_RequestIDR failed");
        return false;
    }
    error = {};
    return true;
}

void CameraBackend::stop() noexcept {
    bool shouldStop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shouldStop = running_;
        running_ = false;
        rawQueue_.clear();
        encodedQueue_.clear();
        queueCv_.notify_all();
    }
    if (shouldStop) {
        deinitVideo();  // joins the VENC worker before returning
    }
}

void CameraBackend::close() noexcept {
    stop();
    registerVideoFrameHandler(static_cast<video_ch_index_t>(channel_), 0, nullptr, nullptr);
    registerVideoFrameHandler(static_cast<video_ch_index_t>(rawChannel_), 0, nullptr, nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    rtspHandler_ = {};
    frameHandler_ = {};
    rawQueue_.clear();
    encodedQueue_.clear();
    rawReadEnabled_ = false;
    encodedReadEnabled_ = false;
    open_ = false;
}

bool CameraBackend::isOpen() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

bool CameraBackend::isRunning() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void CameraBackend::setRtspHandler(EncodedStreamHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    rtspHandler_ = std::move(handler);
}

void CameraBackend::setFrameHandler(Camera::FrameHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameHandler_ = std::move(handler);
}

std::optional<VideoFrame> CameraBackend::read(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    rawReadEnabled_ = true;
    queueCv_.wait_for(lock, timeout, [this] { return !rawQueue_.empty() || !running_; });
    if (rawQueue_.empty()) return std::nullopt;
    VideoFrame frame = std::move(rawQueue_.front());
    rawQueue_.pop_front();
    return frame;
}

std::optional<EncodedFrame> CameraBackend::readEncoded(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    encodedReadEnabled_ = true;
    queueCv_.wait_for(lock, timeout, [this] { return !encodedQueue_.empty() || !running_; });
    if (encodedQueue_.empty()) return std::nullopt;
    EncodedFrame frame = std::move(encodedQueue_.front());
    encodedQueue_.pop_front();
    return frame;
}

int CameraBackend::encodedThunk(void* data, void* args, void* userData) {
    if (userData == nullptr) return CVI_FAILURE;
    return static_cast<CameraBackend*>(userData)->dispatchEncoded(data, args);
}

int CameraBackend::rawThunk(void* data, void*, void* userData) {
    if (userData == nullptr) return CVI_FAILURE;
    return static_cast<CameraBackend*>(userData)->dispatchRaw(data);
}

void CameraBackend::pushRaw(VideoFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (rawQueue_.size() >= config_.queueDepth) rawQueue_.pop_front();
    rawQueue_.push_back(std::move(frame));
    queueCv_.notify_all();
}

void CameraBackend::pushEncoded(EncodedFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (encodedQueue_.size() >= config_.queueDepth) encodedQueue_.pop_front();
    encodedQueue_.push_back(std::move(frame));
    queueCv_.notify_all();
}

int CameraBackend::dispatchEncoded(void* data, void* args) {
    EncodedStreamHandler rtsp;
    Camera::FrameHandler frames;
    CameraConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rtsp = rtspHandler_;
        frames = frameHandler_;
        config = config_;
    }

    int result = CVI_SUCCESS;
    if (rtsp) result = rtsp(data, args);

    if (data != nullptr) {
        auto* stream = static_cast<VENC_STREAM_S*>(data);
        std::vector<std::vector<std::uint8_t>> blocks;
        blocks.reserve(stream->u32PackCount);
        std::uint64_t timestamp = 0;
        for (CVI_U32 i = 0; i < stream->u32PackCount; ++i) {
            const VENC_PACK_S& pack = stream->pstPack[i];
            if (pack.pu8Addr == nullptr || pack.u32Len <= pack.u32Offset) continue;
            if (blocks.empty()) timestamp = pack.u64PTS;
            blocks.emplace_back(pack.pu8Addr + pack.u32Offset, pack.pu8Addr + pack.u32Len);
        }
        bool queueEncoded = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queueEncoded = encodedReadEnabled_ && running_;
        }
        if (queueEncoded && !blocks.empty()) {
            pushEncoded(EncodedFrame(PixelFormat::H264, timestamp, std::move(blocks)));
        }

        if (frames) for (CVI_U32 i = 0; i < stream->u32PackCount; ++i) {
            const VENC_PACK_S& pack = stream->pstPack[i];
            if (pack.pu8Addr == nullptr || pack.u32Len <= pack.u32Offset) continue;
            Frame frame;
            frame.data = pack.pu8Addr + pack.u32Offset;
            frame.size = pack.u32Len - pack.u32Offset;
            frame.width = config.width;
            frame.height = config.height;
            frame.format = PixelFormat::H264;
            frame.timestamp = pack.u64PTS;
            frames(frame);
        }
    }
    return result;
}

int CameraBackend::dispatchRaw(void* data) {
    if (data == nullptr) return CVI_FAILURE;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!rawReadEnabled_ || !running_) return CVI_SUCCESS;
    }
    auto* info = static_cast<VIDEO_FRAME_INFO_S*>(data);
    VIDEO_FRAME_S& source = info->stVFrame;
    std::size_t total = 0;
    for (int plane = 0; plane < 3; ++plane) total += source.u32Length[plane];
    if (total == 0) return CVI_FAILURE;

    std::vector<VideoFrame::Plane> planes;
    planes.reserve(3);
    for (int plane = 0; plane < 3; ++plane) {
        const CVI_U32 length = source.u32Length[plane];
        if (length == 0) continue;
        auto* mapped = static_cast<CVI_U8*>(CVI_SYS_Mmap(source.u64PhyAddr[plane], length));
        if (mapped == nullptr) {
            for (const auto& item : planes) CVI_SYS_Munmap(item.data, item.size);
            return CVI_FAILURE;
        }
        planes.push_back({mapped, length, source.u32Stride[plane]});
    }

    auto state = std::make_shared<FrameLeaseState>();
    std::shared_ptr<void> lease(state.get(), [state](void*) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->released = true;
        state->cv.notify_one();
    });
    pushRaw(VideoFrame(static_cast<int>(source.u32Width), static_cast<int>(source.u32Height),
                       PixelFormat::NV21, source.u64PTS, planes, std::move(lease)));

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&state] { return state->released; });
    }
    for (const auto& item : planes) CVI_SYS_Munmap(item.data, item.size);
    return CVI_SUCCESS;
}

}  // namespace recamera::sg200x
