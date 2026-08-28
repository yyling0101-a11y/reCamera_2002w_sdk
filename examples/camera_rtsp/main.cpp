#include <atomic>
#include <csignal>
#include <cstdio>
#include <unistd.h>

#include <recamera/camera.hpp>

namespace {
std::atomic<bool> running{true};
void stop(int) { running = false; }
}

int main() {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    recamera::Camera camera;
    if (!camera.open({1920, 1080, 30})) return 1;
    if (!camera.startRtsp({8554, "/live0", 60})) return 1;
    if (!camera.start()) return 1;

    unsigned seconds = 0;
    while (running) {
        sleep(1);
        if (++seconds % 5 == 0) {
            const auto status = camera.streamStatus();
            std::fprintf(stderr,
                         "[recamera][STATUS] frames=%llu errors=%llu slow=%llu clients=%d "
                         "last_write_ms=%.3f max_write_ms=%.3f idr=%llu restarts=%llu\n",
                         static_cast<unsigned long long>(status.framesSent),
                         static_cast<unsigned long long>(status.writeErrors),
                         static_cast<unsigned long long>(status.slowWrites),
                         status.clients,
                         status.lastWriteLatencyMs,
                         status.maxWriteLatencyMs,
                         static_cast<unsigned long long>(status.idrRequests),
                         static_cast<unsigned long long>(status.restarts));
        }
    }
    return 0;
}
