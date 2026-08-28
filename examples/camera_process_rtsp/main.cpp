#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>

#include <recamera/camera.hpp>
#include <recamera/rtsp.hpp>

namespace {
std::atomic<bool> running{true};
void stop(int) { running = false; }
}

int main() {
    using namespace std::chrono_literals;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    recamera::Camera camera;
    recamera::RtspStream rtsp;
    if (!camera.open({1920, 1080, 30, 2})) return 1;
    if (!rtsp.start({8554, "/live0", 60})) return 1;
    if (!camera.start()) return 1;

    std::uint64_t processed = 0;
    while (running) {
        auto raw = camera.read(1s);
        if (!raw) continue;

        // Replace this bounded sample with AI inference or other processing.
        // VideoFrame leases the VPSS buffer without copying. Release it promptly
        // after inference/processing so the capture channel can advance.
        volatile std::uint8_t sample = raw->data()[0];
        (void)sample;
        ++processed;
        raw.reset();

        auto encoded = camera.readEncoded(1s);
        if (encoded && !rtsp.write(*encoded)) {
            std::fprintf(stderr, "RTSP write failed: %s\n", rtsp.lastError().message.c_str());
            break;
        }
        if (processed % 50 == 0) {
            const auto status = rtsp.status();
            std::fprintf(stderr, "processed=%llu streamed=%llu errors=%llu clients=%d\n",
                         static_cast<unsigned long long>(processed),
                         static_cast<unsigned long long>(status.framesSent),
                         static_cast<unsigned long long>(status.writeErrors), status.clients);
        }
    }
    return 0;
}
