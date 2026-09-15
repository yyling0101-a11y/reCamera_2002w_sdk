#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include <recamera/error.hpp>
#include <recamera/rtsp.hpp>

extern "C" {
#include <cvi_rtsp/rtsp.h>
}

namespace recamera::sg200x {

class RtspBackend {
public:
    explicit RtspBackend(int vencChannel);
    ~RtspBackend();

    bool start(const RtspConfig& config, Error& error);
    void stop() noexcept;
    bool running() const noexcept;
    StreamStatus status() const noexcept;
    bool write(const EncodedFrame& frame, Error& error);
    int writeEncodedFrame(void* stream, void* vendorContext);

private:
    static void onConnect(const char* ip, void* arg);
    static void onDisconnect(const char* ip, void* arg);
    static void onTcpSendError(int socket, int error, unsigned consecutive,
                               int disconnected, void* arg);

    bool createServerLocked(Error& error);
    void destroyServerLocked() noexcept;
    bool restartServerLocked();
    bool requestIdr(const char* reason) noexcept;
    void noteHealthEventLocked(bool writeError, bool slowWrite);
    int writeData(CVI_RTSP_DATA& data);
    static bool normalizePath(const std::string& path, std::string& sessionName);

    const int vencChannel_;
    mutable std::mutex operationMutex_;
    mutable std::mutex statusMutex_;
    RtspConfig config_;
    std::string sessionName_;
    CVI_RTSP_CTX* server_ = nullptr;
    CVI_RTSP_SESSION* session_ = nullptr;
    CVI_RTSP_STATE_LISTENER listener_{};
    bool running_ = false;
    unsigned consecutiveHealthEvents_ = 0;
    std::atomic<unsigned> pendingTcpSendErrors_{0};
    std::atomic<unsigned> pendingStalledDisconnects_{0};
    StreamStatus status_;
};

}  // namespace recamera::sg200x
