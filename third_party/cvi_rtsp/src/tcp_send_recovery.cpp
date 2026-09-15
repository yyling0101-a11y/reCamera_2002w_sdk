#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <sys/socket.h>
#include <unordered_map>

#include <cvi_rtsp/rtsp.h>

namespace {
std::mutex stateMutex;
struct FailureBurst {
    unsigned count = 0;
    std::chrono::steady_clock::time_point lastFailure;
};
std::unordered_map<int, FailureBurst> failureBursts;
unsigned disconnectThreshold = 8;
void (*errorCallback)(int, int, unsigned, int, void*) = nullptr;
void* errorCallbackArg = nullptr;
}

extern "C" ssize_t __real_send(int socket, const void* buffer, size_t length, int flags);

extern "C" ssize_t __wrap_send(int socket, const void* buffer, size_t length, int flags) {
    const ssize_t result = __real_send(socket, buffer, length, flags);
    if (result >= 0) {
        std::lock_guard<std::mutex> lock(stateMutex);
        const auto found = failureBursts.find(socket);
        if (found != failureBursts.end() &&
            std::chrono::steady_clock::now() - found->second.lastFailure >
                std::chrono::seconds(2)) {
            failureBursts.erase(found);
        }
        return result;
    }

    const int sendError = errno;
    if (sendError != EAGAIN && sendError != EWOULDBLOCK) return result;

    unsigned failures = 0;
    bool disconnect = false;
    void (*callback)(int, int, unsigned, int, void*) = nullptr;
    void* callbackArg = nullptr;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto& burst = failureBursts[socket];
        const auto now = std::chrono::steady_clock::now();
        if (burst.count != 0 && now - burst.lastFailure > std::chrono::seconds(2))
            burst.count = 0;
        burst.lastFailure = now;
        failures = ++burst.count;
        if (disconnectThreshold != 0 && failures >= disconnectThreshold) {
            failureBursts.erase(socket);
            disconnect = true;
        }
        callback = errorCallback;
        callbackArg = errorCallbackArg;
    }

    if (disconnect) {
        // shutdown() wakes live555's read side; live555 remains responsible for
        // closing the fd and deleting its RTSP client/session objects.
        shutdown(socket, SHUT_RDWR);
        std::fprintf(stderr,
                     "[recamera][WARN] closing stalled RTSP/TCP client fd=%d "
                     "after %u consecutive send failures\n",
                     socket, failures);
    }

    if (callback != nullptr) {
        callback(socket, sendError, failures, disconnect ? 1 : 0, callbackArg);
    }
    errno = sendError;
    return result;
}

extern "C" void CVI_RTSP_ConfigureTcpSendRecovery(CVI_RTSP_CTX* ctx) {
    if (ctx == nullptr) return;
    const unsigned configured = ctx->config.tcpSendFailuresBeforeDisconnect;
    std::lock_guard<std::mutex> lock(stateMutex);
    disconnectThreshold = configured == 0 ? 8 : configured;
    errorCallbackArg = ctx->config.tcpSendErrorArg;
    errorCallback = ctx->config.onTcpSendError;
}

extern "C" void CVI_RTSP_ClearTcpSendRecovery(CVI_RTSP_CTX* ctx) {
    if (ctx == nullptr) return;
    std::lock_guard<std::mutex> lock(stateMutex);
    errorCallback = nullptr;
    errorCallbackArg = nullptr;
    failureBursts.clear();
}
