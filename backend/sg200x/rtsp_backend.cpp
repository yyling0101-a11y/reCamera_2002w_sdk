#include "backend/sg200x/rtsp_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

extern "C" {
#include "app_ipcam_venc.h"
#include <cvi_rtsp/rtsp.h>
}

namespace recamera::sg200x {
RtspBackend::RtspBackend(int vencChannel) : vencChannel_(vencChannel) {}

RtspBackend::~RtspBackend() { stop(); }

bool RtspBackend::normalizePath(const std::string& path, std::string& sessionName) {
    if (path.size() < 2 || path.front() != '/') return false;
    sessionName = path.substr(1);
    return !sessionName.empty() && sessionName.size() < 128 && sessionName.find('/') == std::string::npos;
}

bool RtspBackend::start(const RtspConfig& config, Error& error) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    if (running_) {
        error = {ErrorCode::AlreadyRunning, "RTSP is already running"};
        return false;
    }
    if (config.port == 0 || config.gop <= 0 || config.slowWriteThresholdMs <= 0.0 ||
        config.errorsBeforeIdr == 0 || config.errorsBeforeRestart < config.errorsBeforeIdr ||
        (config.authenticationEnabled && (config.username.empty() || config.password.empty())) ||
        config.username.size() >= sizeof(CVI_RTSP_CONFIG{}.username) ||
        config.password.size() >= sizeof(CVI_RTSP_CONFIG{}.password) ||
        config.realm.size() >= sizeof(CVI_RTSP_CONFIG{}.authRealm) ||
        !normalizePath(config.path, sessionName_)) {
        error = {ErrorCode::InvalidArgument, "invalid RTSP configuration or authentication credentials"};
        return false;
    }
    config_ = config;
    consecutiveHealthEvents_ = 0;
    if (!createServerLocked(error)) return false;
    error = {};
    return true;
}

bool RtspBackend::createServerLocked(Error& error) {
    CVI_RTSP_CONFIG serverConfig{};
    serverConfig.port = config_.port;
    serverConfig.authEnabled = config_.authenticationEnabled ? 1 : 0;
    if (config_.authenticationEnabled) {
        std::snprintf(serverConfig.authRealm, sizeof(serverConfig.authRealm), "%s", config_.realm.c_str());
        std::snprintf(serverConfig.username, sizeof(serverConfig.username), "%s", config_.username.c_str());
        std::snprintf(serverConfig.password, sizeof(serverConfig.password), "%s", config_.password.c_str());
    }

    CVI_RTSP_CTX* newServer = nullptr;
    int rc = CVI_RTSP_Create(&newServer, &serverConfig);
    if (rc != CVI_SUCCESS || newServer == nullptr) {
        error = {ErrorCode::BackendError, "CVI_RTSP_Create failed"};
        return false;
    }
    server_ = newServer;

    rc = CVI_RTSP_Start(newServer);
    if (rc != CVI_SUCCESS) {
        error = {ErrorCode::BackendError, "CVI_RTSP_Start failed"};
        destroyServerLocked();
        return false;
    }

    CVI_RTSP_SESSION_ATTR attr{};
    std::snprintf(attr.name, sizeof(attr.name), "%s", sessionName_.c_str());
    attr.reuseFirstSource = 1;
    attr.video.bitrate = 30720;
    attr.video.codec = RTSP_VIDEO_H264;

    CVI_RTSP_SESSION* newSession = nullptr;
    rc = CVI_RTSP_CreateSession(newServer, &attr, &newSession);
    if (rc != CVI_SUCCESS || newSession == nullptr) {
        error = {ErrorCode::BackendError, "CVI_RTSP_CreateSession failed"};
        destroyServerLocked();
        return false;
    }
    session_ = newSession;

    listener_ = {};
    listener_.onConnect = &RtspBackend::onConnect;
    listener_.argConn = this;
    listener_.onDisconnect = &RtspBackend::onDisconnect;
    listener_.argDisconn = this;
    rc = CVI_RTSP_SetListener(newServer, &listener_);
    if (rc != CVI_SUCCESS) {
        error = {ErrorCode::BackendError, "CVI_RTSP_SetListener failed"};
        destroyServerLocked();
        return false;
    }

    running_ = true;
    {
        std::lock_guard<std::mutex> statusLock(statusMutex_);
        status_.running = true;
        status_.recoveryLevel = RecoveryLevel::Streaming;
    }
    std::fprintf(stderr, "[recamera][INFO] RTSP listening on rtsp://0.0.0.0:%u/%s\n",
                 config_.port, sessionName_.c_str());
    return true;
}

void RtspBackend::destroyServerLocked() noexcept {
    CVI_RTSP_CTX* ctx = server_;
    CVI_RTSP_SESSION* media = session_;
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.running = false;
    }
    if (ctx == nullptr) {
        session_ = nullptr;
        return;
    }
    CVI_RTSP_Stop(ctx);
    if (media != nullptr) CVI_RTSP_DestroySession(ctx, media);
    session_ = nullptr;
    CVI_RTSP_Destroy(&ctx);
    server_ = nullptr;
}

void RtspBackend::stop() noexcept {
    std::lock_guard<std::mutex> lock(operationMutex_);
    destroyServerLocked();
}

bool RtspBackend::running() const noexcept {
    std::lock_guard<std::mutex> lock(operationMutex_);
    return running_;
}

StreamStatus RtspBackend::status() const noexcept {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

bool RtspBackend::requestIdr(const char* reason) noexcept {
    const int rc = CVI_VENC_RequestIDR(vencChannel_, CVI_TRUE);
    if (rc != CVI_SUCCESS) {
        std::fprintf(stderr, "[recamera][WARN] request IDR failed (%s), CVI=%#x\n", reason, rc);
        return false;
    }
    std::lock_guard<std::mutex> lock(statusMutex_);
    ++status_.idrRequests;
    status_.recoveryLevel = RecoveryLevel::RequestingIdr;
    return true;
}

void RtspBackend::onConnect(const char* ip, void* arg) {
    auto* self = static_cast<RtspBackend*>(arg);
    if (self == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(self->statusMutex_);
        ++self->status_.clients;
        ++self->status_.totalConnections;
    }
    std::fprintf(stderr, "[recamera][INFO] RTSP client connected: %s\n", ip ? ip : "unknown");
    if (self->config_.requestIdrOnConnect) self->requestIdr("client connected");
}

void RtspBackend::onDisconnect(const char* ip, void* arg) {
    auto* self = static_cast<RtspBackend*>(arg);
    if (self == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(self->statusMutex_);
        self->status_.clients = std::max(0, self->status_.clients - 1);
        ++self->status_.totalDisconnections;
    }
    std::fprintf(stderr, "[recamera][INFO] RTSP client disconnected: %s\n", ip ? ip : "unknown");
}

bool RtspBackend::restartServerLocked() {
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.recoveryLevel = RecoveryLevel::Restarting;
    }
    destroyServerLocked();
    Error error;
    if (!createServerLocked(error)) {
        std::fprintf(stderr, "[recamera][ERROR] automatic RTSP restart failed: %s\n", error.message.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        ++status_.restarts;
    }
    consecutiveHealthEvents_ = 0;
    requestIdr("RTSP restarted");
    return true;
}

void RtspBackend::noteHealthEventLocked(bool writeError, bool slowWrite) {
    if (!writeError && !slowWrite) {
        consecutiveHealthEvents_ = 0;
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.recoveryLevel = RecoveryLevel::Streaming;
        return;
    }
    if (!config_.autoRecover) return;
    ++consecutiveHealthEvents_;
    if (consecutiveHealthEvents_ == config_.errorsBeforeIdr) {
        requestIdr(writeError ? "consecutive RTSP write errors" : "consecutive slow RTSP writes");
    } else if (consecutiveHealthEvents_ >= config_.errorsBeforeRestart) {
        restartServerLocked();
    }
}

int RtspBackend::writeEncodedFrame(void* rawStream, void*) {
    if (rawStream == nullptr) return CVI_FAILURE;
    auto* streamData = static_cast<VENC_STREAM_S*>(rawStream);
    if (streamData->u32PackCount == 0 || streamData->u32PackCount > CVI_RTSP_DATA_MAX_BLOCK) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        ++status_.writeErrors;
        return CVI_FAILURE;
    }

    CVI_RTSP_DATA data{};
    data.blockCnt = streamData->u32PackCount;
    for (CVI_U32 i = 0; i < streamData->u32PackCount; ++i) {
        const VENC_PACK_S& pack = streamData->pstPack[i];
        if (pack.pu8Addr == nullptr || pack.u32Len <= pack.u32Offset) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            ++status_.writeErrors;
            return CVI_FAILURE;
        }
        data.dataPtr[i] = pack.pu8Addr + pack.u32Offset;
        data.dataLen[i] = pack.u32Len - pack.u32Offset;
        if (i == 0) data.timestamp = pack.u64PTS;
    }

    return writeData(data);
}

bool RtspBackend::write(const EncodedFrame& frame, Error& error) {
    if (frame.format() != PixelFormat::H264 || frame.blocks().empty() ||
        frame.blocks().size() > CVI_RTSP_DATA_MAX_BLOCK) {
        error = {ErrorCode::InvalidArgument, "RTSP accepts 1..8 H.264 blocks"};
        return false;
    }
    CVI_RTSP_DATA data{};
    data.blockCnt = static_cast<CVI_U32>(frame.blocks().size());
    data.timestamp = frame.timestamp();
    for (std::size_t i = 0; i < frame.blocks().size(); ++i) {
        const auto& block = frame.blocks()[i];
        if (block.empty()) {
            error = {ErrorCode::InvalidArgument, "encoded frame contains an empty block"};
            return false;
        }
        data.dataPtr[i] = const_cast<CVI_U8*>(block.data());
        data.dataLen[i] = static_cast<CVI_U32>(block.size());
    }
    const int rc = writeData(data);
    if (rc != CVI_SUCCESS) {
        error = {ErrorCode::BackendError, "CVI_RTSP_WriteFrame failed"};
        return false;
    }
    error = {};
    return true;
}

int RtspBackend::writeData(CVI_RTSP_DATA& data) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (!running_ || server_ == nullptr || session_ == nullptr) return CVI_SUCCESS;
    const auto begin = std::chrono::steady_clock::now();
    const int rc = CVI_RTSP_WriteFrame(server_, session_->video, &data);
    const auto end = std::chrono::steady_clock::now();
    const double latency = std::chrono::duration<double, std::milli>(end - begin).count();
    const bool writeError = rc != CVI_SUCCESS;
    const bool slowWrite = latency > config_.slowWriteThresholdMs;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.lastWriteLatencyMs = latency;
        status_.maxWriteLatencyMs = std::max(status_.maxWriteLatencyMs, latency);
        status_.lastFrameTimestamp = data.timestamp;
        if (writeError) ++status_.writeErrors;
        else ++status_.framesSent;
        if (slowWrite) ++status_.slowWrites;
    }
    if (slowWrite) {
        std::fprintf(stderr, "[recamera][WARN] CVI_RTSP_WriteFrame took %.2f ms\n", latency);
    }
    if (writeError) {
        std::fprintf(stderr, "[recamera][ERROR] CVI_RTSP_WriteFrame failed, CVI=%#x\n", rc);
    }
    noteHealthEventLocked(writeError, slowWrite);
    return rc;
}

}  // namespace recamera::sg200x
