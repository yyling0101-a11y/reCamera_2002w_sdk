#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>

#include <recamera/camera.hpp>
#include <recamera/webrtc.hpp>

namespace {
std::atomic<bool> running{true};
void stop(int) { running = false; }
}

int main() {
    using namespace std::chrono_literals;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    recamera::Camera camera;
    recamera::WebRtcStream webrtc;
    if (!camera.open({1920, 1080, 30, 2})) return 1;
    if (!webrtc.start({8080, "/live0", "0.0.0.0", "", 30, 2})) return 1;
    if (!camera.start()) return 1;

    std::fprintf(stderr, "Open http://192.168.42.1:8080/live0\n");
    while (running) {
        auto raw = camera.read(1s);
        if (!raw) continue;

        // Run AI or other processing while this zero-copy VPSS lease is held.
        volatile auto sample = raw->data()[0];
        (void)sample;
        raw.reset();

        auto encoded = camera.readEncoded(1s);
        if (encoded && !webrtc.write(*encoded)) {
            std::fprintf(stderr, "WebRTC write failed: %s\n", webrtc.lastError().message.c_str());
        }
    }
    return 0;
}
